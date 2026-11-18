// dns_server.cpp — UDP + TCP listener in front of dns::forward() (see dns_server.h).

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mstcpip.h>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

#include "dns_server.h"
#include "dns_forward.h"
#include "dns_frame.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace netshutup::dns {

struct Server::Impl {
    Config         cfg;
    Transport&     transport;
    ServerOptions  opts;
    Failover       failover;

    bool           wsa_ok = false;
    SOCKET         udp = INVALID_SOCKET;
    SOCKET         tcp = INVALID_SOCKET;
    unsigned short port = 0;
    std::atomic<bool> stopping{false};
    bool           started = false;

    std::vector<std::thread> udp_threads;
    std::thread              accept_thread;

    std::atomic<unsigned>    tcp_clients{0};
    std::mutex               clients_mu;
    std::set<SOCKET>         client_sockets;

    std::atomic<unsigned long long> n_queries{0}, n_encrypted{0}, n_legacy{0}, n_servfail{0};
    mutable std::mutex       info_mu;
    std::optional<Upstream>  last_upstream;
    std::wstring             last_error;

    Impl(const Config& c, Transport& t, const ServerOptions& o)
        : cfg(c), transport(t), opts(o),
          failover(c.chain.size(), c.fail_threshold, !c.legacy_dns.empty()) {
        opts.udp_workers = std::min(std::max(opts.udp_workers, 1u), 16u);
        WSADATA wsa;
        wsa_ok = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    }
    ~Impl() { if (wsa_ok) WSACleanup(); }

    void record(const ForwardOutcome& o) {
        n_queries++;
        std::lock_guard<std::mutex> l(info_mu);
        switch (o.source) {
            case Source::Encrypted: n_encrypted++; last_upstream = o.used; break;
            case Source::Legacy:    n_legacy++;    last_error = L"encrypted upstreams failing; using the original DNS"; break;
            case Source::Servfail:  n_servfail++;  last_error = L"no upstream answered"; break;
            case Source::Invalid:   break;
        }
    }

    void udp_loop(SOCKET s) {
        std::vector<unsigned char> buf(4096);
        while (!stopping.load()) {
            sockaddr_in from{};
            int from_len = sizeof(from);
            int n = recvfrom(s, reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()), 0,
                             reinterpret_cast<sockaddr*>(&from), &from_len);
            if (n < 0) {
                int e = WSAGetLastError();
                if (stopping.load() || e == WSAENOTSOCK || e == WSAEINTR || e == WSAEINVAL) break;
                continue;   // e.g. WSAECONNRESET from an ICMP error: ignore
            }
            if (n == 0) continue;
            packet::Bytes q(buf.begin(), buf.begin() + n);
            ForwardOutcome out = forward(q, cfg, failover, transport, opts.timeout_ms);
            record(out);
            if (out.response.empty()) continue;   // invalid query: drop silently
            packet::Bytes resp = packet::truncate_for_udp(out.response);
            sendto(s, reinterpret_cast<const char*>(resp.data()), static_cast<int>(resp.size()), 0,
                   reinterpret_cast<sockaddr*>(&from), from_len);
        }
    }

    void tcp_client(SOCKET c) {
        DWORD t = 5000;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
        FrameAssembler frames(65535);
        unsigned char buf[4096];
        bool alive = true;
        while (alive && !stopping.load()) {
            int n = recv(c, reinterpret_cast<char*>(buf), static_cast<int>(sizeof(buf)), 0);
            if (n <= 0) break;   // closed, timeout, or error
            if (!frames.push(buf, static_cast<std::size_t>(n))) break;
            while (auto q = frames.next()) {
                ForwardOutcome out = forward(*q, cfg, failover, transport, opts.timeout_ms);
                record(out);
                if (out.response.empty()) { alive = false; break; }
                packet::Bytes framed = packet::frame_tcp(out.response);
                if (framed.empty()) { alive = false; break; }
                std::size_t sent = 0;
                while (sent < framed.size()) {
                    int m = send(c, reinterpret_cast<const char*>(framed.data() + sent),
                                 static_cast<int>(framed.size() - sent), 0);
                    if (m <= 0) { alive = false; break; }
                    sent += static_cast<std::size_t>(m);
                }
                if (!alive) break;
            }
        }
        {
            std::lock_guard<std::mutex> l(clients_mu);
            client_sockets.erase(c);
        }
        closesocket(c);
        tcp_clients--;   // last touch of *this
    }

    void accept_loop(SOCKET listener) {
        while (!stopping.load()) {
            SOCKET c = accept(listener, nullptr, nullptr);
            if (c == INVALID_SOCKET) {
                int e = WSAGetLastError();
                if (stopping.load() || e == WSAENOTSOCK || e == WSAEINTR || e == WSAEINVAL) break;
                continue;
            }
            if (tcp_clients.load() >= kMaxTcpClients) { closesocket(c); continue; }
            tcp_clients++;
            {
                std::lock_guard<std::mutex> l(clients_mu);
                client_sockets.insert(c);
            }
            try {
                std::thread([this, c] { tcp_client(c); }).detach();
            } catch (...) {
                {
                    std::lock_guard<std::mutex> l(clients_mu);
                    client_sockets.erase(c);
                }
                closesocket(c);
                tcp_clients--;
            }
        }
    }

    static std::wstring bind_error(int e, unsigned short p) {
        if (e == WSAEADDRINUSE || e == WSAEACCES) return L"port " + std::to_wstring(p) + L" is already in use";
        return L"could not bind the DNS listener (Winsock error " + std::to_wstring(e) + L")";
    }

    bool bind_sockets(unsigned short want_port, std::wstring& err) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        if (InetPtonW(AF_INET, opts.bind_ip.c_str(), &addr.sin_addr) != 1) { err = L"bad bind address"; return false; }

        for (int attempt = 0; attempt < 5; ++attempt) {
            udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (udp == INVALID_SOCKET) { err = L"could not create UDP socket"; return false; }
            BOOL excl = TRUE;
            setsockopt(udp, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof(excl));
            // Do not let ICMP "port unreachable" from vanished clients break recvfrom.
            BOOL no_reset = FALSE;
            DWORD ret = 0;
            WSAIoctl(udp, SIO_UDP_CONNRESET, &no_reset, sizeof(no_reset), nullptr, 0, &ret, nullptr, nullptr);

            addr.sin_port = htons(want_port);
            if (bind(udp, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
                err = bind_error(WSAGetLastError(), want_port);
                closesocket(udp); udp = INVALID_SOCKET;
                return false;
            }
            sockaddr_in bound{};
            int blen = sizeof(bound);
            getsockname(udp, reinterpret_cast<sockaddr*>(&bound), &blen);
            port = ntohs(bound.sin_port);

            tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (tcp == INVALID_SOCKET) { err = L"could not create TCP socket"; closesocket(udp); udp = INVALID_SOCKET; return false; }
            setsockopt(tcp, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof(excl));
            addr.sin_port = htons(port);
            if (bind(tcp, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
                listen(tcp, 16) == SOCKET_ERROR) {
                int e = WSAGetLastError();
                closesocket(tcp); tcp = INVALID_SOCKET;
                closesocket(udp); udp = INVALID_SOCKET;
                if (want_port == 0 && attempt < 4) continue;   // ephemeral UDP port was taken on TCP: try another
                err = bind_error(e, port);
                return false;
            }
            return true;
        }
        err = L"could not find a free port";
        return false;
    }
};

