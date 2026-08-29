// build_pages.cpp
//
// Parses enwiki-latest-page.sql.gz and emits:
//   pages.bin   -- array<PageRec> in dense-id order (dense_id = array index)
//   titles.blob -- concatenated title bytes (PageRec.title_off/len index into this)
//   idx_by_id.bin    -- array<uint32> of dense_ids, sorted by page_id (for page_id -> dense_id lookup)
//   idx_by_title.bin -- array<uint32> of dense_ids, sorted by title    (for title -> dense_id lookup)
//
// Only namespace-0 (article) pages are kept. redirect_to is left as NPOS32
// here; it gets filled in by build_redirects.
//
// page table column order (verify against your dump's CREATE TABLE if this
// pipeline errors out — `zcat enwiki-latest-page.sql.gz | head -c 2000`):
//   0 page_id, 1 page_namespace, 2 page_title, 3 page_is_redirect, ...

#include "sql_parser.hpp"
#include "common.hpp"
#include <algorithm>
#include <iostream>
#include <numeric>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s enwiki-latest-page.sql.gz outdir/\n", argv[0]);
        return 1;
    }
    std::string in_path = argv[1];
    std::string out_dir = argv[2];

    std::vector<PageRec> pages;
    std::string titles_blob;
    pages.reserve(11'000'000);
    titles_blob.reserve(200'000'000);

    uint64_t total_rows = 0, kept = 0;

    SqlDumpParser parser(in_path);
    parser.for_each_tuple([&](const std::vector<Field>& f) {
        total_rows++;
        if (f.size() < 4) return;
        uint32_t ns = static_cast<uint32_t>(f[1].as_i64());
        if (ns != 0) return; // articles only

        PageRec rec;
        rec.page_id = static_cast<uint32_t>(f[0].as_u64());
        rec.title_off = static_cast<uint32_t>(titles_blob.size());
        rec.title_len = static_cast<uint32_t>(f[2].value.size());
        rec.is_redirect = static_cast<uint8_t>(f[3].as_i64() != 0);
        rec.redirect_to = NPOS32;

        titles_blob.append(f[2].value);
        pages.push_back(rec);
        kept++;

        if (kept % 2'000'000 == 0)
            std::cerr << "  ...processed " << kept << " ns0 pages (" << total_rows << " rows scanned)\n";
    });

    std::cerr << "page.sql.gz: scanned " << total_rows << " rows, kept " << kept << " ns0 pages\n";

    // idx_by_id: dense ids sorted by original page_id
    std::vector<uint32_t> idx_by_id(pages.size());
    std::iota(idx_by_id.begin(), idx_by_id.end(), 0u);
    std::sort(idx_by_id.begin(), idx_by_id.end(), [&](uint32_t a, uint32_t b) {
        return pages[a].page_id < pages[b].page_id;
    });

    // idx_by_title: dense ids sorted lexicographically by title bytes
    std::vector<uint32_t> idx_by_title(pages.size());
    std::iota(idx_by_title.begin(), idx_by_title.end(), 0u);
    std::sort(idx_by_title.begin(), idx_by_title.end(), [&](uint32_t a, uint32_t b) {
        const PageRec& pa = pages[a];
        const PageRec& pb = pages[b];
        int c = std::memcmp(titles_blob.data() + pa.title_off, titles_blob.data() + pb.title_off,
                             std::min(pa.title_len, pb.title_len));
        if (c != 0) return c < 0;
        return pa.title_len < pb.title_len;
    });

    {
        FILE* f = xfopen(out_dir + "/pages.bin", "wb");
        write_vec(f, pages);
        std::fclose(f);
    }
    {
        FILE* f = xfopen(out_dir + "/titles.blob", "wb");
        write_blob(f, titles_blob);
        std::fclose(f);
    }
    {
        FILE* f = xfopen(out_dir + "/idx_by_id.bin", "wb");
        write_vec(f, idx_by_id);
        std::fclose(f);
    }
    {
        FILE* f = xfopen(out_dir + "/idx_by_title.bin", "wb");
        write_vec(f, idx_by_title);
        std::fclose(f);
    }

    std::cerr << "wrote pages.bin (" << pages.size() << " records), titles.blob ("
              << titles_blob.size() << " bytes)\n";
    return 0;
}
