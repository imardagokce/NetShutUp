// test_dns_server.cpp — Server on loopback with a fake Transport. NO external network:
// only 127.0.0.1 sockets with ephemeral ports.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "dns_frame.h"
#include "dns_server.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>

using namespace netshutup::dns;

static int g_failed = 0;
#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            wprintf(L"FAIL  %hs:%d  %hs\n", __FILE__, __LINE__, #cond);       \
            ++g_failed;                                                       \
        }                                                                     \
    } while (0)

// ── Fake upstream ─────────────────────────────────────────────────────────────

struct FakeTransport : Transport {
    std::atomic<bool> encrypted_ok{true};
    std::atomic<bool> legacy_ok{true};
    std::atomic<std::size_t> big_response{0};   // if >0: pad the answer to this many bytes
    std::atomic<int> encrypted_calls{0}, legacy_calls{0};

    static packet::Bytes answer(const packet::Bytes& q, std::size_t pad_to) {
        packet::Bytes r = q;
        r[2] |= 0x80;   // QR
        if (pad_to > r.size()) r.resize(pad_to, 0xAB);
        return r;
    }
    QueryResult query(const Upstream&, const packet::Bytes& wire, unsigned) override {
        ++encrypted_calls;
        QueryResult r;
        if (!encrypted_ok) { r.error = L"down"; return r; }
        r.ok = true;
        r.response = answer(wire, big_response);
        return r;
    }
    QueryResult query_legacy(const std::vector<std::wstring>&, const packet::Bytes& wire, unsigned) override {
        ++legacy_calls;
        QueryResult r;
        if (!legacy_ok) { r.error = L"down"; return r; }
        r.ok = true;
        r.response = answer(wire, 0);
        return r;
    }
};

static packet::Bytes make_query(std::uint16_t id) {
    packet::Bytes q = {static_cast<unsigned char>(id >> 8), static_cast<unsigned char>(id & 0xFF),
                       0x01, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0,
                       7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 3, 'c', 'o', 'm', 0, 0x00, 0x01, 0x00, 0x01};
    return q;
}

static Config make_cfg(bool with_legacy, unsigned threshold = 3) {
    Config c;
    c.chain = {Upstream{L"cloudflare", Method::DoH}, Upstream{L"google", Method::DoH}};
    if (with_legacy) c.legacy_dns = {L"192.0.2.1"};
    c.fail_threshold = threshold;
    return c;
}

// ── Loopback clients ──────────────────────────────────────────────────────────

static sockaddr_in loop_addr(unsigned short port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    InetPtonW(AF_INET, L"127.0.0.1", &a.sin_addr);
    return a;
}

// Returns the datagram, or empty on timeout.
static packet::Bytes udp_ask(unsigned short port, const packet::Bytes& q, DWORD timeout_ms = 2000) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    DWORD t = timeout_ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
    sockaddr_in a = loop_addr(port);
    sendto(s, reinterpret_cast<const char*>(q.data()), static_cast<int>(q.size()), 0,
           reinterpret_cast<sockaddr*>(&a), sizeof(a));
    unsigned char buf[4096];
    int n = recv(s, reinterpret_cast<char*>(buf), sizeof(buf), 0);
    closesocket(s);
    return n > 0 ? packet::Bytes(buf, buf + n) : packet::Bytes{};
}

static packet::Bytes tcp_ask(unsigned short port, const packet::Bytes& q) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    DWORD t = 3000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
    sockaddr_in a = loop_addr(port);
    if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) { closesocket(s); return {}; }
    packet::Bytes framed = packet::frame_tcp(q);
    send(s, reinterpret_cast<const char*>(framed.data()), static_cast<int>(framed.size()), 0);
    FrameAssembler fa;
    unsigned char buf[4096];
    packet::Bytes result;
    for (int i = 0; i < 50 && result.empty(); ++i) {
        int n = recv(s, reinterpret_cast<char*>(buf), sizeof(buf), 0);
        if (n <= 0) break;
        fa.push(buf, static_cast<std::size_t>(n));
        if (auto m = fa.next()) result = std::move(*m);
    }
    closesocket(s);
    return result;
}

static bool start(Server& s) {
    std::wstring err;
    bool ok = s.start(err);
    if (!ok) wprintf(L"start failed: %ls\n", err.c_str());
    return ok;
}

// ── Tests ─────────────────────────────────────────────────────────────────────

static void test_udp_and_tcp_roundtrip() {
    FakeTransport ft;
    ServerOptions o; o.port = 0;
    Server s(make_cfg(false), ft, o);
    EXPECT(start(s));
    unsigned short p = s.bound_port();
    EXPECT(p != 0);

    packet::Bytes r = udp_ask(p, make_query(0x1111));
    EXPECT(r.size() == 29);
    EXPECT(r.size() >= 12 && r[0] == 0x11 && r[1] == 0x11 && (r[2] & 0x80));

    packet::Bytes r2 = tcp_ask(p, make_query(0x2222));
    EXPECT(r2.size() == 29 && r2[0] == 0x22 && (r2[2] & 0x80));

    ServiceStatus st = s.status();
    EXPECT(st.queries == 2 && st.encrypted == 2 && st.servfail == 0);
    EXPECT(st.upstream.has_value());
    EXPECT(st.mode == Mode::Encrypted);
}

