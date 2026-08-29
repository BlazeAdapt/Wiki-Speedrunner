// graph_engine.hpp — load the built graph once, answer shortest-path queries.
// Shared between src/search.cpp (CLI) and server/main.cpp (HTTP API), so the
// server can keep everything resident in memory across requests instead of
// re-loading per query.
#pragma once
#include "common.hpp"
#include "page_index.hpp"
#include <algorithm>
#include <chrono>
#include <vector>

struct Csr {
    std::vector<uint32_t> offsets, targets;
    inline const uint32_t* begin(uint32_t node) const { return targets.data() + offsets[node]; }
    inline const uint32_t* end(uint32_t node) const { return targets.data() + offsets[node + 1]; }
};

inline Csr load_csr(const std::string& dir, const std::string& prefix) {
    Csr c;
    { FILE* f = xfopen(dir + "/" + prefix + "_offsets.bin", "rb"); c.offsets = read_vec<uint32_t>(f); std::fclose(f); }
    { FILE* f = xfopen(dir + "/" + prefix + "_targets.bin", "rb"); c.targets = read_vec<uint32_t>(f); std::fclose(f); }
    return c;
}

// Everything needed to answer queries, loaded once and kept in RAM.
class GraphEngine {
public:
    explicit GraphEngine(const std::string& dir)
        : idx_(dir), fwd_(load_csr(dir, "fwd")), bwd_(load_csr(dir, "bwd")) {}

    size_t num_pages() const { return idx_.size(); }
    size_t num_edges() const { return fwd_.targets.size(); }

    // Resolves a raw user-typed title to a dense id, following redirects.
    // On success, *out_redirected_from is set to the pre-redirect title iff
    // the input actually was a redirect (empty otherwise). Returns NPOS32 if
    // the title doesn't exist.
    uint32_t resolve_title(const std::string& raw, std::string* out_canonical,
                            std::string* out_redirected_from) const {
        std::string norm = normalize_title(raw);
        uint32_t d = idx_.find_by_title(norm);
        if (d == NPOS32) return NPOS32;
        if (idx_.rec(d).is_redirect) {
            uint32_t r = idx_.resolve_redirect(d);
            if (r != NPOS32) {
                if (out_redirected_from) *out_redirected_from = norm;
                d = r;
            }
        }
        if (out_canonical) *out_canonical = idx_.title_of(d);
        return d;
    }

    std::string title_of(uint32_t dense) const { return idx_.title_of(dense); }

    // Up to `limit` titles starting with `prefix` (case-sensitive, canonical
    // MediaWiki form), for autocomplete.
    std::vector<std::string> suggest(const std::string& raw_prefix, int limit) const {
        std::string prefix = normalize_title(raw_prefix);
        return idx_.titles_with_prefix(prefix, limit);
    }

    struct Result {
        bool start_found = false, goal_found = false;
        bool reachable = false;
        std::string start_canonical, goal_canonical;
        std::string start_redirected_from, goal_redirected_from; // empty if not a redirect
        std::vector<std::string> path; // titles, start..goal inclusive
        double ms = 0;
    };

    Result query(const std::string& start_raw, const std::string& goal_raw) const {
        Result res;
        uint32_t start = resolve_title(start_raw, &res.start_canonical, &res.start_redirected_from);
        uint32_t goal = resolve_title(goal_raw, &res.goal_canonical, &res.goal_redirected_from);
        res.start_found = (start != NPOS32);
        res.goal_found = (goal != NPOS32);
        if (!res.start_found || !res.goal_found) return res;

        auto t0 = std::chrono::steady_clock::now();
        auto path = bidirectional_bfs(start, goal);
        auto t1 = std::chrono::steady_clock::now();
        res.ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        if (path.empty()) return res;
        res.reachable = true;
        res.path.reserve(path.size());
        for (uint32_t d : path) res.path.push_back(idx_.title_of(d));
        return res;
    }

private:
    PageIndex idx_;
    Csr fwd_, bwd_;

    std::vector<uint32_t> bidirectional_bfs(uint32_t start, uint32_t goal) const {
        uint32_t N = static_cast<uint32_t>(idx_.size());
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
                    if (own_parent[v] != NPOS32) continue;
                    own_parent[v] = u;
                    next.push_back(v);
                    if (other_parent[v] != NPOS32) { frontier = std::move(next); return v; }
                }
            }
            frontier = std::move(next);
            return NPOS32;
        };

        while (!fwd_frontier.empty() && !bwd_frontier.empty()) {
            meet = expand(fwd_frontier, fwd_parent, bwd_parent, fwd_);
            if (meet != NPOS32) break;
            meet = expand(bwd_frontier, bwd_parent, fwd_parent, bwd_);
            if (meet != NPOS32) break;
        }
        if (meet == NPOS32) return {};

        std::vector<uint32_t> left;
        for (uint32_t cur = meet;;) {
            left.push_back(cur);
            if (cur == start) break;
            cur = fwd_parent[cur];
        }
        std::reverse(left.begin(), left.end());
        std::vector<uint32_t> right;
        for (uint32_t cur = meet; cur != goal;) {
            cur = bwd_parent[cur];
            right.push_back(cur);
        }
        left.insert(left.end(), right.begin(), right.end());
        return left;
    }
};
