// sql_parser.hpp
//
// Minimal streaming parser for mysqldump-style .sql.gz files, tuned for
// MediaWiki data dumps (page.sql.gz, linktarget.sql.gz, pagelinks.sql.gz,
// redirect.sql.gz, ...).
//
// It does NOT build a general SQL AST. It only understands:
//   INSERT INTO `table` VALUES (f1,f2,...),(f1,f2,...),...;
// which is exactly what these dumps contain, one giant statement (or a
// series of them) per file. Strings are unescaped (backslash-escaping,
// the mysqldump default). NULL is recognized as a null field.
//
// Usage:
//   SqlDumpParser p("enwiki-latest-page.sql.gz");
//   p.for_each_tuple([&](const std::vector<Field>& fields) {
//       // fields[0], fields[1], ... in column order
//   });

#pragma once
#include <zlib.h>
#include <string>
#include <vector>
#include <stdexcept>
#include <cstdint>
#include <cstring>
#include <functional>

struct Field {
    bool is_null = false;
    std::string value; // raw text for numbers, unescaped text for strings

    int64_t as_i64() const {
        if (is_null || value.empty()) return 0;
        return std::strtoll(value.c_str(), nullptr, 10);
    }
    uint64_t as_u64() const {
        if (is_null || value.empty()) return 0;
        return std::strtoull(value.c_str(), nullptr, 10);
    }
};

class GzStream {
public:
    explicit GzStream(const std::string& path, size_t bufsize = 1 << 20)
        : buf_(bufsize) {
        f_ = gzopen(path.c_str(), "rb");
        if (!f_) throw std::runtime_error("cannot open " + path);
        gzbuffer(f_, 1 << 20); // larger internal zlib buffer -> fewer syscalls
    }
    ~GzStream() { if (f_) gzclose(f_); }
    GzStream(const GzStream&) = delete;
    GzStream& operator=(const GzStream&) = delete;

    // returns -1 at EOF
    inline int get() {
        if (pos_ >= len_ && !refill()) return -1;
        return static_cast<unsigned char>(buf_[pos_++]);
    }
    inline int peek() {
        if (pos_ >= len_ && !refill()) return -1;
        return static_cast<unsigned char>(buf_[pos_]);
    }

private:
    bool refill() {
        len_ = gzread(f_, buf_.data(), static_cast<unsigned>(buf_.size()));
        pos_ = 0;
        return len_ > 0;
    }
    gzFile f_ = nullptr;
    std::vector<char> buf_;
    int pos_ = 0;
    int len_ = 0;
};

class SqlDumpParser {
public:
    explicit SqlDumpParser(const std::string& path) : in_(path) {}

    // Calls cb(fields) once per row-tuple found in the file, across every
    // "INSERT INTO ... VALUES (...);" statement present.
    void for_each_tuple(const std::function<void(const std::vector<Field>&)>& cb) {
        std::vector<Field> fields;
        if (!skip_to_values_start()) return; // no INSERT statements at all
        for (;;) {
            fields.clear();
            parse_tuple_fields(fields);
            cb(fields);

            int c = in_.get();
            if (c == ',') {
                int paren = in_.get();
                if (paren != '(') {
                    // malformed; try to resync by searching for next tuple/stmt
                    if (!resync()) return;
                }
                continue; // next tuple in same statement
            } else if (c == ';') {
                if (!skip_to_values_start()) return; // move to next INSERT, or EOF
                continue;
            } else if (c == -1) {
                return;
            } else {
                // whitespace/newline between tuples; keep scanning for ',' or ';'
                if (!resync_after_unexpected(c)) return;
            }
        }
    }

private:
    GzStream in_;

    // Scans forward until it has matched the literal "VALUES (" and consumed
    // it. Returns false at EOF (meaning: no more INSERT statements).
    bool skip_to_values_start() {
        static const char* target = "VALUES (";
        size_t matched = 0;
        for (;;) {
            int c = in_.get();
            if (c == -1) return false;
            if (c == target[matched]) {
                matched++;
                if (target[matched] == '\0') return true;
            } else {
                matched = (c == target[0]) ? 1 : 0;
            }
        }
    }

