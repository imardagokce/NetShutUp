#pragma once

// dns_server.h — local DNS listener (UDP + TCP) that answers every client query
// through dns::forward() (encrypted chain, then legacy fall-back).
//
// Threads: `udp_workers` threads share the UDP socket (recvfrom -> forward ->
// sendto); one TCP accept thread spawns a short-lived handler thread per
// connection (at most kMaxTcpClients at once; extra connections are closed).
// Win32/Winsock only; no atomic_wait/latch/barrier/semaphore.
// Design: docs/design/2026-11-18-legacy-dns-stub.md

#include "dns_config.h"
#include "dns_failover.h"
#include "dns_status.h"
#include "dns_transport.h"
#include <memory>
#include <string>

namespace netshutup::dns {

struct ServerOptions {
    std::wstring   bind_ip = L"127.0.0.1";
    unsigned short port = 53;          // 0 = pick a free port (tests); see bound_port()
    unsigned       timeout_ms = 3000;  // per upstream attempt
    unsigned       udp_workers = 8;    // clamped to 1..16
};

class Server {
public:
    static constexpr unsigned kMaxTcpClients = 32;

    // `transport` must outlive the Server and be safe to call from several threads.
    Server(const Config& cfg, Transport& transport, const ServerOptions& opts);
    ~Server();   // stop()
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Binds UDP and TCP on bind_ip:port and starts the threads. false + `err`
    // (short English reason, e.g. "port 53 is already in use") on failure; nothing
    // stays bound in that case. The TCP socket uses the UDP socket's port when port==0.
    bool start(std::wstring& err);

    // Closes the sockets, joins all threads. Idempotent; safe if start() failed.
    void stop();

    unsigned short bound_port() const;            // 0 before a successful start()
    ServiceStatus  status() const;                // snapshot incl. counters; updated_utc left empty

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace netshutup::dns
