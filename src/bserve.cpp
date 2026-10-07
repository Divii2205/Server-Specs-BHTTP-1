// bserve.cpp - a BHTTP/1 file server.
//
//   Usage:  bserve <root-directory> <port> [-v]
//   Example: bserve ./www 9000
//
// What it does, in order:
//   1. listen on the port
//   2. accept a TCP connection
//   3. read the 8-byte connection preface
//   4. read one binary REQUEST frame at a time
//   5. map the path to a file under <root-directory>
//   6. reply with a RESPONSE frame then DATA frames
//   7. keep the connection open and go back to step 4

#include "net.h"
#include "frame.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace bhttp;

static std::string g_root = "./www";
static bool        g_verbose = false;
static std::atomic<int> g_conn_no{0};

// ---------------------------------------------------------------------
// tiny helpers
// ---------------------------------------------------------------------

// Guess a content-type from the file extension. Anything we do not
// recognise is sent as plain bytes.
static std::string mime_of(const std::string& path) {
    size_t dot = path.rfind('.');
    if (dot == std::string::npos) return "application/octet-stream";
    std::string e = path.substr(dot + 1);
    for (char& c : e) c = (char)tolower((unsigned char)c);

    if (e == "html" || e == "htm") return "text/html";
    if (e == "txt")                return "text/plain";
    if (e == "css")                return "text/css";
    if (e == "js")                 return "application/javascript";
    if (e == "json")               return "application/json";
    if (e == "png")                return "image/png";
    if (e == "jpg" || e == "jpeg") return "image/jpeg";
    if (e == "gif")                return "image/gif";
    if (e == "ico")                return "image/x-icon";
    return "application/octet-stream";
}

// Current time in the usual HTTP date format.
static std::string http_date() {
    std::time_t t = std::time(nullptr);
    std::tm g{};
#ifdef _WIN32
    gmtime_s(&g, &t);
#else
    gmtime_r(&t, &g);
#endif
    char buf[64];
    std::strftime(buf, sizeof buf, "%a, %d %b %Y %H:%M:%S GMT", &g);
    return buf;
}

// Turn "%20" and friends back into real characters.
static std::string percent_decode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() &&
            isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            out += (char)std::stoi(s.substr(i + 1, 2), nullptr, 16);
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

// Map a request path onto a file inside the root directory.
// Returns false if the path is not safe or not well formed.
static bool map_path(const std::string& raw, std::string& file_out) {
    if (raw.empty() || raw[0] != '/') return false;   // must be absolute

    std::string p = raw.substr(0, raw.find('?'));     // drop any ?query
    p = percent_decode(p);

    // Refuse anything that tries to climb out of the root, and refuse
    // backslashes so a Windows client cannot sneak past the check.
    if (p.find("..") != std::string::npos) return false;
    if (p.find('\\') != std::string::npos) return false;

    if (p.back() == '/') p += "index.html";           // "/" -> "/index.html"

    file_out = g_root + p;
    return true;
}

// Read a whole file into memory. Returns false if it cannot be opened.
static bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f),
               std::istreambuf_iterator<char>());
    return true;
}

// ---------------------------------------------------------------------
// sending
// ---------------------------------------------------------------------

static void send_bytes(sock_t s, const std::vector<uint8_t>& b, const char* label) {
    if (g_verbose) hexdump(label, b);
    send_all(s, b.data(), b.size());
}

// Send a status with a short text body. Used for 400 / 404 / 405 / 500.
static void send_error(sock_t s, uint32_t stream, uint16_t status,
                       const std::string& text) {
    std::vector<Header> hs = {
        {"content-type",   "text/plain"},
        {"content-length", std::to_string(text.size())},
        {"server",         "bserve/1.0"},
        {"date",           http_date()}
    };
    send_bytes(s, make_response(stream, status, hs, false), "S> RESPONSE");
    send_bytes(s, make_data(stream, (const uint8_t*)text.data(), text.size(), true),
               "S> DATA");
}

