#pragma once

// dns_transport.h — abstract way to send one DNS message upstream.
// Real implementations: DohTransport (dns_doh.h), later DotTransport.
// Tests use fakes. Design: docs/design/2026-11-18-legacy-dns-stub.md

#include "dns_config.h"
#include "dns_packet.h"
#include <string>
#include <vector>

namespace netshutup::dns {

struct QueryResult {
    bool               ok = false;
    packet::Bytes      response;     // valid only when ok
    std::wstring       error;        // short English reason when !ok
};

class Transport {
public:
    virtual ~Transport() = default;

    // Sends `wire` (a complete DNS message) to the encrypted upstream and returns
    // the raw response message. Must not throw.
    virtual QueryResult query(const Upstream& upstream,
                              const packet::Bytes& wire,
                              unsigned timeout_ms) = 0;

    // Sends `wire` as plain DNS (UDP, TCP retry on truncation) to the first of
    // `servers` (IPv4 strings) that answers. Used only in legacy fall-back.
    virtual QueryResult query_legacy(const std::vector<std::wstring>& servers,
                                     const packet::Bytes& wire,
                                     unsigned timeout_ms) = 0;
};

} // namespace netshutup::dns
