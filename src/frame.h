// frame.h - everything that defines the BHTTP/1 wire format.
//
// This one header is the code version of SPEC.md. If the spec and this
// file ever disagree, the spec is wrong.
//
//  Frame layout (8 bytes of header, then the payload):
//
//   byte:  0        1        2        3        4        5        6        7
//        +--------+--------+--------+--------+--------+--------+--------+--------+
//        |        Length (24 bits)          |  Type  | Flags  |R|  Stream ID (23)|
//        +--------+--------+--------+--------+--------+--------+--------+--------+
//
//  All multi-byte numbers are big-endian (network byte order).

#ifndef FRAME_H
#define FRAME_H

#include <cstdint>
#include <string>
#include <vector>

namespace bhttp {

// ---------------------------------------------------------------------
// Connection preface. The very first 8 bytes a client sends.
// It lets the server know at once that this is BHTTP and not text HTTP.
// ---------------------------------------------------------------------
extern const char PREFACE[9];          // "BHTTP/1\n"
const size_t PREFACE_LEN = 8;

// ---------------------------------------------------------------------
// Fixed sizes
// ---------------------------------------------------------------------
const size_t   HEADER_LEN        = 8;              // bytes in a frame header
const uint32_t MAX_PAYLOAD       = 0xFFFFFF;       // 24-bit length field = 16 MiB - 1
const uint32_t MAX_ACCEPT_LEN    = 1u << 20;       // we refuse anything over 1 MiB
const uint32_t CHUNK             = 16384;          // bytes of body per DATA frame

// ---------------------------------------------------------------------
// Frame types
// ---------------------------------------------------------------------
enum Type : uint8_t {
    T_REQUEST  = 0x01,   // client -> server : method + path + headers
    T_RESPONSE = 0x02,   // server -> client : status code + headers
    T_DATA     = 0x03,   // either direction : raw body bytes
    T_GOAWAY   = 0x04    // either direction : "I am closing, here is why"
    // 0x05 .. 0xFF are unassigned. A receiver MUST skip them (see skip rule).
};

// ---------------------------------------------------------------------
// Flags (a bit field in byte 4)
// ---------------------------------------------------------------------
const uint8_t F_END_MSG = 0x01;   // this frame is the last one of the message

// ---------------------------------------------------------------------
// The static header table. "Number the ten names you actually send."
// ID 0 is reserved to mean "a literal name follows".
// ---------------------------------------------------------------------
const int STATIC_TABLE_SIZE = 10;
extern const char* STATIC_TABLE[STATIC_TABLE_SIZE + 1];   // index 0 unused

// Returns the table id for `name` (1..10), or 0 if the name is not listed.
uint8_t static_id(const std::string& name);

// ---------------------------------------------------------------------
// A parsed frame
// ---------------------------------------------------------------------
struct Frame {
    uint8_t              type   = 0;
    uint8_t              flags  = 0;
    uint32_t             stream = 0;
    std::vector<uint8_t> payload;
};

// One header field: a name and a value.
struct Header {
    std::string name;
    std::string value;
};

// ---------------------------------------------------------------------
// Building bytes
// ---------------------------------------------------------------------

// Append the 8-byte frame header for a payload of `len` bytes.
void put_frame_header(std::vector<uint8_t>& out, uint32_t len,
                      uint8_t type, uint8_t flags, uint32_t stream);

// Encode a list of headers into a header block:
//     count(1) then, for each header:
//         nameId(1)  [ if 0: nameLen(1) + name ]  valueLen(2) + value
void encode_headers(std::vector<uint8_t>& out, const std::vector<Header>& hs);

// Read a header block back out. Returns false if the bytes run out or
// the block is malformed.
bool decode_headers(const std::vector<uint8_t>& buf, size_t pos,
                    std::vector<Header>& out);

// Whole frames, ready to hand to send_all().
std::vector<uint8_t> make_request(uint32_t stream, const std::string& method,
                                  const std::string& path,
                                  const std::vector<Header>& extra);

std::vector<uint8_t> make_response(uint32_t stream, uint16_t status,
                                   const std::vector<Header>& hs,
                                   bool end_msg);

std::vector<uint8_t> make_data(uint32_t stream, const uint8_t* body,
                               size_t n, bool end_msg);

std::vector<uint8_t> make_goaway(uint32_t stream, uint16_t code,
                                 const std::string& reason);

// ---------------------------------------------------------------------
// Reading bytes
// ---------------------------------------------------------------------

// What went wrong while reading a frame.
//
// Note the FRAME_ prefix. The obvious names (R_OK, R_IO ...) cannot be
// used: <io.h> on Windows and <unistd.h> on Linux already define R_OK
// as a macro for access(), and a macro would quietly replace our
// constant everywhere it appears.
enum ReadResult {
    FRAME_OK = 0,      // got a frame
    FRAME_CLOSED,      // peer hung up cleanly between frames
    FRAME_IO_ERROR,    // socket error, or half a frame and then EOF
    FRAME_TOO_BIG      // length field larger than MAX_ACCEPT_LEN
};

// Read exactly one frame off the socket. `sock` is int or SOCKET
// depending on the platform, so it is passed through as a template-free
// intptr to keep this header free of <winsock2.h>.
ReadResult read_frame(long long sock, Frame& f);

// Helpers for pulling numbers out of a payload.
uint16_t get_u16(const std::vector<uint8_t>& b, size_t pos);
uint32_t get_u24(const std::vector<uint8_t>& b, size_t pos);

// Human-readable name of a frame type, for the -v log.
const char* type_name(uint8_t t);

// Print an annotated hexdump of `data` to stderr with `label` on top.
void hexdump(const char* label, const std::vector<uint8_t>& data);
void hexdump(const char* label, const uint8_t* data, size_t n);

} // namespace bhttp
#endif // FRAME_H
