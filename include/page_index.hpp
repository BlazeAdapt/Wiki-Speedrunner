// page_index.hpp — loads the output of build_pages and provides lookups.
#pragma once
#include "common.hpp"
#include <algorithm>
#include <string>
#include <cstring>

class PageIndex {
public:
    explicit PageIndex(const std::string& dir) {
        { FILE* f = xfopen(dir + "/pages.bin", "rb"); pages_ = read_vec<PageRec>(f); std::fclose(f); }
        { FILE* f = xfopen(dir + "/titles.blob", "rb"); titles_ = read_blob(f); std::fclose(f); }
        { FILE* f = xfopen(dir + "/idx_by_id.bin", "rb"); idx_by_id_ = read_vec<uint32_t>(f); std::fclose(f); }
        { FILE* f = xfopen(dir + "/idx_by_title.bin", "rb"); idx_by_title_ = read_vec<uint32_t>(f); std::fclose(f); }
    }

    size_t size() const { return pages_.size(); }
    const PageRec& rec(uint32_t dense_id) const { return pages_[dense_id]; }
    PageRec& mut_rec(uint32_t dense_id) { return pages_[dense_id]; }

    std::string title_of(uint32_t dense_id) const {
        const PageRec& r = pages_[dense_id];
        return titles_.substr(r.title_off, r.title_len);
    }

    // Returns dense_id or NPOS32 if not found.
    uint32_t find_by_page_id(uint32_t page_id) const {
        auto it = std::lower_bound(idx_by_id_.begin(), idx_by_id_.end(), page_id,
            [&](uint32_t dense, uint32_t pid) { return pages_[dense].page_id < pid; });
        if (it == idx_by_id_.end() || pages_[*it].page_id != page_id) return NPOS32;
        return *it;
    }

    // title must already be in canonical MediaWiki form (underscores, capitalized).
    uint32_t find_by_title(const std::string& title) const {
        auto cmp_less = [&](uint32_t dense, const std::string& t) {
            const PageRec& r = pages_[dense];
            int c = std::memcmp(titles_.data() + r.title_off, t.data(), std::min<size_t>(r.title_len, t.size()));
            if (c != 0) return c < 0;
            return r.title_len < t.size();
        };
        auto it = std::lower_bound(idx_by_title_.begin(), idx_by_title_.end(), title, cmp_less);
        if (it == idx_by_title_.end()) return NPOS32;
        const PageRec& r = pages_[*it];
        if (r.title_len != title.size() || std::memcmp(titles_.data() + r.title_off, title.data(), title.size()) != 0)
            return NPOS32;
        return *it;
    }

    void save_pages(const std::string& dir) const {
        FILE* f = xfopen(dir + "/pages.bin", "wb");
        write_vec(f, pages_);
        std::fclose(f);
    }

    // Follows redirect_to chains (with a hop cap to guard against cycles),
    // returns the final non-redirect dense_id, or NPOS32 if it dead-ends /
    // cycles / is unresolved.
    uint32_t resolve_redirect(uint32_t dense_id, int max_hops = 5) const {
        uint32_t cur = dense_id;
        for (int i = 0; i < max_hops; i++) {
            const PageRec& r = pages_[cur];
            if (!r.is_redirect) return cur;
            if (r.redirect_to == NPOS32) return NPOS32;
            cur = r.redirect_to;
        }
        return NPOS32; // too many hops -> treat as broken
    }

private:
    std::vector<PageRec> pages_;
    std::string titles_;
    std::vector<uint32_t> idx_by_id_;
    std::vector<uint32_t> idx_by_title_;
};
