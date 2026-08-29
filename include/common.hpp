// common.hpp — shared structs / IO helpers for the wikispeedrun pipeline.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <stdexcept>

// ---- pages.bin -------------------------------------------------------
// One record per ns0 (article) page, in "dense id" order = array index.
// dense_id is what every other stage (linktargets, edges, CSR) uses.
struct PageRec {
    uint32_t page_id;      // original MediaWiki page_id
    uint32_t title_off;    // offset into titles.blob
    uint32_t title_len;    // length of title in titles.blob
    uint32_t redirect_to;  // dense_id of redirect target, or UINT32_MAX if not a redirect
                            // or unresolved
    uint8_t  is_redirect;  // 1 if page_is_redirect was set in the dump
    uint8_t  _pad[3] = {0,0,0};
};

static_assert(sizeof(PageRec) == 20, "PageRec must stay POD/fixed-size for binary IO");

constexpr uint32_t NPOS32 = 0xFFFFFFFFu;

inline FILE* xfopen(const std::string& path, const char* mode) {
    FILE* f = std::fopen(path.c_str(), mode);
    if (!f) throw std::runtime_error("cannot open " + path);
    return f;
}

template <typename T>
inline void write_vec(FILE* f, const std::vector<T>& v) {
    uint64_t n = v.size();
    std::fwrite(&n, sizeof(n), 1, f);
    if (n) std::fwrite(v.data(), sizeof(T), n, f);
}

template <typename T>
inline std::vector<T> read_vec(FILE* f) {
    uint64_t n = 0;
    if (std::fread(&n, sizeof(n), 1, f) != 1) throw std::runtime_error("read_vec: short read (count)");
    std::vector<T> v(n);
    if (n && std::fread(v.data(), sizeof(T), n, f) != n) throw std::runtime_error("read_vec: short read (data)");
    return v;
}

inline void write_blob(FILE* f, const std::string& blob) {
    uint64_t n = blob.size();
    std::fwrite(&n, sizeof(n), 1, f);
    if (n) std::fwrite(blob.data(), 1, n, f);
}

inline std::string read_blob(FILE* f) {
    uint64_t n = 0;
    if (std::fread(&n, sizeof(n), 1, f) != 1) throw std::runtime_error("read_blob: short read (count)");
    std::string s(n, '\0');
    if (n && std::fread(&s[0], 1, n, f) != n) throw std::runtime_error("read_blob: short read (data)");
    return s;
}

// Normalizes a user-typed article title into MediaWiki's canonical form:
// spaces -> underscores, first letter uppercased (enwiki default capitalization).
inline std::string normalize_title(std::string t) {
    // trim
    size_t a = t.find_first_not_of(" \t\r\n");
    size_t b = t.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    t = t.substr(a, b - a + 1);
    for (auto& c : t) if (c == ' ') c = '_';
    if (!t.empty() && t[0] >= 'a' && t[0] <= 'z') t[0] = static_cast<char>(t[0] - 'a' + 'A');
    return t;
}
