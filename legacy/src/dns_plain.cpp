// dns_plain.cpp — plain DNS (UDP, TCP retry on truncation) used only as the last-resort
// fall-back to the original DNS servers. See dns_plain.h for the contract.

#include <winsock2.h>
#include <ws2tcpip.h>

#include "dns_plain.h"
#include "dns_frame.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace netshutup::dns {

namespace {

using Clock = std::chrono::steady_clock;

// Closes its socket on destruction.
class SocketGuard {
public:
    explicit SocketGuard(SOCKET s) : s_(s) {}
    ~SocketGuard() {
        if (s_ != INVALID_SOCKET) closesocket(s_);
    }
    SocketGuard(const SocketGuard&) = delete;
    SocketGuard& operator=(const SocketGuard&) = delete;
    bool valid() const { return s_ != INVALID_SOCKET; }
    SOCKET get() const { return s_; }

private:
    SOCKET s_;
};

long long ms_left(Clock::time_point deadline) {
    const auto d = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
    return d.count() > 0 ? static_cast<long long>(d.count()) : 0;
}

timeval to_timeval(long long ms) {
    timeval tv{};
    tv.tv_sec = static_cast<long>(ms / 1000);
    tv.tv_usec = static_cast<long>((ms % 1000) * 1000);
    return tv;
}

bool wait_readable(SOCKET s, long long ms) {
    fd_set rset;
    FD_ZERO(&rset);
    FD_SET(s, &rset);
    timeval tv = to_timeval(ms);
    return select(0, &rset, nullptr, nullptr, &tv) > 0;
}

// True when the socket became writable or reported an error (non-blocking connect).
bool wait_writable_or_error(SOCKET s, long long ms) {
    fd_set wset;
    fd_set eset;
    FD_ZERO(&wset);
    FD_ZERO(&eset);
    FD_SET(s, &wset);
    FD_SET(s, &eset);
    timeval tv = to_timeval(ms);
    return select(0, nullptr, &wset, &eset, &tv) > 0;
}

// A reply counts only if it carries our ID and has QR set.
bool is_matching_response(const std::uint8_t* data, std::size_t len, std::uint16_t id) {
    if (len < 12) return false;
    const auto got = packet::get_id(data, len);
    return got && *got == id && (data[2] & 0x80) != 0;
}

// UDP exchange with one server. Returns an empty string on success (out filled),
// otherwise a short English reason.
std::wstring udp_exchange(const sockaddr_in& server, const packet::Bytes& wire,
                          std::uint16_t id, unsigned timeout_ms, packet::Bytes& out) {
    SocketGuard s(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (!s.valid()) return L"cannot create UDP socket";

    const int len = static_cast<int>(wire.size());
    if (sendto(s.get(), reinterpret_cast<const char*>(wire.data()), len, 0,
               reinterpret_cast<const sockaddr*>(&server), sizeof(server)) != len) {
        return L"UDP send failed";
    }

    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    std::vector<std::uint8_t> buf(65535);
    for (;;) {
        const long long left = ms_left(deadline);
        if (left <= 0 || !wait_readable(s.get(), left)) {
            return L"no UDP answer before timeout";
        }
        const int n = recv(s.get(), reinterpret_cast<char*>(buf.data()),
                           static_cast<int>(buf.size()), 0);
        if (n == SOCKET_ERROR) return L"UDP receive failed";
        if (!is_matching_response(buf.data(), static_cast<std::size_t>(n), id)) continue;
        out.assign(buf.begin(), buf.begin() + n);
        return L"";
    }
}

// TCP exchange with one server (used after a truncated UDP answer). Same contract as
// udp_exchange. Frames are reassembled with FrameAssembler; stray messages are ignored.
std::wstring tcp_exchange(const sockaddr_in& server, const packet::Bytes& wire,
                          std::uint16_t id, unsigned timeout_ms, packet::Bytes& out) {
    const packet::Bytes framed = packet::frame_tcp(wire);
    if (framed.empty()) return L"query too large for TCP";

    SocketGuard s(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (!s.valid()) return L"cannot create TCP socket";

    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);

    // Non-blocking connect, bounded by select().
    u_long nonblocking = 1;
    ioctlsocket(s.get(), FIONBIO, &nonblocking);
    if (connect(s.get(), reinterpret_cast<const sockaddr*>(&server), sizeof(server)) == SOCKET_ERROR) {
        if (WSAGetLastError() != WSAEWOULDBLOCK) return L"TCP connect failed";
        if (!wait_writable_or_error(s.get(), ms_left(deadline))) return L"TCP connect timed out";
        int so_error = 0;
        int opt_len = sizeof(so_error);
        if (getsockopt(s.get(), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &opt_len) != 0 ||
            so_error != 0) {
            return L"TCP connect failed";
        }
    }
    u_long blocking = 0;
    ioctlsocket(s.get(), FIONBIO, &blocking);

    const long long send_left = ms_left(deadline);
    if (send_left <= 0) return L"TCP connect timed out";
    DWORD send_timeout = static_cast<DWORD>(send_left);
    setsockopt(s.get(), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&send_timeout),
               sizeof(send_timeout));
    const int framed_len = static_cast<int>(framed.size());
    if (send(s.get(), reinterpret_cast<const char*>(framed.data()), framed_len, 0) != framed_len) {
        return L"TCP send failed";
    }

    FrameAssembler assembler;
    std::vector<std::uint8_t> buf(4096);
    for (;;) {
        for (auto msg = assembler.next(); msg; msg = assembler.next()) {
            if (is_matching_response(msg->data(), msg->size(), id)) {
                out = std::move(*msg);
                return L"";
            }
        }

        const long long left = ms_left(deadline);
        if (left <= 0) return L"no TCP answer before timeout";
        DWORD recv_timeout = static_cast<DWORD>(left);
        setsockopt(s.get(), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&recv_timeout),
                   sizeof(recv_timeout));
        const int n = recv(s.get(), reinterpret_cast<char*>(buf.data()),
                           static_cast<int>(buf.size()), 0);
        if (n <= 0) return L"TCP connection closed or timed out before answer";
        if (!assembler.push(buf.data(), static_cast<std::size_t>(n))) return L"malformed TCP answer";
    }
}

} // namespace

