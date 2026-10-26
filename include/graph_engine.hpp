// graph_engine.hpp — load the built graph once, answer shortest-path queries.
// Shared between src/search.cpp (CLI) and server/main.cpp (HTTP API), so the
// server can keep everything resident in memory across requests instead of
// re-loading per query.
#pragma once
#include "common.hpp"
#include "page_index.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
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
        std::string start_redirected_from, goal_redirected_from;
        std::vector<std::string> path;
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

    // Returns the shortest path followed by up to `limit - 1` distinct
    // loopless alternatives. The shortest path is passed in from query(), so
    // it is not recomputed. Yen's algorithm generates alternatives by finding
    // shortest spur paths while excluding edges/nodes that would reproduce an
    // already accepted route.
    std::vector<std::vector<std::string>> paths_with_alternatives(
        const std::string& start_raw, const std::string& goal_raw,
        const std::vector<std::string>& first_path, size_t limit) const {
        std::vector<std::vector<std::string>> output;
        if (first_path.empty() || limit == 0) return output;

        std::string ignored_start, ignored_goal;
        uint32_t start = resolve_title(start_raw, &ignored_start, nullptr);
        uint32_t goal = resolve_title(goal_raw, &ignored_goal, nullptr);
        if (start == NPOS32 || goal == NPOS32) return output;

        std::vector<uint32_t> first;
        first.reserve(first_path.size());
        for (const auto& title : first_path) {
            std::string canonical;
            uint32_t id = resolve_title(title, &canonical, nullptr);
            if (id == NPOS32) return output;
            first.push_back(id);
        }

        std::vector<std::vector<uint32_t>> accepted;
        accepted.push_back(std::move(first));
        std::vector<std::vector<uint32_t>> candidates;
        std::unordered_set<std::string> candidate_keys;
        candidate_keys.reserve(limit * 16);
        auto key_of = [](const std::vector<uint32_t>& path) {
            std::string key;
            key.reserve(path.size() * sizeof(uint32_t));
            for (uint32_t id : path)
                for (int b = 0; b < 4; ++b) key.push_back(static_cast<char>((id >> (b * 8)) & 0xff));
            return key;
        };

        while (accepted.size() < limit) {
            const auto& previous = accepted.back();
            for (size_t spur_index = 0; spur_index + 1 < previous.size(); ++spur_index) {
                std::vector<uint32_t> root(previous.begin(), previous.begin() + spur_index + 1);
                std::unordered_set<uint32_t> banned_nodes;
                banned_nodes.reserve(spur_index * 2 + 1);
                for (size_t i = 0; i < spur_index; ++i) banned_nodes.insert(root[i]);

                std::unordered_set<uint64_t> banned_edges;
                for (const auto& route : accepted) {
                    if (route.size() <= spur_index + 1) continue;
                    bool same_root = true;
                    for (size_t i = 0; i <= spur_index; ++i) {
                        if (route[i] != root[i]) { same_root = false; break; }
                    }
                    if (same_root) banned_edges.insert(edge_key(route[spur_index], route[spur_index + 1]));
                }

                auto spur = shortest_path_avoiding(root.back(), goal, banned_nodes, banned_edges);
                if (spur.empty()) continue;
                std::vector<uint32_t> candidate(root.begin(), root.end() - 1);
                candidate.insert(candidate.end(), spur.begin(), spur.end());
                const std::string key = key_of(candidate);
                if (candidate_keys.insert(key).second) candidates.push_back(std::move(candidate));
            }

            if (candidates.empty()) break;
            auto best = std::min_element(candidates.begin(), candidates.end(),
                [](const auto& a, const auto& b) {
                    if (a.size() != b.size()) return a.size() < b.size();
                    return a < b;
                });
            accepted.push_back(std::move(*best));
            candidates.erase(best);
        }

        output.reserve(accepted.size());
        for (const auto& route : accepted) {
            std::vector<std::string> titles;
            titles.reserve(route.size());
            for (uint32_t id : route) titles.push_back(idx_.title_of(id));
            output.push_back(std::move(titles));
        }
        return output;
    }

private:
    PageIndex idx_;
    Csr fwd_, bwd_;

    static uint64_t edge_key(uint32_t from, uint32_t to) {
        return (static_cast<uint64_t>(from) << 32) | static_cast<uint64_t>(to);
    }

    // Bidirectional BFS with per-query exclusions. Hash maps hold only visited
    // nodes, avoiding an O(number-of-pages) array initialization for every spur.
    std::vector<uint32_t> shortest_path_avoiding(
        uint32_t start, uint32_t goal,
        const std::unordered_set<uint32_t>& banned_nodes,
        const std::unordered_set<uint64_t>& banned_edges) const {
        if (start == goal) return {start};
        if (banned_nodes.count(start) || banned_nodes.count(goal)) return {};

        std::unordered_map<uint32_t, uint32_t> fwd_parent, bwd_parent;
        fwd_parent.reserve(65536);
        bwd_parent.reserve(65536);
        fwd_parent.emplace(start, start);
        bwd_parent.emplace(goal, goal);
        std::vector<uint32_t> fwd_frontier{start}, bwd_frontier{goal};
        uint32_t meet = NPOS32;

        auto expand = [&](std::vector<uint32_t>& frontier,
                          std::unordered_map<uint32_t, uint32_t>& own_parent,
                          const std::unordered_map<uint32_t, uint32_t>& other_parent,
                          const Csr& csr, bool forward) -> uint32_t {
            std::vector<uint32_t> next;
            for (uint32_t u : frontier) {
                for (const uint32_t* p = csr.begin(u); p != csr.end(u); ++p) {
                    uint32_t v = *p;
                    if (banned_nodes.count(v) || own_parent.count(v)) continue;
                    const uint64_t edge = forward ? edge_key(u, v) : edge_key(v, u);
                    if (banned_edges.count(edge)) continue;
                    own_parent.emplace(v, u);
                    next.push_back(v);
                    if (other_parent.count(v)) { frontier = std::move(next); return v; }
                }
            }
            frontier = std::move(next);
            return NPOS32;
        };

        while (!fwd_frontier.empty() && !bwd_frontier.empty()) {
            meet = expand(fwd_frontier, fwd_parent, bwd_parent, fwd_, true);
            if (meet != NPOS32) break;
            meet = expand(bwd_frontier, bwd_parent, fwd_parent, bwd_, false);
            if (meet != NPOS32) break;
        }
        if (meet == NPOS32) return {};

        std::vector<uint32_t> left;
        for (uint32_t cur = meet;;) {
            left.push_back(cur);
            if (cur == start) break;
            auto it = fwd_parent.find(cur);
            if (it == fwd_parent.end()) return {};
            cur = it->second;
        }
        std::reverse(left.begin(), left.end());
        for (uint32_t cur = meet; cur != goal;) {
            auto it = bwd_parent.find(cur);
            if (it == bwd_parent.end()) return {};
            cur = it->second;
            left.push_back(cur);
        }
        return left;
    }

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