Server::Server(const Config& cfg, Transport& transport, const ServerOptions& opts)
    : impl_(new Impl(cfg, transport, opts)) {}

Server::~Server() { stop(); }

bool Server::start(std::wstring& err) {
    Impl& d = *impl_;
    if (d.started) return true;
    if (!d.wsa_ok) { err = L"Winsock could not be started"; return false; }
    if (d.cfg.chain.empty()) { err = L"no upstreams configured"; return false; }
    if (!d.bind_sockets(d.opts.port, err)) return false;

    d.stopping = false;
    try {
        for (unsigned i = 0; i < d.opts.udp_workers; ++i) {
            d.udp_threads.emplace_back([&d, s = d.udp] { d.udp_loop(s); });
        }
        d.accept_thread = std::thread([&d, s = d.tcp] { d.accept_loop(s); });
    } catch (...) {
        d.started = true;   // let stop() clean up whatever did start
        stop();
        err = L"could not start worker threads";
        return false;
    }
    d.started = true;
    return true;
}

void Server::stop() {
    Impl& d = *impl_;
    d.stopping = true;
    if (d.udp != INVALID_SOCKET) { closesocket(d.udp); d.udp = INVALID_SOCKET; }
    if (d.tcp != INVALID_SOCKET) { closesocket(d.tcp); d.tcp = INVALID_SOCKET; }
    {
        std::lock_guard<std::mutex> l(d.clients_mu);
        for (SOCKET c : d.client_sockets) shutdown(c, SD_BOTH);   // unblocks recv; handler closes it
    }
    for (auto& t : d.udp_threads) if (t.joinable()) t.join();
    d.udp_threads.clear();
    if (d.accept_thread.joinable()) d.accept_thread.join();
    while (d.tcp_clients.load() > 0) Sleep(5);   // handlers are detached; wait until all are done
    d.started = false;
}

unsigned short Server::bound_port() const { return impl_->started ? impl_->port : 0; }

ServiceStatus Server::status() const {
    const Impl& d = *impl_;
    ServiceStatus s;
    s.mode = d.failover.mode();
    s.queries = d.n_queries.load();
    s.encrypted = d.n_encrypted.load();
    s.legacy = d.n_legacy.load();
    s.servfail = d.n_servfail.load();
    std::lock_guard<std::mutex> l(d.info_mu);
    s.upstream = d.last_upstream;
    s.last_error = d.last_error;
    return s;
}

} // namespace netshutup::dns
