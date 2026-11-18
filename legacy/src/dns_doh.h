#pragma once

// dns_doh.h — DNS-over-HTTPS client (RFC 8484, POST application/dns-message)
// on WinHTTP. Windows 7 SP1 needs KB3140245 for TLS 1.2 in WinHTTP; without it
// tls12_available() is false and every query fails with a clear message.
//
// Rules: no system proxy, no redirects, strict certificate validation (no
// ignore-error flags), one TLS 1.2-only session shared by all queries.
// Design: docs/design/2026-11-18-legacy-dns-stub.md

#include "dns_transport.h"
#include <string>

namespace netshutup::dns {

struct UrlParts {
    std::wstring   host;     // brackets of an IPv6 literal are removed
    unsigned short port = 443;
    std::wstring   path;     // always starts with '/', includes any query string
};

// https:// only. Rejects: other schemes, empty host, userinfo ('@'), port 0 or > 65535
// or non-numeric, whitespace anywhere. Missing path -> "/". Pure.
bool parse_https_url(const std::wstring& url, UrlParts& out);

// True if a Content-Type value is "application/dns-message" (case-insensitive,
// optional parameters after ';' and surrounding spaces allowed). Pure.
bool is_dns_message_content_type(const std::wstring& value);

class DohTransport final : public Transport {
public:
    DohTransport();
    ~DohTransport() override;
    DohTransport(const DohTransport&) = delete;
    DohTransport& operator=(const DohTransport&) = delete;

    // True if the WinHTTP session was opened and accepted TLS 1.2-only.
    bool tls12_available() const { return tls12_ok_; }

    // Only Method::DoH upstreams; DoT or unknown provider -> ok=false with a reason.
    // Success requires HTTP 200 + Content-Type application/dns-message and a body of
    // 12..65535 bytes.
    QueryResult query(const Upstream& upstream, const packet::Bytes& wire,
                      unsigned timeout_ms) override;

    // Not supported by this transport (plain DNS lives elsewhere): always ok=false.
    QueryResult query_legacy(const std::vector<std::wstring>& servers,
                             const packet::Bytes& wire, unsigned timeout_ms) override;

private:
    void* session_ = nullptr;   // HINTERNET
    bool  tls12_ok_ = false;
};

} // namespace netshutup::dns
