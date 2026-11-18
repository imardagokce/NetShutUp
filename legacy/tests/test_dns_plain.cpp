// test_dns_plain.cpp — Unit tests for the plain DNS fall-back transport (NetShutUp Legacy).
// Loopback only: every "server" is a fake running on a std::thread inside this process,
// bound to 127.0.0.1 (or 127.0.0.2) on an ephemeral port. No external network.

#include <winsock2.h>
#include <ws2tcpip.h>

#include "dns_plain.h"
#include "dns_config.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace netshutup::dns;
using netshutup::dns::packet::Bytes;

static int g_failed = 0;

#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            wprintf(L"FAIL  %hs:%d  %hs\n", __FILE__, __LINE__, #cond);       \
            ++g_failed;                                                       \
        }                                                                     \
    } while (0)

namespace {

// ── Winsock / socket helpers ────────────────────────────────────────────────

struct WsaGuard {
    WsaGuard() {
        WSADATA d{};
        ok = WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }
    ~WsaGuard() {
        if (ok) WSACleanup();
    }
    bool ok = false;
};

// Owns one SOCKET.
class Sock {
public:
    Sock() = default;
    explicit Sock(SOCKET s) : s_(s) {}
    Sock(Sock&& o) noexcept : s_(o.s_) { o.s_ = INVALID_SOCKET; }
    Sock(const Sock&) = delete;
    Sock& operator=(const Sock&) = delete;
    ~Sock() { close(); }

