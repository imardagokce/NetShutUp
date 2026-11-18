#pragma once

// dns_real_transport.h — the production Transport: routes each upstream to the DoH
// (WinHTTP), DoT (Schannel) or plain-DNS client. Used by the DNS service, the
// foreground runner and the apply-time probe.

#include "dns_doh.h"
#include "dns_dot.h"
#include "dns_plain.h"

namespace netshutup::dns {

class RealTransport final : public Transport {
public:
    explicit RealTransport(unsigned short plain_port = 53) : plain_(plain_port) {}

    QueryResult query(const Upstream& u, const packet::Bytes& wire, unsigned timeout_ms) override {
        return u.method == Method::DoH ? doh_.query(u, wire, timeout_ms) : dot_.query(u, wire, timeout_ms);
    }
    QueryResult query_legacy(const std::vector<std::wstring>& servers, const packet::Bytes& wire,
                             unsigned timeout_ms) override {
        return plain_.query_legacy(servers, wire, timeout_ms);
    }
    bool tls12_available() const { return doh_.tls12_available(); }

private:
    DohTransport   doh_;
    DotTransport   dot_;
    PlainTransport plain_;
};

} // namespace netshutup::dns
