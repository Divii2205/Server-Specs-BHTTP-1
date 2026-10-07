# BHTTP/1 — the specification

**BHTTP/1** is HTTP with the text taken out. Instead of a request line and
CRLF-separated headers, every message is carried in **frames**: a fixed
8-byte header that says how long the rest is, followed by that many bytes.

One TCP connection carries many requests, one after another, and stays open.

---

## 1. Connection preface

The client sends these 8 bytes, once, before anything else:

```
42 48 54 54 50 2f 31 0a        "BHTTP/1\n"
```

The server reads exactly 8 bytes and compares them. If they do not match it
sends a `GOAWAY` frame and closes the connection.

**Why bother?** A browser or a `telnet` session will send `GET / HTTP/1.1`,
which does not match. The server finds out in the first 8 bytes instead of
trying to parse text as a frame header. The preface also carries the version
number, so a future BHTTP/2 can announce itself here.

---

## 2. The frame header

Every frame starts with exactly 8 bytes. All numbers are **big-endian**
(network byte order).

```
 byte 0     byte 1     byte 2     byte 3     byte 4     byte 5     byte 6     byte 7
+----------+----------+----------+----------+----------+----------+----------+----------+
|                Length (24)               |  Type(8) | Flags(8) |R|      Stream ID (23)|
+----------+----------+----------+----------+----------+----------+----------+----------+
|                              Payload — exactly `Length` bytes                         |
+---------------------------------------------------------------------------------------+
```

| Field         | Bits | Meaning                                              |
|---------------|------|------------------------------------------------------|
| **Length**    | 24   | Number of payload bytes that follow the header.      |
| **Type**      | 8    | What kind of frame this is (section 3).              |
| **Flags**     | 8    | Bit field. Only bit 0 is defined (section 4).        |
| **R**         | 1    | Reserved. Senders MUST write 0, receivers MUST ignore it. |
| **Stream ID** | 23   | Which request/response pair this frame belongs to.   |

### Why these widths

HTTP/2 chose 24 / 8 / 8 / 1+31 and got a 9-byte header. I chose 24 / 8 / 8 /
1+23 and got **8**. Here is the reasoning for each field.

**Length — 24 bits.** This is the one field you cannot get wrong, because
every other frame on the connection is found by trusting it. 16 bits caps a
frame at 64 KiB, which would chop an ordinary image into dozens of frames for
no reason. 32 bits lets a hostile peer announce a 4 GiB frame and watch the
receiver try to allocate it. 24 bits gives a 16 MiB ceiling — big enough that
chunking is a choice rather than an obligation, small enough that the worst
case is survivable. In practice this implementation refuses anything over
1 MiB (section 8), so the 24-bit field is a generous outer bound, not a target.

**Type — 8 bits, byte aligned.** 256 possible frame types, of which 4 are
used. A receiver reads one whole byte and does a `switch` on it — no masking,
no shifting. Byte alignment is worth more than the 4 bits I could have saved,
because the alternative is bit-twiddling code in the hottest path in the
protocol. 252 spare types is exactly the room the skip rule (section 6) exists
to protect.

**Flags — 8 bits, byte aligned.** Eight independent on/off switches, one of
which (`END_MSG`) is defined today. Same argument as Type: one byte, no
shifting. Flags are deliberately *separate* from Type so that a flag means the
same thing across frame types and a receiver can act on `END_MSG` without
knowing what the frame is.

**Stream ID — 23 bits, plus 1 reserved bit.** BHTTP/1 sends one request at a
time, so strictly speaking the id could be dropped. It is kept for two
reasons: it lets a reply be matched to its request in a log or a packet
capture, and it is the field a future version would widen use of to multiplex.
HTTP/2 needs 31 bits because it interleaves many streams and burns id numbers
odd/even between the two peers; a sequential protocol does not, and 8.3 million
requests on one connection is far past any realistic connection lifetime.
The reserved bit is copied from HTTP/2 because it costs nothing and gives a
version 2 somewhere to put a flag that must not be mistaken for stream data.

**The total — 8 bytes.** This is the actual payoff. A power-of-two header
means the payload always begins on an 8-byte boundary, so a receiver can read
the length field as an aligned word instead of reassembling it byte by byte.
HTTP/2's 9-byte header misaligns everything that follows it, forever. It also
makes a hexdump readable: with 16 bytes per line, the header occupies exactly
the left half of the first line, every time.

---

## 3. Frame types

| Value  | Name       | Direction        | Payload                                |
|--------|------------|------------------|----------------------------------------|
| `0x01` | `REQUEST`  | client → server  | a header block (section 5)              |
| `0x02` | `RESPONSE` | server → client  | status (16 bits) + a header block       |
| `0x03` | `DATA`     | either           | raw body bytes, nothing else            |
| `0x04` | `GOAWAY`   | either           | code (16 bits) + a reason string        |
| `0x05`–`0xFF` | *unassigned* | —       | **must be skipped** (section 6)         |

