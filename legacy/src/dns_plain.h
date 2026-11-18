#pragma once

// dns_plain.h — plain (unencrypted) DNS transport, used ONLY as the last-resort
// fall-back to the original DNS servers (Transport::query_legacy).
// Design: docs/design/2026-11-18-legacy-dns-stub.md

#include "dns_transport.h"
#include <memory>

namespace netshutup::dns {

class PlainTransport final : public Transport {
public:
    // `port` is 53 in production; tests pass an ephemeral loopback port.
    explicit PlainTransport(unsigned short port = 53);
    ~PlainTransport() override;
    PlainTransport(const PlainTransport&) = delete;
    PlainTransport& operator=(const PlainTransport&) = delete;

    // Not supported: always ok=false ("plain DNS only serves as legacy fall-back").
    QueryResult query(const Upstream& upstream, const packet::Bytes& wire,
                      unsigned timeout_ms) override;

    // For each server (IPv4 text) in order: send `wire` over UDP to server:port and wait
    // up to timeout_ms for a datagram whose ID equals the query ID and whose QR bit is
    // set (other datagrams are ignored until the timeout). If that response has the TC
    // bit, repeat the query over TCP (2-byte length framing, FrameAssembler) to the same
    // server and return the TCP answer. The first server that gives an answer wins.
    // Invalid server text, a query shorter than 12 bytes, or all servers failing ->
    // ok=false with a short English reason.
    QueryResult query_legacy(const std::vector<std::wstring>& servers,
                             const packet::Bytes& wire, unsigned timeout_ms) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace netshutup::dns
