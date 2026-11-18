// FrostServer's HTTP front: many connections on one thread, none of which can hold the others.
//
// The old loop accepted one connection and then blocked in recv() on it, so a single client
// that connected and never sent a byte stopped every route and the scheduled announcements
// until it went away (security report A10). Here every client socket is non-blocking and sits
// in one select() set with the listener. A request is handed to the handler only once it is
// complete, a client that has not finished within kRequestMs is dropped, and there is a cap on
// how many may be open at once. The tick callback runs on every pass, at least every
// kTickMs, whatever the clients are doing.
//
// Header-only and free of FrostServer's own state, so tests/httpmux_test.cpp drives it over
// real loopback sockets.
#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace httpmux {

constexpr size_t   kMaxHeader   = 8192;   // request line + headers
constexpr size_t   kMaxBody     = 4096;   // the announce POST is the only body, and it is small
constexpr uint64_t kRequestMs   = 2000;   // a whole request must arrive within this
constexpr DWORD    kSendMs      = 2000;   // a response must drain within this
constexpr size_t   kMaxConns    = 32;     // well under FD_SETSIZE (64) with the listener
constexpr long     kTickMs      = 250;

enum class Need { More, Complete, TooLarge };

inline std::string Lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    return s;
}

// Whether `buf` holds one whole request: the headers, then as much body as Content-Length
// declares. A declared body over kMaxBody is complete at the headers, so the handler can answer
// 413 at once instead of waiting for bytes it will refuse.
inline Need RequestState(const std::string& buf) {
    const size_t hend = buf.find("\r\n\r\n");
    if (hend == std::string::npos) return buf.size() > kMaxHeader ? Need::TooLarge : Need::More;
    if (hend > kMaxHeader) return Need::TooLarge;
    const std::string head = Lower(buf.substr(0, hend + 2));
    const size_t k = head.find("\r\ncontent-length:");
    if (k == std::string::npos) return Need::Complete;
    const size_t declared = (size_t)std::strtoul(head.c_str() + k + 17, nullptr, 10);
    if (declared > kMaxBody) return Need::Complete;
    return buf.size() - (hend + 4) >= declared ? Need::Complete : Need::More;
}

// The handler gets a blocking socket with send/receive timeouts, the whole request, and the
// peer's IPv4 address in network order. It writes its response; the socket is closed after.
using Handler = std::function<void(SOCKET, std::string&, uint32_t peerIp)>;
using Tick    = std::function<void()>;

struct Conn {
    SOCKET      s = INVALID_SOCKET;
    std::string buf;
    uint64_t    deadline = 0;
    uint32_t    ip = 0;
};

inline void SendSmall(SOCKET s, const char* text) { send(s, text, (int)std::strlen(text), 0); }

// Runs until `running` goes false or the listener is closed under it. `listen` must already
// be bound and listening.
inline void Serve(SOCKET listen, const std::atomic<bool>& running, const Handler& onRequest,
                  const Tick& onTick) {
    std::vector<Conn> conns;
    auto drop = [&](size_t i) {
        closesocket(conns[i].s);
        conns.erase(conns.begin() + (long)i);
    };
    while (running.load()) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen, &rfds);
        for (const Conn& c : conns) FD_SET(c.s, &rfds);
        timeval tv{0, kTickMs * 1000};
        const int ready = select(0, &rfds, nullptr, nullptr, &tv);
        if (!running.load()) break;
        if (onTick) onTick();
        if (ready == SOCKET_ERROR) {
            // The listener was closed by the stop path, or a client socket went bad.
            if (!running.load()) break;
            Sleep(10);
        }

        const uint64_t now = GetTickCount64();
        for (size_t i = conns.size(); i-- > 0;) {
            Conn& c = conns[i];
            if (ready > 0 && FD_ISSET(c.s, &rfds)) {
                char chunk[2048];
                const int n = recv(c.s, chunk, sizeof(chunk), 0);
                if (n <= 0) { drop(i); continue; }
                c.buf.append(chunk, (size_t)n);
                const Need need = RequestState(c.buf);
                if (need == Need::TooLarge) {
                    SendSmall(c.s, "HTTP/1.1 431 Request Header Fields Too Large\r\n"
                                   "Content-Length: 0\r\nConnection: close\r\n\r\n");
                    drop(i);
                    continue;
                }
                if (need == Need::Complete) {
                    u_long blocking = 0;
                    ioctlsocket(c.s, FIONBIO, &blocking);
                    const DWORD ms = kSendMs;
                    setsockopt(c.s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&ms, sizeof(ms));
                    setsockopt(c.s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ms, sizeof(ms));
                    onRequest(c.s, c.buf, c.ip);
                    drop(i);
                    continue;
                }
            }
            if (now >= c.deadline) drop(i);   // too slow: whatever it was doing, it is done
        }

        if (ready > 0 && FD_ISSET(listen, &rfds)) {
            sockaddr_in peer{};
            int plen = sizeof(peer);
            SOCKET s = accept(listen, (sockaddr*)&peer, &plen);
            if (s == INVALID_SOCKET) {
                if (!running.load()) break;
                continue;
            }
            if (conns.size() >= kMaxConns) {
                // Full: refuse the newcomer rather than evict a client mid-request.
                closesocket(s);
                continue;
            }
            u_long nonblocking = 1;
            ioctlsocket(s, FIONBIO, &nonblocking);
            Conn c;
            c.s = s;
            c.deadline = GetTickCount64() + kRequestMs;
            c.ip = peer.sin_family == AF_INET ? peer.sin_addr.s_addr : 0;
            conns.push_back(std::move(c));
        }
    }
    for (Conn& c : conns) closesocket(c.s);
}

} // namespace httpmux