---

## 4. Flags

| Bit    | Name      | Meaning                                              |
|--------|-----------|------------------------------------------------------|
| `0x01` | `END_MSG` | This is the last frame of the message.               |
| others | reserved  | Senders write 0. Receivers ignore them.              |

A message is finished when a frame arrives with `END_MSG` set. A `RESPONSE`
with no body sets `END_MSG` on the `RESPONSE` frame itself; otherwise the
flag goes on the final `DATA` frame.

---

## 5. Header encoding

Two mechanisms, borrowed from HPACK and nothing more.

**Mechanism one — number the names.** Ten header names are given a number.
These are the only names this protocol actually sends often, so they never
travel as text:

| ID | Name             | ID | Name             |
|----|------------------|----|------------------|
| 1  | `:method`        | 6  | `content-type`   |
| 2  | `:path`          | 7  | `user-agent`     |
| 3  | `:status`        | 8  | `server`         |
| 4  | `host`           | 9  | `date`           |
| 5  | `content-length` | 10 | `connection`     |

**Mechanism two — length-prefix the rest.** Anything not in the table is sent
as text, with its length in front, so the reader never scans for a delimiter.

A **header block** is:

```
+----------+
| Count(8) |                        how many headers follow
+----------+
   then, repeated `Count` times:
+----------+
| NameID(8)|                        1..10 = use the table, 0 = literal
+----------+
[ if NameID == 0:  NameLen(8) + name bytes ]
+---------------+
| ValueLen (16) |                   big-endian
+---------------+
| value bytes ... |
```

Names are lower-case. Pseudo-headers (`:method`, `:path`, `:status`) come
first, as in HTTP/2. Values are limited to 65535 bytes by the 16-bit length.

---

## 6. The skip rule

> **A receiver that meets a frame type it does not know MUST skip it cleanly.**

"Cleanly" means: read the 8-byte header, read exactly `Length` more bytes,
throw them away, and carry on with the next frame as if nothing happened. It
MUST NOT close the connection, MUST NOT reply with an error, and MUST NOT
try to guess what the frame meant.

This is possible only because `Length` sits in the fixed header and is
mandatory for *every* frame, including ones that have not been invented yet.
That is the whole reason the length comes first.

This rule is what makes a version 2 possible. A BHTTP/2 client can send a new
frame type to a BHTTP/1 server; the old server ignores it and answers the
request anyway, instead of dropping the connection. Without the rule, the only
way to add anything to the protocol is to break every existing implementation.

A receiver MAY log the fact that it skipped something. `bserve` prints:

```
[conn 4] unknown frame type 0x2a, 27 bytes skipped
```

---

## 7. A complete exchange

```
client                                          server
  |                                               |
  |-- "BHTTP/1\n" ------------------------------->|   preface, once
  |                                               |
  |-- REQUEST  stream 1, END_MSG ---------------->|   :method GET, :path /notes.txt
  |                                               |
  |<------------------ RESPONSE stream 1 ---------|   status 200 + headers
  |<------------------ DATA stream 1, END_MSG ----|   the file
  |                                               |
  |-- REQUEST  stream 2, END_MSG ---------------->|   same connection, next file
  |<------------------ RESPONSE stream 2 ---------|
  |<------------------ DATA stream 2, END_MSG ----|
  |                                               |
  |-- (client closes) --------------------------->|
```

The connection stays open between requests. The stream id counts up: 1, 2,
3, … A response always carries the stream id of the request it answers.

---

## 8. Status codes and errors

`RESPONSE` carries the status as a **16-bit number**, not the text `"200"`.
It is fixed width, needs no parsing, and cannot be `"2OO"`.

| Code | When                                                          |
|------|---------------------------------------------------------------|
| 200  | the file was found and is being sent                          |
| 400  | the frame or header block is malformed, or the path is unsafe |
| 404  | no such file under the root directory                         |
| 405  | the method is not `GET`                                       |
| 500  | the server failed for its own reasons                         |

A `400` is sent when, and only when, the bytes themselves are wrong: a header
block that claims more headers than it contains, a value length that runs past
the end of the payload, a `NameID` above 10, a missing `:method` or `:path`,
or a path containing `..`.

**Limits a receiver enforces.** A frame whose `Length` exceeds **1 MiB** is
refused with `400` and the connection is closed, without allocating the
buffer. This is well below the 16 MiB the length field could express — the
field sets the ceiling, the implementation sets the policy.

`GOAWAY` is for failures that are not about one request, such as a bad
preface. Its payload is a 16-bit code followed by a human-readable reason.

---

## 9. What is deliberately not here

No multiplexing (one request at a time), no flow control, no compression, no
TLS, no server push, no header dynamic table, no trailers. Those are the parts
of HTTP/2 that need a second version to earn. The skip rule in section 6 is
the hook they would hang from.
