// bcurl.cpp - a BHTTP/1 client, shaped like curl.
//
//   Usage:  bcurl [-v] [--send-unknown] host:port/path [more/paths ...]
//   Example: bcurl -v localhost:9000/index.html
//
// What it does, in order:
//   1. open ONE TCP connection
//   2. send the 8-byte connection preface
//   3. build and send a binary REQUEST frame
//   4. read frames back until the message ends
//   5. write the body to stdout
//   6. if more paths were given, reuse the SAME connection for them
//   7. exit non-zero if any response was a 4xx or 5xx
//
// It never opens a second connection.

#include "net.h"
#include "frame.h"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
  #include <io.h>
  #include <fcntl.h>
#endif

using namespace bhttp;

static bool g_verbose = false;

// ---------------------------------------------------------------------
// url parsing:  [bhttp://]host[:port]/path
// ---------------------------------------------------------------------
struct Url {
    std::string host = "localhost";
    int         port = 9000;
    std::string path = "/";
};

static bool parse_url(const std::string& in, Url& u) {
    std::string s = in;

    const std::string scheme = "bhttp://";       // optional, ignored
    if (s.rfind(scheme, 0) == 0) s = s.substr(scheme.size());
    if (s.empty()) return false;

    // If it starts with '/', it is just a path - reuse the previous host.
    if (s[0] == '/') { u.path = s; return true; }

    size_t slash = s.find('/');
    std::string authority = (slash == std::string::npos) ? s : s.substr(0, slash);
    u.path = (slash == std::string::npos) ? "/" : s.substr(slash);

    size_t colon = authority.find(':');
    if (colon == std::string::npos) {
        u.host = authority;                      // no port given, keep the default
    } else {
        u.host = authority.substr(0, colon);
        u.port = std::atoi(authority.substr(colon + 1).c_str());
    }
    if (u.host.empty() || u.port <= 0 || u.port > 65535) return false;
    return true;
}

// ---------------------------------------------------------------------
// connecting
// ---------------------------------------------------------------------
static sock_t connect_to(const std::string& host, int port) {
    addrinfo hints{};
    hints.ai_family   = AF_INET;                 // IPv4, to keep this simple
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* list = nullptr;
    std::string portstr = std::to_string(port);
    if (getaddrinfo(host.c_str(), portstr.c_str(), &hints, &list) != 0) {
        std::fprintf(stderr, "bcurl: cannot resolve '%s'\n", host.c_str());
        return BAD_SOCK;
    }

    sock_t s = BAD_SOCK;
    for (addrinfo* a = list; a; a = a->ai_next) {
        s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == BAD_SOCK) continue;
        if (connect(s, a->ai_addr, (socklen_int)a->ai_addrlen) == 0) break;
        CLOSE_SOCK(s);
        s = BAD_SOCK;
    }
    freeaddrinfo(list);

    if (s == BAD_SOCK)
        std::fprintf(stderr, "bcurl: cannot connect to %s:%d\n", host.c_str(), port);
    return s;
}

// Hexdump a received frame for -v. read_frame() hands back only the
// payload, so rebuild the 8 header bytes to show the real frame.
static void dump_received(const Frame& f) {
    std::vector<uint8_t> whole;
    put_frame_header(whole, (uint32_t)f.payload.size(), f.type, f.flags, f.stream);
    whole.insert(whole.end(), f.payload.begin(), f.payload.end());
    char label[80];
    std::snprintf(label, sizeof label, "C< %s frame", type_name(f.type));
    hexdump(label, whole);
}