    void close() {
        if (s_ != INVALID_SOCKET) {
            closesocket(s_);
            s_ = INVALID_SOCKET;
        }
    }
    bool valid() const { return s_ != INVALID_SOCKET; }
    SOCKET get() const { return s_; }

private:
    SOCKET s_ = INVALID_SOCKET;
};

// Joins the fake server thread when it goes out of scope.
class ThreadJoin {
public:
    explicit ThreadJoin(std::thread&& t) : t_(std::move(t)) {}
    ~ThreadJoin() {
        if (t_.joinable()) t_.join();
    }
    ThreadJoin(const ThreadJoin&) = delete;
    ThreadJoin& operator=(const ThreadJoin&) = delete;

private:
    std::thread t_;
};

sockaddr_in loopback(unsigned char last_octet, unsigned short port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.S_un.S_addr = htonl(0x7F000000u | static_cast<std::uint32_t>(last_octet));
    return a;
}

unsigned short local_port(SOCKET s) {
    sockaddr_in a{};
    int n = sizeof(a);
    getsockname(s, reinterpret_cast<sockaddr*>(&a), &n);
    return ntohs(a.sin_port);
}

// UDP socket bound to 127.0.0.<last_octet>:<port> (port 0 = ephemeral). Invalid on failure.
Sock udp_bind(unsigned char last_octet, unsigned short port) {
    Sock s(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (!s.valid()) return s;
    sockaddr_in a = loopback(last_octet, port);
    if (bind(s.get(), reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) s.close();
    return s;
}

// TCP listener on 127.0.0.1:<port>. Invalid on failure.
Sock tcp_listen(unsigned short port) {
    Sock s(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (!s.valid()) return s;
    sockaddr_in a = loopback(1, port);
    if (bind(s.get(), reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 ||
        listen(s.get(), 4) != 0) {
        s.close();
    }
    return s;
}

bool wait_readable(SOCKET s, unsigned ms) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval tv{};
    tv.tv_sec = static_cast<long>(ms / 1000);
    tv.tv_usec = static_cast<long>((ms % 1000) * 1000);
    return select(0, &set, nullptr, nullptr, &tv) > 0;
}

bool recv_exact(SOCKET s, std::uint8_t* buf, std::size_t n, unsigned ms) {
    std::size_t got = 0;
    while (got < n) {
        if (!wait_readable(s, ms)) return false;
        int r = recv(s, reinterpret_cast<char*>(buf + got), static_cast<int>(n - got), 0);
        if (r <= 0) return false;
        got += static_cast<std::size_t>(r);
    }
    return true;
}

bool send_all(SOCKET s, const std::uint8_t* buf, std::size_t n) {
    std::size_t sent = 0;
    while (sent < n) {
        int r = send(s, reinterpret_cast<const char*>(buf + sent), static_cast<int>(n - sent), 0);
        if (r <= 0) return false;
        sent += static_cast<std::size_t>(r);
    }
    return true;
}

// ── Fake servers ────────────────────────────────────────────────────────────

// Given the received query, returns the datagrams the fake sends back (in order).
using UdpReply = std::function<std::vector<Bytes>(const Bytes& query)>;

// Waits (bounded) for one datagram, then sends the reply datagrams to its sender.
void udp_fake_once(SOCKET s, UdpReply reply) {
    if (!wait_readable(s, 3000)) return;
    std::uint8_t buf[1024];
    sockaddr_in from{};
    int from_len = sizeof(from);
    int n = recvfrom(s, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                     reinterpret_cast<sockaddr*>(&from), &from_len);
    if (n < 12) return;
    Bytes query(buf, buf + n);
    for (const Bytes& d : reply(query)) {
        sendto(s, reinterpret_cast<const char*>(d.data()), static_cast<int>(d.size()), 0,
               reinterpret_cast<sockaddr*>(&from), from_len);
    }
}

// Accepts one TCP connection, reads one framed query, and answers with `answer`
// framed by a 2-byte length. The frame is split across two send() calls.
void tcp_fake_once(SOCKET listener, Bytes answer) {
    if (!wait_readable(listener, 3000)) return;
    Sock conn(accept(listener, nullptr, nullptr));
    if (!conn.valid()) return;

    std::uint8_t hdr[2] = {0, 0};
    if (!recv_exact(conn.get(), hdr, 2, 3000)) return;
    std::size_t qlen = (static_cast<std::size_t>(hdr[0]) << 8) | static_cast<std::size_t>(hdr[1]);
    Bytes query(qlen);
    if (!recv_exact(conn.get(), query.data(), qlen, 3000)) return;

    Bytes framed;
    framed.push_back(static_cast<std::uint8_t>(answer.size() >> 8));
    framed.push_back(static_cast<std::uint8_t>(answer.size() & 0xFF));
    framed.insert(framed.end(), answer.begin(), answer.end());

    const std::size_t split = 40;   // inside the payload, not at a frame boundary
    send_all(conn.get(), framed.data(), split);
    Sleep(50);
    send_all(conn.get(), framed.data() + split, framed.size() - split);
}

// ── Message helpers ─────────────────────────────────────────────────────────

// 29-byte example.com A query: id 0x1234, flags 0x0100 (RD), QDCOUNT 1.
Bytes example_query() {
    return Bytes{
        0x12, 0x34,                                       // ID
        0x01, 0x00,                                       // flags: RD
        0x00, 0x01,                                       // QDCOUNT
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,               // AN/NS/AR
        0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e',          // 7"example"
        0x03, 'c', 'o', 'm',                              // 3"com"
        0x00,                                             // root
        0x00, 0x01,                                       // QTYPE A
        0x00, 0x01,                                       // QCLASS IN
    };
}

std::vector<std::wstring> servers(std::initializer_list<const wchar_t*> list) {
    std::vector<std::wstring> out;
    for (const wchar_t* s : list) out.emplace_back(s);
    return out;
}

// Response to `q` with QR set (and optionally TC).
Bytes with_qr(Bytes q, bool tc = false) {
    q[2] = static_cast<std::uint8_t>(q[2] | 0x80);
    if (tc) q[2] = static_cast<std::uint8_t>(q[2] | 0x02);
    return q;
}

bool is_response_to(const Bytes& resp, const Bytes& q) {
    return resp.size() >= 12 && resp[0] == q[0] && resp[1] == q[1] && (resp[2] & 0x80) != 0;
}

long long elapsed_ms(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t0).count();
}

// ── Tests ───────────────────────────────────────────────────────────────────

// (1) UDP fake echoes the query with QR set -> ok, response ID == query ID.
void test_udp_answer() {
    Sock s = udp_bind(1, 0);
    EXPECT(s.valid());
    if (!s.valid()) return;
    const unsigned short port = local_port(s.get());
    const Bytes q = example_query();

    ThreadJoin th(std::thread(udp_fake_once, s.get(),
                              UdpReply([](const Bytes& in) { return std::vector<Bytes>{with_qr(in)}; })));

    PlainTransport t(port);
    QueryResult r = t.query_legacy(servers({L"127.0.0.1"}), q, 1000);
    EXPECT(r.ok);
    EXPECT(is_response_to(r.response, q));
    EXPECT(r.response.size() == q.size());
}

// (2) First server is unusable text, second is 127.0.0.1 -> second answers.
void test_bad_first_server_skipped() {
    Sock s = udp_bind(1, 0);
    EXPECT(s.valid());
    if (!s.valid()) return;
    const unsigned short port = local_port(s.get());
    const Bytes q = example_query();

    ThreadJoin th(std::thread(udp_fake_once, s.get(),
                              UdpReply([](const Bytes& in) { return std::vector<Bytes>{with_qr(in)}; })));

    PlainTransport t(port);
    QueryResult r = t.query_legacy(servers({L"not-an-ip", L"127.0.0.1"}), q, 1000);
    EXPECT(r.ok);
    EXPECT(is_response_to(r.response, q));
}

// (3a) Silent server only -> ok=false within about a second, with a reason.
void test_silent_server_times_out() {
    Sock s = udp_bind(1, 0);            // bound, never reads or answers
    EXPECT(s.valid());
    if (!s.valid()) return;
    const unsigned short port = local_port(s.get());
    const Bytes q = example_query();

    PlainTransport t(port);
    const auto t0 = std::chrono::steady_clock::now();
    QueryResult r = t.query_legacy(servers({L"127.0.0.1"}), q, 300);
    const long long ms = elapsed_ms(t0);

    EXPECT(!r.ok);
    EXPECT(!r.error.empty());
    EXPECT(ms < 1000);
}

// (3b) Silent server on 127.0.0.1, working server on 127.0.0.2, same port -> second answers.
void test_silent_then_working_server() {
    Sock silent = udp_bind(1, 0);
    EXPECT(silent.valid());
    if (!silent.valid()) return;
    const unsigned short port = local_port(silent.get());

    Sock working = udp_bind(2, port);
    if (!working.valid()) {
        wprintf(L"SKIP  silent_then_working_server: cannot bind 127.0.0.2:%u on this machine\n", port);
        return;
    }
    const Bytes q = example_query();

    ThreadJoin th(std::thread(udp_fake_once, working.get(),
                              UdpReply([](const Bytes& in) { return std::vector<Bytes>{with_qr(in)}; })));

    PlainTransport t(port);
    QueryResult r = t.query_legacy(servers({L"127.0.0.1", L"127.0.0.2"}), q, 300);
    EXPECT(r.ok);
    EXPECT(is_response_to(r.response, q));
}

// (4) A datagram with the wrong ID arrives first, then the correct one -> correct one returned.
void test_wrong_id_ignored() {
    Sock s = udp_bind(1, 0);
    EXPECT(s.valid());
    if (!s.valid()) return;
    const unsigned short port = local_port(s.get());
    const Bytes q = example_query();

    ThreadJoin th(std::thread(udp_fake_once, s.get(), UdpReply([](const Bytes& in) {
        Bytes wrong = with_qr(in);
        wrong[0] = 0x99;
        wrong[1] = 0x99;
        wrong.push_back(0xCD);                     // marker: the wrong one
        Bytes right = with_qr(in);
        right.push_back(0xAB);                     // marker: the correct one
        return std::vector<Bytes>{wrong, right};
    })));

    PlainTransport t(port);
    QueryResult r = t.query_legacy(servers({L"127.0.0.1"}), q, 1000);
    EXPECT(r.ok);
    EXPECT(is_response_to(r.response, q));
    EXPECT(!r.response.empty() && r.response.back() == 0xAB);
}

// (5) UDP answer has TC set -> the full 100-byte TCP answer (split frame) is returned.
void test_truncated_udp_retries_tcp() {
    Sock udp = udp_bind(1, 0);
    EXPECT(udp.valid());
    if (!udp.valid()) return;
    const unsigned short port = local_port(udp.get());
    Sock tcp = tcp_listen(port);
    EXPECT(tcp.valid());
    if (!tcp.valid()) return;

    const Bytes q = example_query();

    Bytes full(100);
    full[0] = q[0];
    full[1] = q[1];
    full[2] = 0x81;                                // QR | RD
    full[3] = 0x80;                                // RA
    full[4] = 0x00;
    full[5] = 0x01;
    for (std::size_t i = 12; i < full.size(); ++i) full[i] = static_cast<std::uint8_t>(i);

    ThreadJoin tcp_th(std::thread(tcp_fake_once, tcp.get(), full));
    ThreadJoin udp_th(std::thread(udp_fake_once, udp.get(), UdpReply([](const Bytes& in) {
        return std::vector<Bytes>{with_qr(in, true)};   // TC bit set, short answer
    })));

    PlainTransport t(port);
    QueryResult r = t.query_legacy(servers({L"127.0.0.1"}), q, 1000);
    EXPECT(r.ok);
    EXPECT(r.response.size() == 100);
    EXPECT(r.response == full);
}

// (6a) Query shorter than 12 bytes -> ok=false immediately, no network.
void test_short_query_rejected() {
    Sock s = udp_bind(1, 0);           // silent: if the code wrongly sent, it would wait
    EXPECT(s.valid());
    if (!s.valid()) return;
    const unsigned short port = local_port(s.get());

    PlainTransport t(port);
    const Bytes short_q(11, 0x00);
    const auto t0 = std::chrono::steady_clock::now();
    QueryResult r = t.query_legacy(servers({L"127.0.0.1"}), short_q, 5000);
    const long long ms = elapsed_ms(t0);
    EXPECT(!r.ok);
    EXPECT(!r.error.empty());
    EXPECT(ms < 1000);
}

// (6b) Empty server list -> ok=false.
void test_empty_servers_rejected() {
    PlainTransport t(9);
    const Bytes q = example_query();
    const auto t0 = std::chrono::steady_clock::now();
    QueryResult r = t.query_legacy(std::vector<std::wstring>{}, q, 5000);
    const long long ms = elapsed_ms(t0);
    EXPECT(!r.ok);
    EXPECT(!r.error.empty());
    EXPECT(ms < 1000);
}

// (6c) query() is not supported -> always ok=false.
void test_query_unsupported() {
    PlainTransport t(9);
    const Bytes q = example_query();
    Upstream u{L"quad9", Method::DoH};
    QueryResult r = t.query(u, q, 500);
    EXPECT(!r.ok);
    EXPECT(!r.error.empty());
}

} // namespace

int main() {
    WsaGuard wsa;
    EXPECT(wsa.ok);

    test_udp_answer();
    test_bad_first_server_skipped();
    test_silent_server_times_out();
    test_silent_then_working_server();
    test_wrong_id_ignored();
    test_truncated_udp_retries_tcp();
    test_short_query_rejected();
    test_empty_servers_rejected();
    test_query_unsupported();

    if (g_failed != 0) {
        wprintf(L"%d check(s) failed\n", g_failed);
        return 1;
    }
    wprintf(L"test_dns_plain: all checks passed\n");
    return 0;
}
