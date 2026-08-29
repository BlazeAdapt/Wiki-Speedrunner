// build_redirects.cpp
//
// Parses enwiki-latest-redirect.sql.gz and fills in redirect_to on every
// PageRec flagged is_redirect in pages.bin, so later stages can collapse
// "A -> redirect -> B" into a direct "A -> B" edge.
//
// redirect table column order: rd_from, rd_namespace, rd_title, rd_interwiki, rd_fragment
//
// If you don't have enwiki-latest-redirect.sql.gz, skip this stage — the
// pipeline still works, it just won't collapse redirect hops (a redirect
// page becomes a dead-end / a slightly-off-topic node in the graph).

#include "sql_parser.hpp"
#include "page_index.hpp"
#include <iostream>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s enwiki-latest-redirect.sql.gz outdir/\n", argv[0]);
        return 1;
    }
    std::string in_path = argv[1];
    std::string out_dir = argv[2];

    PageIndex idx(out_dir);
    std::cerr << "loaded " << idx.size() << " pages\n";

    uint64_t total = 0, resolved = 0, skipped_ns = 0, not_found = 0;

    SqlDumpParser parser(in_path);
    parser.for_each_tuple([&](const std::vector<Field>& f) {
        total++;
        if (f.size() < 3) return;
        uint32_t from_page_id = static_cast<uint32_t>(f[0].as_u64());
        uint32_t rd_ns = static_cast<uint32_t>(f[1].as_i64());
        if (rd_ns != 0) { skipped_ns++; return; } // only handle redirects to articles

        uint32_t from_dense = idx.find_by_page_id(from_page_id);
        if (from_dense == NPOS32) return; // redirect source not an ns0 page we tracked

        std::string title = f[2].value; // already in canonical form in the dump
        uint32_t to_dense = idx.find_by_title(title);
        if (to_dense == NPOS32) { not_found++; return; }

        idx.mut_rec(from_dense).redirect_to = to_dense;
        resolved++;

        if (total % 2'000'000 == 0)
            std::cerr << "  ...processed " << total << " redirect rows\n";
    });

    std::cerr << "redirect.sql.gz: scanned " << total << " rows, resolved " << resolved
              << ", skipped (non-ns0 target) " << skipped_ns
              << ", target title not found " << not_found << "\n";

    idx.save_pages(out_dir);
    std::cerr << "updated pages.bin with redirect targets\n";
    return 0;
}