// ---------------------------------------------------------------------
// one request / response exchange on an already-open connection
// Returns the HTTP-style status code, or 0 if the exchange failed.
// ---------------------------------------------------------------------
static int do_exchange(sock_t s, uint32_t stream, const std::string& host,
                       const std::string& path, bool send_unknown, bool malformed) {
    // -- optional: a deliberately broken REQUEST frame, to check that the
    //    server answers 400 instead of crashing or hanging. The header
    //    block claims five headers but only one is actually there.
    if (malformed) {
        std::vector<uint8_t> bad = {0x05, 0x01, 0x00, 0x03, 'G', 'E', 'T'};
        std::vector<uint8_t> f;
        put_frame_header(f, (uint32_t)bad.size(), T_REQUEST, F_END_MSG, stream);
        f.insert(f.end(), bad.begin(), bad.end());
        if (g_verbose) hexdump("C> REQUEST frame (deliberately malformed)", f);
        send_all(s, f.data(), f.size());

        Frame r;
        bool got = read_frame((long long)s, r) == FRAME_OK;
        if (got && g_verbose) dump_received(r);
        if (got && r.type == T_RESPONSE && r.payload.size() >= 2) {
            int st = get_u16(r.payload, 0);
            std::fprintf(stderr, "bcurl: server answered %d to the malformed frame\n", st);
            // Drain the little error body that follows.
            if (!(r.flags & F_END_MSG)) {
                Frame d;
                while (read_frame((long long)s, d) == FRAME_OK) {
                    if (g_verbose) dump_received(d);
                    if (d.type == T_DATA) {
                        std::fwrite(d.payload.data(), 1, d.payload.size(), stdout);
                        if (d.flags & F_END_MSG) break;
                    }
                }
            }
            std::fflush(stdout);
            return st;
        }
        std::fprintf(stderr, "bcurl: no usable answer to the malformed frame\n");
        return 0;
    }

    // -- optional: an experimental frame the server has never heard of.
    //    A correct BHTTP/1 receiver reads its length, throws the payload
    //    away, and carries on. This is how we prove the skip rule works.
    if (send_unknown) {
        const char* junk = "hello from a future version";
        std::vector<uint8_t> f;
        put_frame_header(f, (uint32_t)std::strlen(junk), 0x2A, 0, stream);
        f.insert(f.end(), junk, junk + std::strlen(junk));
        if (g_verbose) hexdump("C> UNKNOWN (type 0x2a)", f);
        send_all(s, f.data(), f.size());
    }

    // -- build and send the request frame
    std::vector<Header> extra = {
        {"host",       host},
        {"user-agent", "bcurl/1.0"}
    };
    std::vector<uint8_t> req = make_request(stream, "GET", path, extra);

    if (g_verbose) {
        std::fprintf(stderr, "C> GET %s  (stream %u)\n", path.c_str(), stream);
        hexdump("C> REQUEST frame", req);
    }
    if (!send_all(s, req.data(), req.size())) {
        std::fprintf(stderr, "bcurl: failed to send the request\n");
        return 0;
    }

    // -- read frames until the message is finished
    int status = 0;
    bool seen_response = false;

    for (;;) {
        Frame f;
        ReadResult r = read_frame((long long)s, f);
        if (r != FRAME_OK) {
            std::fprintf(stderr,
                "bcurl: connection ended before the response was complete\n");
            return 0;
        }

        if (g_verbose) dump_received(f);

        if (f.type == T_RESPONSE) {
            if (f.payload.size() < 2) {
                std::fprintf(stderr, "bcurl: RESPONSE frame is too short\n");
                return 0;
            }
            status = get_u16(f.payload, 0);

            std::vector<Header> hs;
            if (!decode_headers(f.payload, 2, hs)) {
                std::fprintf(stderr, "bcurl: could not decode the response headers\n");
                return 0;
            }
            seen_response = true;

            if (g_verbose) {
                std::fprintf(stderr, "C< status %d\n", status);
                for (const Header& h : hs)
                    std::fprintf(stderr, "C<   %s: %s\n", h.name.c_str(), h.value.c_str());
            }
            if (f.flags & F_END_MSG) break;      // a response with no body

        } else if (f.type == T_DATA) {
            if (!seen_response) {
                std::fprintf(stderr, "bcurl: DATA arrived before RESPONSE\n");
                return 0;
            }
            // The body goes to stdout, so `bcurl ... > file` just works.
            std::fwrite(f.payload.data(), 1, f.payload.size(), stdout);
            if (f.flags & F_END_MSG) break;

        } else if (f.type == T_GOAWAY) {
            int code = f.payload.size() >= 2 ? get_u16(f.payload, 0) : 0;
            std::string why(f.payload.begin() + (f.payload.size() >= 2 ? 2 : 0),
                            f.payload.end());
            std::fprintf(stderr, "bcurl: server sent GOAWAY %d: %s\n", code, why.c_str());
            return code ? code : 0;

        } else {
            // The skip rule, on the client side. read_frame() already ate
            // the whole payload, so we only have to ignore it.
            std::fprintf(stderr, "bcurl: skipping unknown frame type 0x%02x (%zu bytes)\n",
                         f.type, f.payload.size());
        }
    }

    std::fflush(stdout);
    return status;
}

