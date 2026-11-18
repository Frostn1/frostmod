// A10: FrostServer's HTTP front (src/httpmux.h) over real loopback sockets.
//
// The old server read each connection with a blocking recv() and no timeout, on the one thread
// that also ran the announcement schedule. One client that connected and sent nothing froze
// every route and every scheduled announcement. These checks hold an idle connection (and a
// half-sent one) open and require that other clients are still answered at once, that the tick
// keeps running, and that the idle ones are dropped after the deadline.

#include "../src/httpmux.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#pragma comment(lib, "ws2_32.lib")

static int g_failures = 0;

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ++g_failures;                                                       \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                    \
            std::printf(__VA_ARGS__);                                           \
            std::printf("\n  (%s)\n", #cond);                                   \
        }                                                                       \
    } while (0)

using namespace httpmux;
using Clock = std::chrono::steady_clock;

static SOCKET Connect(int port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(s, (sockaddr*)&a, sizeof(a)) != 0) { closesocket(s); return INVALID_SOCKET; }
    const DWORD ms = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ms, sizeof(ms));
    return s;
}

// Sends `req` and reads to EOF. "" if nothing came back.
static std::string Fetch(int port, const std::string& req) {
    SOCKET s = Connect(port);
    if (s == INVALID_SOCKET) return "";
    send(s, req.data(), (int)req.size(), 0);
    std::string out;
    char buf[1024];
    for (;;) {
        int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) break;
        out.append(buf, (size_t)n);
    }
    closesocket(s);
    return out;
}

// True once the server has closed `s` (recv returns 0 or an error other than a timeout).
static bool ClosedByServer(SOCKET s, DWORD waitMs) {
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&waitMs, sizeof(waitMs));
    char b;
    const int n = recv(s, &b, 1, 0);
    return n == 0 || (n < 0 && WSAGetLastError() != WSAETIMEDOUT);
}

int main() {
    {   // request framing (pure)
        CHECK(RequestState("GET / HTTP/1.1\r\nHost: x\r\n") == Need::More, "headers not finished");
        CHECK(RequestState("GET / HTTP/1.1\r\nHost: x\r\n\r\n") == Need::Complete, "GET complete");
        CHECK(RequestState("POST /a HTTP/1.1\r\nContent-Length: 5\r\n\r\nab") == Need::More, "body short");
        CHECK(RequestState("POST /a HTTP/1.1\r\ncontent-length: 5\r\n\r\nabcde") == Need::Complete, "body all here");
        CHECK(RequestState("POST /a HTTP/1.1\r\nContent-Length: 999999\r\n\r\n") == Need::Complete,
              "an oversized declared body is answered at the headers, not waited for");
        CHECK(RequestState(std::string(kMaxHeader + 1, 'a')) == Need::TooLarge, "endless headers");
    }

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    addr.sin_port = 0;
    bind(listener, (sockaddr*)&addr, sizeof(addr));
    listen(listener, 16);
    int alen = sizeof(addr);
    getsockname(listener, (sockaddr*)&addr, &alen);
    const int port = ntohs(addr.sin_port);

    std::atomic<bool> running{true};
    std::atomic<int>  ticks{0};
    std::atomic<int>  handled{0};
    std::thread server([&] {
        Serve(listener, running,
              [&](SOCKET c, std::string& req, uint32_t ip) {
                  ++handled;
                  const std::string body = req.compare(0, 4, "POST") == 0
                      ? req.substr(req.find("\r\n\r\n") + 4)
                      : std::string(ip == htonl(INADDR_LOOPBACK) ? "loopback" : "other");
                  const std::string resp = "HTTP/1.1 200 OK\r\nContent-Length: " +
                      std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
                  send(c, resp.data(), (int)resp.size(), 0);
              },
              [&] { ++ticks; });
    });

    {   // an idle connection and a half-sent request do not hold anybody up
        SOCKET idle = Connect(port);
        SOCKET half = Connect(port);
        CHECK(idle != INVALID_SOCKET && half != INVALID_SOCKET, "connected");
        send(half, "GET /frostserver/info HTTP/1.1\r\n", 32, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        const int ticksBefore = ticks.load();
        const auto t0 = Clock::now();
        const std::string r = Fetch(port, "GET /health HTTP/1.1\r\nHost: x\r\n\r\n");
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
        CHECK(r.find("200 OK") != std::string::npos && r.find("loopback") != std::string::npos,
              "answered while two others sit open: [%s]", r.c_str());
        CHECK(ms < 500, "and at once, took %lld ms", (long long)ms);

        // A POST whose body comes in two pieces is still handed over whole.
        SOCKET p = Connect(port);
        const std::string head = "POST /frostserver/announce HTTP/1.1\r\nContent-Length: 11\r\n\r\nhello";
        send(p, head.data(), (int)head.size(), 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        send(p, " world", 6, 0);
        std::string got;
        char buf[512];
        for (int n; (n = recv(p, buf, sizeof(buf), 0)) > 0;) got.append(buf, (size_t)n);
        closesocket(p);
        CHECK(got.find("hello world") != std::string::npos, "split body arrives whole: [%s]", got.c_str());

        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        CHECK(ticks.load() - ticksBefore >= 2, "the schedule keeps ticking, %d ticks", ticks.load() - ticksBefore);

        // Past the deadline both stragglers are cut off, never handled.
        const int handledBefore = handled.load();
        CHECK(ClosedByServer(idle, (DWORD)kRequestMs + 1500), "the idle connection is dropped");
        CHECK(ClosedByServer(half, 1500), "the half-sent one too");
        CHECK(handled.load() == handledBefore, "and neither reached the handler");
        closesocket(idle);
        closesocket(half);
    }

    {   // more connections than the cap: the extras are refused, the server keeps serving
        SOCKET hold[kMaxConns + 4];
        for (auto& s : hold) s = Connect(port);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        for (auto& s : hold) if (s != INVALID_SOCKET) closesocket(s);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const std::string r = Fetch(port, "GET /health HTTP/1.1\r\n\r\n");
        CHECK(r.find("200 OK") != std::string::npos, "still serving after a flood: [%s]", r.c_str());
    }

    running = false;
    closesocket(listener);
    server.join();
    WSACleanup();

    if (g_failures == 0) std::printf("httpmux_test: all checks passed\n");
    else                 std::printf("httpmux_test: %d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