struct PlainTransport::Impl {
    unsigned short port = 53;
    bool           wsa_ok = false;
};

PlainTransport::PlainTransport(unsigned short port) : impl_(std::make_unique<Impl>()) {
    impl_->port = port;
    WSADATA data{};
    impl_->wsa_ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
}

PlainTransport::~PlainTransport() {
    if (impl_ && impl_->wsa_ok) WSACleanup();
}

QueryResult PlainTransport::query(const Upstream&, const packet::Bytes&, unsigned) {
    QueryResult r;
    r.error = L"plain DNS only serves as legacy fall-back";
    return r;
}

QueryResult PlainTransport::query_legacy(const std::vector<std::wstring>& servers,
                                         const packet::Bytes& wire, unsigned timeout_ms) {
    QueryResult result;
    if (!impl_->wsa_ok) {
        result.error = L"Winsock could not be started";
        return result;
    }
    const auto id = packet::get_id(wire.data(), wire.size());
    if (wire.size() < 12 || !id) {
        result.error = L"query shorter than a DNS header";
        return result;
    }
    if (servers.empty()) {
        result.error = L"no DNS servers to try";
        return result;
    }

    std::wstring last = L"no DNS server answered";
    for (const std::wstring& text : servers) {
        sockaddr_in server{};
        server.sin_family = AF_INET;
        server.sin_port = htons(impl_->port);
        if (InetPtonW(AF_INET, text.c_str(), &server.sin_addr) != 1) {
            last = L"invalid IPv4 server address";
            continue;
        }

        packet::Bytes response;
        std::wstring err = udp_exchange(server, wire, *id, timeout_ms, response);
        if (err.empty() && packet::is_truncated(response.data(), response.size())) {
            err = tcp_exchange(server, wire, *id, timeout_ms, response);
        }
        if (err.empty()) {
            result.ok = true;
            result.response = std::move(response);
            return result;
        }
        last = err;
    }
    result.error = last;
    return result;
}

} // namespace netshutup::dns
