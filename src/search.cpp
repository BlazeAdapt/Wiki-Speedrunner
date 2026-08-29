// search.cpp
//
// Loads the built graph (pages + forward/backward CSR) and answers
// shortest-hop queries between two article titles using classic
// bidirectional BFS (expand forward one layer, expand backward one layer,
// repeat, stop as soon as the two visited sets touch). This is optimal
// (shortest number of hops) because both sides advance depth-by-depth in
// lockstep.
//
// Usage:
//   ./search outdir/                     interactive mode
//   ./search outdir/ "Start" "Goal"       one-shot mode

#include "common.hpp"
#include "page_index.hpp"
#include <chrono>
#include <iostream>
#include <queue>

struct Csr {
    std::vector<uint32_t> offsets, targets;
    inline const uint32_t* begin(uint32_t node) const { return targets.data() + offsets[node]; }
    inline const uint32_t* end(uint32_t node) const { return targets.data() + offsets[node + 1]; }
};

static Csr load_csr(const std::string& dir, const std::string& prefix) {
    Csr c;
    { FILE* f = xfopen(dir + "/" + prefix + "_offsets.bin", "rb"); c.offsets = read_vec<uint32_t>(f); std::fclose(f); }
    { FILE* f = xfopen(dir + "/" + prefix + "_targets.bin", "rb"); c.targets = read_vec<uint32_t>(f); std::fclose(f); }
    return c;
}

// Returns the shortest path as a list of dense ids [start, ..., goal], or
// empty if unreachable.
static std::vector<uint32_t> bidirectional_bfs(uint32_t start, uint32_t goal, uint32_t N,
                                                const Csr& fwd, const Csr& bwd) {
    if (start == goal) return {start};

    std::vector<uint32_t> fwd_parent(N, NPOS32), bwd_parent(N, NPOS32);
    fwd_parent[start] = start;
    bwd_parent[goal] = goal;

    std::vector<uint32_t> fwd_frontier{start}, bwd_frontier{goal};
    uint32_t meet = NPOS32;

    auto expand = [&](std::vector<uint32_t>& frontier, std::vector<uint32_t>& own_parent,
                       const std::vector<uint32_t>& other_parent, const Csr& csr) -> uint32_t {
        std::vector<uint32_t> next;
        next.reserve(frontier.size() * 8);
        for (uint32_t u : frontier) {
            for (const uint32_t* p = csr.begin(u); p != csr.end(u); ++p) {
                uint32_t v = *p;
                if (own_parent[v] != NPOS32) continue; // already visited this side
                own_parent[v] = u;
                next.push_back(v);
                if (other_parent[v] != NPOS32) { frontier = std::move(next); return v; } // meeting point
            }
        }
        frontier = std::move(next);
        return NPOS32;
    };

    while (!fwd_frontier.empty() && !bwd_frontier.empty()) {
        meet = expand(fwd_frontier, fwd_parent, bwd_parent, fwd);
        if (meet != NPOS32) break;
        meet = expand(bwd_frontier, bwd_parent, fwd_parent, bwd);
        if (meet != NPOS32) break;
    }
    if (meet == NPOS32) return {};

    std::vector<uint32_t> left;
    for (uint32_t cur = meet;; ) {
        left.push_back(cur);
        if (cur == start) break;
        cur = fwd_parent[cur];
    }
    std::reverse(left.begin(), left.end());

    std::vector<uint32_t> right;
    for (uint32_t cur = meet; cur != goal; ) {
        cur = bwd_parent[cur];
        right.push_back(cur);
    }

    left.insert(left.end(), right.begin(), right.end());
    return left;
}

static void run_query(const PageIndex& idx, const Csr& fwd, const Csr& bwd,
                       const std::string& start_title_raw, const std::string& goal_title_raw) {
    std::string st = normalize_title(start_title_raw);
    std::string gt = normalize_title(goal_title_raw);
    uint32_t start = idx.find_by_title(st);
    uint32_t goal = idx.find_by_title(gt);
    if (start == NPOS32) { std::cout << "Article not found: \"" << start_title_raw << "\"\n"; return; }
    if (goal == NPOS32) { std::cout << "Article not found: \"" << goal_title_raw << "\"\n"; return; }

    if (idx.rec(start).is_redirect) {
        uint32_t r = idx.resolve_redirect(start);
        if (r != NPOS32) { std::cout << "(\"" << st << "\" is a redirect -> \"" << idx.title_of(r) << "\")\n"; start = r; }
    }
    if (idx.rec(goal).is_redirect) {
        uint32_t r = idx.resolve_redirect(goal);
        if (r != NPOS32) { std::cout << "(\"" << gt << "\" is a redirect -> \"" << idx.title_of(r) << "\")\n"; goal = r; }
    }

    auto t0 = std::chrono::steady_clock::now();
    auto path = bidirectional_bfs(start, goal, static_cast<uint32_t>(idx.size()), fwd, bwd);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    if (path.empty()) {
        std::cout << "No path found (" << ms << " ms)\n";
        return;
    }
    std::cout << path.size() - 1 << " hops (" << ms << " ms):\n";
    for (size_t i = 0; i < path.size(); i++) {
        std::cout << "  " << i << ". " << idx.title_of(path[i]) << "\n";
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s outdir/ [\"Start Title\" \"Goal Title\"]\n", argv[0]);
        return 1;
    }
    std::string dir = argv[1];

    std::cerr << "loading graph from " << dir << " ...\n";
    PageIndex idx(dir);
    Csr fwd = load_csr(dir, "fwd");
    Csr bwd = load_csr(dir, "bwd");
    std::cerr << "loaded " << idx.size() << " pages, " << fwd.targets.size() << " edges\n";

    if (argc >= 4) {
        run_query(idx, fwd, bwd, argv[2], argv[3]);
        return 0;
    }

    std::cout << "wiki-speedrun search. Enter two article titles per query (blank line to quit).\n";
    for (;;) {
        std::string start_title, goal_title;
        std::cout << "\nStart article: ";
        if (!std::getline(std::cin, start_title) || start_title.empty()) break;
        std::cout << "Goal article: ";
        if (!std::getline(std::cin, goal_title) || goal_title.empty()) break;
        run_query(idx, fwd, bwd, start_title, goal_title);
    }
    return 0;
}
