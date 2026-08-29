// build_linktargets.cpp
//
// Parses enwiki-latest-linktarget.sql.gz and emits lt_to_dense.bin: an
// array indexed by lt_id, giving the dense page id it refers to (or NPOS32
// if it's not an ns0 page we track, e.g. links to categories/templates/
// nonexistent pages).
//
// linktarget table column order: lt_id, lt_namespace, lt_title

#include "sql_parser.hpp"
#include "page_index.hpp"
#include <iostream>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s enwiki-latest-linktarget.sql.gz outdir/\n", argv[0]);
        return 1;
    }
    std::string in_path = argv[1];
    std::string out_dir = argv[2];

    PageIndex idx(out_dir);
    std::cerr << "loaded " << idx.size() << " pages\n";

    std::vector<std::pair<uint32_t, uint32_t>> pairs; // (lt_id, dense_id)
    pairs.reserve(60'000'000);

    uint64_t total = 0, ns0 = 0, resolved = 0;
    uint32_t max_lt_id = 0;

    SqlDumpParser parser(in_path);
    parser.for_each_tuple([&](const std::vector<Field>& f) {
        total++;
        if (f.size() < 3) return;
        uint32_t lt_id = static_cast<uint32_t>(f[0].as_u64());
        if (lt_id > max_lt_id) max_lt_id = lt_id;
        int64_t ns = f[1].as_i64();
        if (ns != 0) return; // only care about links that could target an article
        ns0++;

        uint32_t dense = idx.find_by_title(f[2].value);
        if (dense == NPOS32) return;
        pairs.emplace_back(lt_id, dense);
        resolved++;

        if (total % 5'000'000 == 0)
            std::cerr << "  ...processed " << total << " linktarget rows\n";
    });

    std::cerr << "linktarget.sql.gz: scanned " << total << " rows, ns0 " << ns0
              << ", resolved to a tracked page " << resolved << ", max lt_id " << max_lt_id << "\n";

    std::vector<uint32_t> lt_to_dense(static_cast<size_t>(max_lt_id) + 1, NPOS32);
    for (auto& pr : pairs) lt_to_dense[pr.first] = pr.second;

    FILE* f = xfopen(out_dir + "/lt_to_dense.bin", "wb");
    write_vec(f, lt_to_dense);
    std::fclose(f);

    std::cerr << "wrote lt_to_dense.bin (" << lt_to_dense.size() << " entries, "
              << (lt_to_dense.size() * 4 / (1024 * 1024)) << " MB)\n";
    return 0;
}