    // After a malformed separator, try to recover by skipping to the next
    // '(' (start of a tuple) or ';' (end of statement, handled by caller).
    bool resync_after_unexpected(int firstChar) {
        int c = firstChar;
        while (c == ' ' || c == '\t' || c == '\n' || c == '\r') c = in_.get();
        if (c == '(') return true; // caller loop will treat next parse_tuple as starting now
        // put back logic not available (single-pass stream) -- best effort: if we
        // see ';' here, restart search for next statement.
        if (c == ';') return skip_to_values_start();
        return c != -1;
    }
    bool resync() {
        // very defensive fallback; scan until next '(' or EOF
        for (;;) {
            int c = in_.get();
            if (c == -1) return false;
            if (c == '(') return true;
        }
    }

    void parse_tuple_fields(std::vector<Field>& fields) {
        // We are positioned right after the opening '(' of the tuple.
        for (;;) {
            skip_ws();
            Field f;
            int c = in_.peek();
            if (c == '\'') {
                parse_quoted_string(f);
            } else if (matches_literal("NULL")) {
                f.is_null = true;
            } else {
                parse_raw_token(f);
            }
            fields.push_back(std::move(f));
            skip_ws();
            int sep = in_.get();
            if (sep == ',') continue;
            if (sep == ')') return;
            // Unexpected char inside tuple; stop tuple defensively.
            return;
        }
    }

    void skip_ws() {
        for (;;) {
            int c = in_.peek();
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { in_.get(); }
            else break;
        }
    }

    // Checks (and consumes, if matched) a short literal like "NULL".
    bool matches_literal(const char* lit) {
        // Cheap approach: only called where NULL vs. quote vs. number/hex is
        // the only possibility, first-char check is enough to disambiguate
        // for these dump formats (NULL never abuts a digit/quote).
        int c0 = in_.peek();
        if (c0 != lit[0] && c0 != tolower(lit[0])) return false;
        // consume len(lit) chars and verify
        std::string got;
        for (size_t i = 0; lit[i]; i++) {
            int c = in_.get();
            if (c == -1) { return false; }
            got.push_back(static_cast<char>(c));
        }
        if (got == lit) return true;
        // Mismatch: we've already consumed characters we can't push back.
        // This should not happen for well-formed MediaWiki dumps.
        throw std::runtime_error("sql_parser: expected literal NULL, got '" + got + "'");
    }

    void parse_quoted_string(Field& f) {
        in_.get(); // consume opening '
        std::string& out = f.value;
        for (;;) {
            int c = in_.get();
            if (c == -1) throw std::runtime_error("sql_parser: unterminated string");
            if (c == '\\') {
                int c2 = in_.get();
                switch (c2) {
                    case 'n': out.push_back('\n'); break;
                    case 't': out.push_back('\t'); break;
                    case 'r': out.push_back('\r'); break;
                    case '0': out.push_back('\0'); break;
                    case 'Z': out.push_back('\x1a'); break;
                    case '\\': out.push_back('\\'); break;
                    case '\'': out.push_back('\''); break;
                    case '"': out.push_back('"'); break;
                    default:
                        if (c2 == -1) throw std::runtime_error("sql_parser: unterminated escape");
                        out.push_back(static_cast<char>(c2));
                }
            } else if (c == '\'') {
                if (in_.peek() == '\'') { // doubled-quote escaping, just in case
                    in_.get();
                    out.push_back('\'');
                } else {
                    break; // end of string
                }
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }

    void parse_raw_token(Field& f) {
        std::string& out = f.value;
        for (;;) {
            int c = in_.peek();
            if (c == ',' || c == ')' || c == -1) break;
            out.push_back(static_cast<char>(in_.get()));
        }
    }
};
