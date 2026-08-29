// server/main.cpp
//
// Loads the graph ONCE at process startup (GraphEngine ctor) and keeps it
// resident in memory for the life of the process. Every HTTP request just
// runs bidirectional BFS against data already in RAM — no per-request
// loading, so queries stay fast (single-digit-to-low-double-digit ms even
// on the full enwiki graph) for as long as the server keeps running.
//
// Endpoints:
//   GET /api/search?start=Cat&goal=Dog       -> JSON path result
//   GET /api/suggest?q=Cat&limit=8           -> JSON title autocomplete
//   GET /api/health                          -> JSON {ok, pages, edges}
//   GET /  (and any other path)              -> serves ./static/
//
// Build (see server/README.md / project README for full instructions):
//   g++ -O2 -std=c++17 -Iinclude -o bin/server.exe server/main.cpp -lz -lpthread
//   (Windows/MinGW also needs: -lws2_32 -lcrypt32)

#include "graph_engine.hpp"
#include "httplib.h"
#include <iostream>
#include <sstream>

static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) { char buf[8]; std::snprintf(buf, sizeof buf, "\\u%04x", c); out += buf; }
                else out += static_cast<char>(c);
        }
    }
    return out;
}

static std::string result_to_json(const GraphEngine::Result& r) {
    std::ostringstream j;
    j << "{";
    j << "\"start_found\":" << (r.start_found ? "true" : "false") << ",";
    j << "\"goal_found\":" << (r.goal_found ? "true" : "false") << ",";
    j << "\"reachable\":" << (r.reachable ? "true" : "false") << ",";
    j << "\"start_canonical\":\"" << json_escape(r.start_canonical) << "\",";
    j << "\"goal_canonical\":\"" << json_escape(r.goal_canonical) << "\",";
    j << "\"start_redirected_from\":\"" << json_escape(r.start_redirected_from) << "\",";
    j << "\"goal_redirected_from\":\"" << json_escape(r.goal_redirected_from) << "\",";
    j << "\"ms\":" << r.ms << ",";
    j << "\"hops\":" << (r.reachable ? static_cast<long long>(r.path.size()) - 1 : -1) << ",";
    j << "\"path\":[";
    for (size_t i = 0; i < r.path.size(); i++) {
        if (i) j << ",";
        j << "\"" << json_escape(r.path[i]) << "\"";
    }
    j << "]}";
    return j.str();
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s outdir/ [port] [static_dir]\n", argv[0]);
        return 1;
    }
    std::string dir = argv[1];
    int port = (argc >= 3) ? std::atoi(argv[2]) : 8080;
    std::string static_dir = (argc >= 4) ? argv[3] : "static";

    std::cerr << "loading graph from " << dir << " ...\n";
    GraphEngine engine(dir); // <-- loaded once, lives for the whole process
    std::cerr << "loaded " << engine.num_pages() << " pages, " << engine.num_edges() << " edges. Ready.\n";

    httplib::Server svr;

    svr.set_default_headers({{"Access-Control-Allow-Origin", "*"}});

    svr.Get("/api/health", [&](const httplib::Request&, httplib::Response& res) {
        std::ostringstream j;
        j << "{\"ok\":true,\"pages\":" << engine.num_pages() << ",\"edges\":" << engine.num_edges() << "}";
        res.set_content(j.str(), "application/json");
    });

    svr.Get("/api/search", [&](const httplib::Request& req, httplib::Response& res) {
        if (!req.has_param("start") || !req.has_param("goal")) {
            res.status = 400;
            res.set_content("{\"error\":\"missing start or goal query param\"}", "application/json");
            return;
        }
        auto result = engine.query(req.get_param_value("start"), req.get_param_value("goal"));
        res.set_content(result_to_json(result), "application/json");
    });

    svr.Get("/api/suggest", [&](const httplib::Request& req, httplib::Response& res) {
        std::string q = req.has_param("q") ? req.get_param_value("q") : "";
        int limit = req.has_param("limit") ? std::atoi(req.get_param_value("limit").c_str()) : 8;
        limit = std::max(1, std::min(limit, 25));
        auto matches = engine.suggest(q, limit);
        std::ostringstream j;
        j << "[";
        for (size_t i = 0; i < matches.size(); i++) {
            if (i) j << ",";
            j << "\"" << json_escape(matches[i]) << "\"";
        }
        j << "]";
        res.set_content(j.str(), "application/json");
    });

    // Static frontend (index.html, script.js, style.css, ...)
    auto ret = svr.set_mount_point("/", static_dir);
    if (!ret) std::cerr << "warning: static dir '" << static_dir << "' not found, only /api/* will work\n";

    std::cerr << "listening on http://localhost:" << port << "\n";
    svr.listen("0.0.0.0", port);
    return 0;
}
