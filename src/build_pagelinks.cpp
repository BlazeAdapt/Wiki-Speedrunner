// build_pagelinks.cpp
//
// Parses enwiki-latest-pagelinks.sql.gz and emits edges.bin: a flat stream
// of (dense_from uint32, dense_to uint32) pairs, one per article->article
// link, with:
//   - non-ns0 sources/targets dropped
//   - links FROM a redirect page dropped (not meaningful for the graph)
//   - links TO a redirect page collapsed through to the redirect's final
//     target (if build_redirects was run; otherwise such links are dropped,
//     since an unresolved redirect has no known destination)
//   - self-loops dropped
//
// pagelinks table column order (post-2021 link-target-normalization schema):
//   pl_from, pl_from_namespace, pl_target_id
// pl_target_id indexes into linktarget.sql.gz (see build_linktargets).

#include "sql_parser.hpp"
#include "page_index.hpp"
#include <iostream>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s enwiki-latest-pagelinks.sql.gz outdir/\n", argv[0]);
        return 1;
    }
    std::string in_path = argv[1];
    std::string out_dir = argv[2];

    PageIndex idx(out_dir);
    std::cerr << "loaded " << idx.size() << " pages\n";

    std::vector<uint32_t> lt_to_dense;
    {
        FILE* f = xfopen(out_dir + "/lt_to_dense.bin", "rb");
        lt_to_dense = read_vec<uint32_t>(f);
        std::fclose(f);
    }
    std::cerr << "loaded lt_to_dense (" << lt_to_dense.size() << " entries)\n";

    FILE* out = xfopen(out_dir + "/edges.bin", "wb");
    // Reserve 8 bytes at the front for the edge count, filled in at the end.
    uint64_t edge_count = 0;
    std::fwrite(&edge_count, sizeof(edge_count), 1, out);

    uint64_t total = 0, wrote = 0;
    std::vector<uint32_t> out_buf;
    out_buf.reserve(1 << 20);

    auto flush = [&]() {
        if (!out_buf.empty()) {
            std::fwrite(out_buf.data(), sizeof(uint32_t), out_buf.size(), out);
            out_buf.clear();
        }
    };

    SqlDumpParser parser(in_path);
    parser.for_each_tuple([&](const std::vector<Field>& f) {
        total++;
        if (f.size() < 3) return;
        int64_t from_ns = f[1].as_i64();
        if (from_ns != 0) return;

        uint32_t from_page_id = static_cast<uint32_t>(f[0].as_u64());
        uint32_t from_dense = idx.find_by_page_id(from_page_id);
        if (from_dense == NPOS32) return;
        if (idx.rec(from_dense).is_redirect) return; // links from redirects aren't useful

        uint64_t lt_id = f[2].as_u64();
        if (lt_id >= lt_to_dense.size()) return;
        uint32_t to_dense = lt_to_dense[lt_id];
        if (to_dense == NPOS32) return;

        uint32_t resolved = idx.resolve_redirect(to_dense);
        if (resolved == NPOS32) return;
        if (resolved == from_dense) return; // self-loop

        out_buf.push_back(from_dense);
        out_buf.push_back(resolved);
        wrote++;
        if (out_buf.size() >= (1 << 20)) flush();

        if (total % 10'000'000 == 0)
            std::cerr << "  ...processed " << total << " pagelinks rows, wrote " << wrote << " edges\n";
    });
    flush();

    edge_count = wrote;
    std::fseek(out, 0, SEEK_SET);
    std::fwrite(&edge_count, sizeof(edge_count), 1, out);
    std::fclose(out);

    std::cerr << "pagelinks.sql.gz: scanned " << total << " rows, wrote " << wrote << " edges to edges.bin\n";
    return 0;
}
