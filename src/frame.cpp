// frame.cpp - the implementation of the wire format described in frame.h

#include "frame.h"
#include "net.h"

#include <cstdio>
#include <cstring>
#include <cctype>

namespace bhttp {

const char PREFACE[9] = "BHTTP/1\n";

// The ten header names we actually send. Index 0 is deliberately empty:
// id 0 on the wire means "a literal name follows".
const char* STATIC_TABLE[STATIC_TABLE_SIZE + 1] = {
    "",                 // 0 - reserved: literal name follows
    ":method",          // 1
    ":path",            // 2
    ":status",          // 3
    "host",             // 4
    "content-length",   // 5
    "content-type",     // 6
    "user-agent",       // 7
    "server",           // 8
    "date",             // 9
    "connection"        // 10
};

uint8_t static_id(const std::string& name) {
    for (int i = 1; i <= STATIC_TABLE_SIZE; ++i)
        if (name == STATIC_TABLE[i]) return (uint8_t)i;
    return 0;
}

const char* type_name(uint8_t t) {
    switch (t) {
        case T_REQUEST:  return "REQUEST";
        case T_RESPONSE: return "RESPONSE";
        case T_DATA:     return "DATA";
        case T_GOAWAY:   return "GOAWAY";
        default:         return "UNKNOWN";
    }
}

// ---------------------------------------------------------------------
// small helpers for writing big-endian numbers
// ---------------------------------------------------------------------
static void put_u8(std::vector<uint8_t>& o, uint8_t v) { o.push_back(v); }

static void put_u16(std::vector<uint8_t>& o, uint16_t v) {
    o.push_back((uint8_t)(v >> 8));
    o.push_back((uint8_t)(v));
}

static void put_u24(std::vector<uint8_t>& o, uint32_t v) {
    o.push_back((uint8_t)(v >> 16));
    o.push_back((uint8_t)(v >> 8));
    o.push_back((uint8_t)(v));
}

uint16_t get_u16(const std::vector<uint8_t>& b, size_t p) {
    return (uint16_t)((b[p] << 8) | b[p + 1]);
}

uint32_t get_u24(const std::vector<uint8_t>& b, size_t p) {
    return ((uint32_t)b[p] << 16) | ((uint32_t)b[p + 1] << 8) | b[p + 2];
}

// ---------------------------------------------------------------------
// frame header
// ---------------------------------------------------------------------
void put_frame_header(std::vector<uint8_t>& out, uint32_t len,
                      uint8_t type, uint8_t flags, uint32_t stream) {
    put_u24(out, len);                          // bytes 0-2  length
    put_u8(out, type);                          // byte  3    type
    put_u8(out, flags);                         // byte  4    flags
    // byte 5: top bit is reserved and always 0, then 23 bits of stream id
    put_u8(out, (uint8_t)((stream >> 16) & 0x7F));
    put_u8(out, (uint8_t)((stream >> 8) & 0xFF));
    put_u8(out, (uint8_t)(stream & 0xFF));
}

// ---------------------------------------------------------------------
// header block
// ---------------------------------------------------------------------
void encode_headers(std::vector<uint8_t>& out, const std::vector<Header>& hs) {
    put_u8(out, (uint8_t)hs.size());            // how many headers follow
    for (const Header& h : hs) {
        uint8_t id = static_id(h.name);
        put_u8(out, id);
        if (id == 0) {                          // name is not in the table
            put_u8(out, (uint8_t)h.name.size());
            out.insert(out.end(), h.name.begin(), h.name.end());
        }
        put_u16(out, (uint16_t)h.value.size()); // values are length-prefixed
        out.insert(out.end(), h.value.begin(), h.value.end());
    }
}

bool decode_headers(const std::vector<uint8_t>& buf, size_t pos,
                    std::vector<Header>& out) {
    if (pos >= buf.size()) return false;
    uint8_t count = buf[pos++];

    for (int i = 0; i < count; ++i) {
        if (pos >= buf.size()) return false;
        uint8_t id = buf[pos++];

        Header h;
        if (id == 0) {                          // literal name
            if (pos >= buf.size()) return false;
            uint8_t nlen = buf[pos++];
            if (pos + nlen > buf.size()) return false;
            h.name.assign((const char*)&buf[pos], nlen);
            pos += nlen;
        } else if (id <= STATIC_TABLE_SIZE) {   // indexed name
            h.name = STATIC_TABLE[id];
        } else {
            return false;                       // id out of range -> malformed
        }

        if (pos + 2 > buf.size()) return false;
        uint16_t vlen = get_u16(buf, pos);
        pos += 2;
        if (pos + vlen > buf.size()) return false;
        h.value.assign((const char*)&buf[pos], vlen);
        pos += vlen;

        out.push_back(h);
    }
    return true;
}

// ---------------------------------------------------------------------
// whole frames
// ---------------------------------------------------------------------
std::vector<uint8_t> make_request(uint32_t stream, const std::string& method,
                                  const std::string& path,
                                  const std::vector<Header>& extra) {
    std::vector<Header> hs;
    hs.push_back({":method", method});          // pseudo-headers come first
    hs.push_back({":path", path});
    for (const Header& h : extra) hs.push_back(h);

    std::vector<uint8_t> body;
    encode_headers(body, hs);

    std::vector<uint8_t> frame;
    put_frame_header(frame, (uint32_t)body.size(), T_REQUEST, F_END_MSG, stream);
    frame.insert(frame.end(), body.begin(), body.end());
    return frame;
}

std::vector<uint8_t> make_response(uint32_t stream, uint16_t status,
                                   const std::vector<Header>& hs,
                                   bool end_msg) {
    std::vector<uint8_t> body;
    put_u16(body, status);                      // status is a 16-bit number
    encode_headers(body, hs);

    std::vector<uint8_t> frame;
    put_frame_header(frame, (uint32_t)body.size(), T_RESPONSE,
                     end_msg ? F_END_MSG : 0, stream);
    frame.insert(frame.end(), body.begin(), body.end());
    return frame;
}

std::vector<uint8_t> make_data(uint32_t stream, const uint8_t* body,
                               size_t n, bool end_msg) {
    std::vector<uint8_t> frame;
    put_frame_header(frame, (uint32_t)n, T_DATA, end_msg ? F_END_MSG : 0, stream);
    frame.insert(frame.end(), body, body + n);
    return frame;
}

std::vector<uint8_t> make_goaway(uint32_t stream, uint16_t code,
                                 const std::string& reason) {
    std::vector<uint8_t> body;
    put_u16(body, code);
    body.insert(body.end(), reason.begin(), reason.end());

    std::vector<uint8_t> frame;
    put_frame_header(frame, (uint32_t)body.size(), T_GOAWAY, F_END_MSG, stream);
    frame.insert(frame.end(), body.begin(), body.end());
    return frame;
}

// ---------------------------------------------------------------------
// reading a frame
// ---------------------------------------------------------------------
ReadResult read_frame(long long sock, Frame& f) {
    sock_t s = (sock_t)sock;

    uint8_t head[HEADER_LEN];
    bool clean = false;
    if (!recv_all(s, head, HEADER_LEN, &clean))
        return clean ? FRAME_CLOSED : FRAME_IO_ERROR;  // clean EOF between frames is fine

    uint32_t len = ((uint32_t)head[0] << 16) | ((uint32_t)head[1] << 8) | head[2];
    f.type  = head[3];
    f.flags = head[4];
    // head[5] bit 7 is the reserved bit; mask it off and ignore it.
    f.stream = ((uint32_t)(head[5] & 0x7F) << 16) |
               ((uint32_t)head[6] << 8) | head[7];

    if (len > MAX_ACCEPT_LEN) return FRAME_TOO_BIG;   // refuse silly sizes

    f.payload.assign(len, 0);
    if (len > 0 && !recv_all(s, f.payload.data(), len))
        return FRAME_IO_ERROR;                  // header promised bytes that never came

    return FRAME_OK;
}

// ---------------------------------------------------------------------
// hexdump for -v
// ---------------------------------------------------------------------
void hexdump(const char* label, const uint8_t* d, size_t n) {
    std::fprintf(stderr, "%s  (%zu bytes)\n", label, n);
    for (size_t i = 0; i < n; i += 16) {
        std::fprintf(stderr, "  %04zx  ", i);

        for (size_t j = 0; j < 16; ++j) {       // the hex half
            if (i + j < n) std::fprintf(stderr, "%02x ", d[i + j]);
            else           std::fprintf(stderr, "   ");
            if (j == 7) std::fprintf(stderr, " ");
        }

        std::fprintf(stderr, " |");             // the printable half
        for (size_t j = 0; j < 16 && i + j < n; ++j) {
            unsigned char c = d[i + j];
            std::fputc(std::isprint(c) ? c : '.', stderr);
        }
        std::fprintf(stderr, "|\n");
    }
}

void hexdump(const char* label, const std::vector<uint8_t>& data) {
    hexdump(label, data.data(), data.size());
}

} // namespace bhttp