// Send a file: one RESPONSE frame, then the body split into DATA frames.
static void send_file(sock_t s, uint32_t stream, const std::string& path,
                      const std::vector<uint8_t>& body) {
    std::vector<Header> hs = {
        {"content-type",   mime_of(path)},
        {"content-length", std::to_string(body.size())},
        {"server",         "bserve/1.0"},
        {"date",           http_date()}
    };
    send_bytes(s, make_response(stream, 200, hs, body.empty()), "S> RESPONSE");

    // Split the body into CHUNK-sized DATA frames. The last one carries
    // the END_MSG flag so the client knows the message is finished.
    size_t off = 0;
    while (off < body.size()) {
        size_t n = std::min((size_t)CHUNK, body.size() - off);
        bool last = (off + n == body.size());
        send_bytes(s, make_data(stream, body.data() + off, n, last), "S> DATA");
        off += n;
    }
}

// ---------------------------------------------------------------------
// one request
// ---------------------------------------------------------------------
static void handle_request(sock_t s, const Frame& f, int cid) {
    std::vector<Header> hs;
    if (!decode_headers(f.payload, 0, hs)) {
        std::printf("[conn %d] malformed header block -> 400\n", cid);
        send_error(s, f.stream, 400, "400 Bad Request: malformed header block\n");
        return;
    }

    std::string method, path;
    for (const Header& h : hs) {
        if (h.name == ":method") method = h.value;
        if (h.name == ":path")   path   = h.value;
    }

    if (method.empty() || path.empty()) {
        std::printf("[conn %d] missing :method or :path -> 400\n", cid);
        send_error(s, f.stream, 400, "400 Bad Request: missing :method or :path\n");
        return;
    }

    std::printf("[conn %d] stream %u  %s %s\n", cid, f.stream,
                method.c_str(), path.c_str());

    if (method != "GET") {                         // this server only serves files
        send_error(s, f.stream, 405, "405 Method Not Allowed: only GET\n");
        return;
    }

    std::string file;
    if (!map_path(path, file)) {
        std::printf("[conn %d]   unsafe path -> 400\n", cid);
        send_error(s, f.stream, 400, "400 Bad Request: unsafe path\n");
        return;
    }

    std::vector<uint8_t> body;
    if (!read_file(file, body)) {
        std::printf("[conn %d]   %s not found -> 404\n", cid, file.c_str());
        send_error(s, f.stream, 404, "404 Not Found\n");
        return;
    }

    std::printf("[conn %d]   200 %s (%zu bytes)\n", cid, file.c_str(), body.size());
    send_file(s, f.stream, file, body);
}

