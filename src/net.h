// net.h - a very small wrapper so the same socket code builds on
//         Windows (Winsock) and on Linux / macOS (BSD sockets).
//
// Nothing clever happens here. It only renames a handful of things so
// that bserve.cpp and bcurl.cpp can be written once.

#ifndef NET_H
#define NET_H

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET sock_t;                 // Windows calls a socket a SOCKET
  #define BAD_SOCK    INVALID_SOCKET
  #define CLOSE_SOCK  closesocket
  typedef int socklen_int;
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  typedef int sock_t;                    // on Unix a socket is just an int
  #define BAD_SOCK    (-1)
  #define CLOSE_SOCK  close
  typedef socklen_t socklen_int;
#endif

#include <cerrno>
#include <string>

// Call once at the start of main(). On Windows it starts Winsock,
// everywhere else it does nothing.
inline bool net_start() {
#ifdef _WIN32
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
    return true;
#endif
}

// Call once before the program exits.
inline void net_stop() {
#ifdef _WIN32
    WSACleanup();
#endif
}

// The error number from the last socket call, for error messages.
inline int last_error() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

// Send every byte of `buf`. send() is allowed to send fewer bytes than
// we asked for, so we loop until the whole buffer is gone.
// Returns true if all n bytes were written.
inline bool send_all(sock_t s, const void* buf, size_t n) {
    const char* p = static_cast<const char*>(buf);
    size_t sent = 0;
    while (sent < n) {
        int k = ::send(s, p + sent, (int)(n - sent), 0);
        if (k <= 0) return false;        // peer closed, or an error
        sent += (size_t)k;
    }
    return true;
}

// Read exactly n bytes. recv() may return less than we asked for, so we
// loop. Returns true only if all n bytes arrived.
// A return of false with *clean_eof == true means "the peer hung up
// politely before sending anything" - that is normal, not an error.
inline bool recv_all(sock_t s, void* buf, size_t n, bool* clean_eof = nullptr) {
    char* p = static_cast<char*>(buf);
    size_t got = 0;
    if (clean_eof) *clean_eof = false;
    while (got < n) {
        int k = ::recv(s, p + got, (int)(n - got), 0);
        if (k == 0) {                    // orderly shutdown by the peer
            if (clean_eof && got == 0) *clean_eof = true;
            return false;
        }
        if (k < 0) return false;         // real error
        got += (size_t)k;
    }
    return true;
}

#endif // NET_H
