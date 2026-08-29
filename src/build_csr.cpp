// build_csr.cpp
//
// Reads edges.bin (uint64 edge_count, then edge_count * (uint32 from, uint32 to)
// pairs) and builds compressed-sparse-row adjacency in both directions:
//   fwd_offsets.bin / fwd_targets.bin  -- out-links, for forward BFS
//   bwd_offsets.bin / bwd_targets.bin  -- in-links,  for backward BFS
//
// offsets[i] has N+1 entries: node i's neighbors are targets[offsets[i] .. offsets[i+1]).

#include "common.hpp"
#include "page_index.hpp"
#include <iostream>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s outdir/\n", argv[0]);
        return 1;
    }
    std::string dir = argv[1];

    PageIndex idx(dir);
    uint32_t N = static_cast<uint32_t>(idx.size());
    std::cerr << "N (dense pages) = " << N << "\n";

    FILE* ef = xfopen(dir + "/edges.bin", "rb");
    uint64_t E = 0;
    if (std::fread(&E, sizeof(E), 1, ef) != 1) throw std::runtime_error("edges.bin: cannot read count");
    std::cerr << "E (edges) = " << E << "\n";

    std::vector<uint32_t> fwd_offsets(N + 1, 0), bwd_offsets(N + 1, 0);

    // ---- pass 1: count out-degree / in-degree ----
    {
        const size_t CHUNK = 1 << 20; // pairs per read
        std::vector<uint32_t> buf(CHUNK * 2);
        uint64_t remaining = E;
        uint64_t seen = 0;
        while (remaining > 0) {
            size_t n = static_cast<size_t>(std::min<uint64_t>(remaining, CHUNK));
            size_t got = std::fread(buf.data(), sizeof(uint32_t), n * 2, ef);
            if (got != n * 2) throw std::runtime_error("edges.bin: short read in count pass");
            for (size_t i = 0; i < n; i++) {
                uint32_t from = buf[2 * i], to = buf[2 * i + 1];
                fwd_offsets[from + 1]++;
                bwd_offsets[to + 1]++;
            }
            remaining -= n;
            seen += n;
            if (seen % 50'000'000 < CHUNK) std::cerr << "  ...counted " << seen << " / " << E << " edges\n";
        }
    }
    for (uint32_t i = 0; i < N; i++) {
        fwd_offsets[i + 1] += fwd_offsets[i];
        bwd_offsets[i + 1] += bwd_offsets[i];
    }
    if (fwd_offsets[N] != E || bwd_offsets[N] != E)
        throw std::runtime_error("CSR offset sum mismatch vs edge count (bad dense ids in edges.bin?)");

    std::vector<uint32_t> fwd_targets(E), bwd_targets(E);
    std::vector<uint32_t> fwd_cursor(fwd_offsets.begin(), fwd_offsets.end() - 1);
    std::vector<uint32_t> bwd_cursor(bwd_offsets.begin(), bwd_offsets.end() - 1);

    // ---- pass 2: place ----
    {
        std::fseek(ef, sizeof(uint64_t), SEEK_SET);
        const size_t CHUNK = 1 << 20;
        std::vector<uint32_t> buf(CHUNK * 2);
        uint64_t remaining = E;
        uint64_t seen = 0;
        while (remaining > 0) {
            size_t n = static_cast<size_t>(std::min<uint64_t>(remaining, CHUNK));
            size_t got = std::fread(buf.data(), sizeof(uint32_t), n * 2, ef);
            if (got != n * 2) throw std::runtime_error("edges.bin: short read in place pass");
            for (size_t i = 0; i < n; i++) {
                uint32_t from = buf[2 * i], to = buf[2 * i + 1];
                fwd_targets[fwd_cursor[from]++] = to;
                bwd_targets[bwd_cursor[to]++] = from;
            }
            remaining -= n;
            seen += n;
            if (seen % 50'000'000 < CHUNK) std::cerr << "  ...placed " << seen << " / " << E << " edges\n";
        }
    }
    std::fclose(ef);

    auto save = [&](const std::string& name, const std::vector<uint32_t>& v) {
        FILE* f = xfopen(dir + "/" + name, "wb");
        write_vec(f, v);
        std::fclose(f);
    };
    save("fwd_offsets.bin", fwd_offsets);
    save("fwd_targets.bin", fwd_targets);
    save("bwd_offsets.bin", bwd_offsets);
    save("bwd_targets.bin", bwd_targets);

    std::cerr << "wrote CSR graph: " << N << " nodes, " << E << " edges, both directions\n";
    return 0;
}