// ---------------------------------------------------------------------
// main
// ---------------------------------------------------------------------
int main(int argc, char** argv) {
    bool send_unknown = false;
    bool malformed    = false;
    std::vector<std::string> targets;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-v")                   g_verbose = true;
        else if (a == "--send-unknown")  send_unknown = true;
        else if (a == "--malformed")     malformed = true;
        else if (a == "-h" || a == "--help") { targets.clear(); break; }
        else                             targets.push_back(a);
    }

    if (targets.empty()) {
        std::fprintf(stderr,
            "usage: bcurl [-v] [--send-unknown] [--malformed] host:port/path [paths ...]\n"
            "  -v              hexdump every frame sent and received\n"
            "  --send-unknown  also send an unknown frame type, to test the skip rule\n"
            "  --malformed     send a broken request frame, to test the 400 reply\n"
            "  extra paths are fetched over the SAME connection\n"
            "example: bcurl -v localhost:9000/index.html\n");
        return 2;
    }

#ifdef _WIN32
    // Stop Windows from turning \n into \r\n when we write the body.
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    Url first;
    if (!parse_url(targets[0], first)) {
        std::fprintf(stderr, "bcurl: cannot understand '%s'\n", targets[0].c_str());
        return 2;
    }

    if (!net_start()) {
        std::fprintf(stderr, "bcurl: could not start the network layer\n");
        return 1;
    }

    // ---- exactly one connection, for every path on the command line ----
    sock_t s = connect_to(first.host, first.port);
    if (s == BAD_SOCK) { net_stop(); return 1; }
    if (g_verbose)
        std::fprintf(stderr, "C: connected to %s:%d\n", first.host.c_str(), first.port);

    // the connection preface
    if (g_verbose) hexdump("C> PREFACE", (const uint8_t*)PREFACE, PREFACE_LEN);
    if (!send_all(s, PREFACE, PREFACE_LEN)) {
        std::fprintf(stderr, "bcurl: failed to send the preface\n");
        CLOSE_SOCK(s); net_stop(); return 1;
    }

    int worst = 200;                             // the "most severe" status seen
    uint32_t stream = 1;                         // stream ids count up: 1, 2, 3 ...

    for (size_t i = 0; i < targets.size(); ++i) {
        Url u = first;                           // inherit host and port
        if (!parse_url(targets[i], u)) {
            std::fprintf(stderr, "bcurl: cannot understand '%s'\n", targets[i].c_str());
            worst = 400;
            continue;
        }
        if (u.host != first.host || u.port != first.port) {
            // We promised never to open a second connection, so we do not.
            std::fprintf(stderr,
                "bcurl: '%s' is on a different server; skipping "
                "(bcurl uses one connection only)\n", targets[i].c_str());
            worst = 400;
            continue;
        }

        int st = do_exchange(s, stream++, u.host, u.path, send_unknown, malformed);
        if (st == 0) { CLOSE_SOCK(s); net_stop(); return 1; }   // transport failure
        if (st > worst) worst = st;
        if (g_verbose) std::fprintf(stderr, "C: --- %s -> %d ---\n", u.path.c_str(), st);
    }

    CLOSE_SOCK(s);
    net_stop();

    // Exit code: 0 for success, 4 for any 4xx, 5 for any 5xx.
    if (worst >= 500) return 5;
    if (worst >= 400) return 4;
    return 0;
}
