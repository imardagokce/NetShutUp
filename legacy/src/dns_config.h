#pragma once

// dns_config.h — encrypted-DNS provider table, upstream chain per profile and
// the dns.cfg text format (pure: no I/O, no Windows calls).
// Design: docs/design/2026-11-18-legacy-dns-stub.md

#include <optional>
#include <string>
#include <vector>

namespace netshutup::dns {

enum class Method { DoH, DoT };

struct Provider {
    const wchar_t* id;             // L"cloudflare" | L"google" | L"quad9"
    const wchar_t* name;           // display name
    const wchar_t* ipv4_primary;
    const wchar_t* ipv4_secondary;
    const wchar_t* doh_url;        // IP-literal URL, e.g. L"https://1.1.1.1/dns-query"
    unsigned short dot_port;       // 853
};

// Table: cloudflare 1.1.1.1 / 1.0.0.1, google 8.8.8.8 / 8.8.4.4,
//        quad9 9.9.9.9 / 149.112.112.112; DoH URL = https://<primary>/dns-query.
const Provider* find_provider(const std::wstring& id);   // nullptr if unknown

struct Upstream {
    std::wstring provider_id;
    Method       method = Method::DoH;
    bool operator==(const Upstream& o) const { return provider_id == o.provider_id && method == o.method; }
};

// Chain for a profile ("privacy" | "performance" | "recommended"), empty vector if
// the profile name is unknown. Provider order A then B:
//   privacy:      quad9, cloudflare
//   recommended:  cloudflare, google
//   performance:  cloudflare, google — unless `fastest_first` names cloudflare or
//                 google, in which case that provider is A (the other is B).
// Chain = (A,DoH), (A,DoT), (B,DoH), (B,DoT).
std::vector<Upstream> chain_for_profile(const std::wstring& profile_name,
                                        const std::wstring& fastest_first = L"");

struct Config {
    std::vector<Upstream>    chain;
    std::vector<std::wstring> legacy_dns;      // original IPv4 DNS servers; may be empty
    unsigned                 fail_threshold = 3;   // 1..100
};

// dns.cfg text (UTF-8, LF line endings):
//   NETSHUTUP_DNS_V1
//   FAIL_THRESHOLD 3
//   UPSTREAM quad9 doh
//   UPSTREAM quad9 dot
//   LEGACY 192.168.1.1
//   END
// (one UPSTREAM / LEGACY line per item, in order; zero LEGACY lines allowed)
std::string serialize_config(const Config& c);

// Strict parser: returns nullopt on a missing/wrong header, missing END, unknown
// provider or method, malformed IPv4 in LEGACY (four decimal octets 0-255),
// FAIL_THRESHOLD outside 1..100, an empty chain, unknown keywords, or trailing
// content after END. Accepts CRLF as well as LF.
std::optional<Config> parse_config(const std::string& text);

} // namespace netshutup::dns