static void test_invalid_datagram_dropped() {
    FakeTransport ft;
    ServerOptions o; o.port = 0;
    Server s(make_cfg(false), ft, o);
    EXPECT(start(s));
    packet::Bytes r = udp_ask(s.bound_port(), packet::Bytes{1, 2, 3, 4, 5}, 400);
    EXPECT(r.empty());
    EXPECT(ft.encrypted_calls == 0);
}

static void test_large_answer_truncated_over_udp_full_over_tcp() {
    FakeTransport ft;
    ft.big_response = 700;
    ServerOptions o; o.port = 0;
    Server s(make_cfg(false), ft, o);
    EXPECT(start(s));
    packet::Bytes u = udp_ask(s.bound_port(), make_query(0x3333));
    EXPECT(u.size() == 29 && (u[2] & 0x02));   // TC set, header + question only
    packet::Bytes t = tcp_ask(s.bound_port(), make_query(0x3334));
    EXPECT(t.size() == 700);
}

static void test_servfail_when_everything_fails() {
    FakeTransport ft;
    ft.encrypted_ok = false;
    ServerOptions o; o.port = 0;
    Server s(make_cfg(false), ft, o);
    EXPECT(start(s));
    packet::Bytes r = udp_ask(s.bound_port(), make_query(0x4444));
    EXPECT(r.size() >= 12 && (r[3] & 0x0F) == 2 && r[0] == 0x44);
    EXPECT(s.status().servfail == 1);
    EXPECT(ft.legacy_calls == 0);
}

static void test_fallback_to_legacy_after_threshold() {
    FakeTransport ft;
    ft.encrypted_ok = false;
    ServerOptions o; o.port = 0;
    Server s(make_cfg(true, 1), ft, o);
    EXPECT(start(s));
    packet::Bytes first = udp_ask(s.bound_port(), make_query(0x5001));
    EXPECT(first.size() >= 12 && (first[3] & 0x0F) == 2);   // that round fails -> SERVFAIL, enters fallback
    packet::Bytes second = udp_ask(s.bound_port(), make_query(0x5002));
    EXPECT(second.size() == 29 && (second[3] & 0x0F) == 0 && (second[2] & 0x80));
    ServiceStatus st = s.status();
    EXPECT(st.mode == Mode::LegacyFallback);
    EXPECT(st.legacy == 1);
    EXPECT(ft.legacy_calls == 1);
}

static void test_concurrent_clients() {
    FakeTransport ft;
    ServerOptions o; o.port = 0;
    Server s(make_cfg(false), ft, o);
    EXPECT(start(s));
    const unsigned short p = s.bound_port();
    std::atomic<int> good{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 16; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < 10; ++i) {
                std::uint16_t id = static_cast<std::uint16_t>(0x6000 + t * 16 + i);
                packet::Bytes r = udp_ask(p, make_query(id));
                if (r.size() == 29 && r[0] == (id >> 8) && r[1] == (id & 0xFF)) ++good;
            }
        });
    }
    for (auto& th : threads) th.join();
    EXPECT(good == 160);
    EXPECT(s.status().queries == 160);
}

static void test_stop_and_port_reuse() {
    FakeTransport ft;
    ServerOptions o; o.port = 0;
    unsigned short p = 0;
    {
        Server s(make_cfg(false), ft, o);
        EXPECT(start(s));
        p = s.bound_port();

        // A second server on the same port must fail with a clear reason.
        ServerOptions o2; o2.port = p;
        Server dup(make_cfg(false), ft, o2);
        std::wstring err;
        EXPECT(!dup.start(err));
        EXPECT(err.find(L"already in use") != std::wstring::npos);

        DWORD t0 = GetTickCount();
        s.stop();
        EXPECT(GetTickCount() - t0 < 3000);
        s.stop();   // idempotent
    }
    ServerOptions o3; o3.port = p;
    Server again(make_cfg(false), ft, o3);
    EXPECT(start(again));   // port was released
}

static void test_start_fails_cleanly_without_upstreams() {
    FakeTransport ft;
    Config c;   // empty chain
    ServerOptions o; o.port = 0;
    Server s(c, ft, o);
    std::wstring err;
    EXPECT(!s.start(err));
    EXPECT(!err.empty());
    EXPECT(s.bound_port() == 0);
}

int main() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    test_udp_and_tcp_roundtrip();
    test_invalid_datagram_dropped();
    test_large_answer_truncated_over_udp_full_over_tcp();
    test_servfail_when_everything_fails();
    test_fallback_to_legacy_after_threshold();
    test_concurrent_clients();
    test_stop_and_port_reuse();
    test_start_fails_cleanly_without_upstreams();
    WSACleanup();
    if (g_failed) { wprintf(L"%d failure(s)\n", g_failed); return 1; }
    wprintf(L"dns_server: all passed\n");
    return 0;
}
