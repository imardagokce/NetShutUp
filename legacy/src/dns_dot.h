#pragma once

// dns_dot.h — DNS-over-TLS client (RFC 7858): TLS 1.2 via Schannel (SSPI) over a
// Winsock TCP connection to port 853. We connect by IP address and pass the
// provider's DNS name (Provider::dot_host) to Schannel so the certificate is
// verified against it (no bootstrap name resolution, no ignored errors).
//
// One cached connection per provider, serialised by a mutex: DoT is the second
// method in the chain, so simplicity beats parallelism. A failed use of a cached
// connection is retried once on a fresh one (the server may have closed it idle).
// Design: docs/design/2026-11-18-legacy-dns-stub.md

#include "dns_transport.h"
#include <memory>

namespace netshutup::dns {

class DotTransport final : public Transport {
public:
    DotTransport();
    ~DotTransport() override;
    DotTransport(const DotTransport&) = delete;
    DotTransport& operator=(const DotTransport&) = delete;

    // Only Method::DoT upstreams; DoH or unknown provider -> ok=false with a reason.
    QueryResult query(const Upstream& upstream, const packet::Bytes& wire,
                      unsigned timeout_ms) override;

    // Not supported by this transport: always ok=false.
    QueryResult query_legacy(const std::vector<std::wstring>& servers,
                             const packet::Bytes& wire, unsigned timeout_ms) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace netshutup::dns