// ---------------------------------------------------------------------
// one connection
// ---------------------------------------------------------------------
static void serve_connection(sock_t s, int cid) {
    // 1. the connection preface
    char pre[PREFACE_LEN];
    if (!recv_all(s, pre, PREFACE_LEN) ||
        std::memcmp(pre, PREFACE, PREFACE_LEN) != 0) {
        std::printf("[conn %d] bad preface, closing\n", cid);
        std::vector<uint8_t> g = make_goaway(0, 400, "bad preface");
        send_all(s, g.data(), g.size());
        CLOSE_SOCK(s);
        return;
    }
    if (g_verbose) hexdump("S< PREFACE", (const uint8_t*)pre, PREFACE_LEN);
    std::printf("[conn %d] preface ok, connection open\n", cid);

    // 2. frames, until the client goes away
    for (;;) {
        Frame f;
        ReadResult r = read_frame((long long)s, f);

        if (r == FRAME_CLOSED) {
            std::printf("[conn %d] client closed the connection\n", cid);
            break;
        }
        if (r == FRAME_IO_ERROR) {
            // Usually the client went away without saying goodbye. Nothing
            // useful can be sent down a broken socket, so just close.
            std::printf("[conn %d] connection lost\n", cid);
            break;
        }
        if (r == FRAME_TOO_BIG) {
            std::printf("[conn %d] frame too large -> 400 and close\n", cid);
            send_error(s, 0, 400, "400 Bad Request: frame too large\n");
            break;
        }

        if (g_verbose) {
            char label[64];
            std::snprintf(label, sizeof label, "S< %s payload", type_name(f.type));
            hexdump(label, f.payload);
        }

        switch (f.type) {
        case T_REQUEST:
            handle_request(s, f, cid);
            break;

        case T_DATA:
            // A GET has no body. Anything we get here is a protocol error.
            std::printf("[conn %d] unexpected DATA -> 400\n", cid);
            send_error(s, f.stream, 400, "400 Bad Request: unexpected DATA\n");
            break;

        case T_GOAWAY:
            std::printf("[conn %d] client sent GOAWAY, closing\n", cid);
            CLOSE_SOCK(s);
            return;

        default:
            // ===========================================================
            // THE SKIP RULE.
            // We do not know this frame type. The length field told us
            // exactly how many payload bytes to read, and read_frame()
            // has already consumed all of them. So we simply log it and
            // carry on with the next frame. Nothing is desynchronised.
            // This is what leaves room for a version 2.
            // ===========================================================
            std::printf("[conn %d] unknown frame type 0x%02x, %zu bytes skipped\n",
                        cid, f.type, f.payload.size());
            break;
        }
    }

    CLOSE_SOCK(s);
    std::printf("[conn %d] closed\n", cid);
}

// ---------------------------------------------------------------------
// main
// ---------------------------------------------------------------------
int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: bserve <root-directory> <port> [-v]\n");
        std::fprintf(stderr, "  example: bserve ./www 9000\n");
        return 2;
    }
    g_root = argv[1];
    int port = std::atoi(argv[2]);
    for (int i = 3; i < argc; ++i)
        if (std::string(argv[i]) == "-v") g_verbose = true;

    if (port <= 0 || port > 65535) {
        std::fprintf(stderr, "bserve: '%s' is not a valid port\n", argv[2]);
        return 2;
    }
    if (!g_root.empty() && (g_root.back() == '/' || g_root.back() == '\\'))
        g_root.pop_back();                       // no trailing slash, we add our own

    if (!net_start()) {
        std::fprintf(stderr, "bserve: could not start the network layer\n");
        return 1;
    }

    sock_t listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == BAD_SOCK) {
        std::fprintf(stderr, "bserve: socket() failed\n");
        return 1;
    }

    // Let us re-bind the port straight away after a restart.
    int yes = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof yes);

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons((uint16_t)port);

    if (bind(listener, (sockaddr*)&addr, sizeof addr) != 0) {
        std::fprintf(stderr, "bserve: cannot bind port %d (is it already in use?)\n", port);
        CLOSE_SOCK(listener);
        net_stop();
        return 1;
    }
    if (listen(listener, 16) != 0) {
        std::fprintf(stderr, "bserve: listen() failed\n");
        CLOSE_SOCK(listener);
        net_stop();
        return 1;
    }

    std::printf("bserve: serving '%s' on port %d\n", g_root.c_str(), port);
    std::printf("bserve: waiting for connections (Ctrl+C to stop)\n");
    std::fflush(stdout);

    for (;;) {
        sockaddr_in who{};
        socklen_int wholen = sizeof who;
        sock_t c = accept(listener, (sockaddr*)&who, &wholen);
        if (c == BAD_SOCK) continue;

        int cid = ++g_conn_no;
        char ip[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &who.sin_addr, ip, sizeof ip);
        std::printf("[conn %d] accepted from %s:%d\n", cid, ip, ntohs(who.sin_port));
        std::fflush(stdout);

        // One thread per connection, so a slow client does not block
        // everybody else. The connection stays open until the client
        // closes it.
        std::thread([c, cid]() {
            serve_connection(c, cid);
            std::fflush(stdout);
        }).detach();
    }

    CLOSE_SOCK(listener);
    net_stop();
    return 0;
}
