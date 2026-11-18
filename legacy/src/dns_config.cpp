// dns_config.cpp — encrypted-DNS provider table, upstream chains and dns.cfg text format.
// Pure logic: no I/O, no Windows calls. Spec: dns_config.h.

#include "dns_config.h"

#include <cstddef>

namespace netshutup::dns {

namespace {

const Provider kProviders[] = {
    {L"cloudflare", L"Cloudflare", L"1.1.1.1", L"1.0.0.1", L"https://1.1.1.1/dns-query", L"one.one.one.one", 853},
    {L"google",     L"Google",     L"8.8.8.8", L"8.8.4.4", L"https://8.8.8.8/dns-query", L"dns.google", 853},
    {L"quad9",      L"Quad9",      L"9.9.9.9", L"149.112.112.112", L"https://9.9.9.9/dns-query", L"dns.quad9.net", 853},
};

constexpr const char* kHeader = "NETSHUTUP_DNS_V1";

// Config text is ASCII (provider ids, IPv4 literals, keywords), so a plain
// narrowing/widening per character is exact for every value this module produces.
std::string to_narrow(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (wchar_t ch : w) s.push_back(static_cast<char>(ch));
    return s;
}

std::wstring to_wide(const std::string& s) {
    std::wstring w;
    w.reserve(s.size());
    for (char ch : s) w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(ch)));
    return w;
}

// Splits on single spaces exactly; empty tokens are kept so that malformed
// spacing makes the line fail validation.
std::vector<std::string> split_spaces(const std::string& line) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (;;) {
        std::size_t sp = line.find(' ', start);
        if (sp == std::string::npos) {
            out.push_back(line.substr(start));
            break;
        }
        out.push_back(line.substr(start, sp - start));
        start = sp + 1;
    }
    return out;
}

// Decimal value of 1..3 ASCII digits, must be <= max_value.
bool parse_decimal(const std::string& s, unsigned max_value, unsigned& out) {
    if (s.empty() || s.size() > 3) return false;
    unsigned v = 0;
    for (char ch : s) {
        if (ch < '0' || ch > '9') return false;
        v = v * 10 + static_cast<unsigned>(ch - '0');
    }
    if (v > max_value) return false;
    out = v;
    return true;
}

// Four decimal octets 0-255; returns canonical "a.b.c.d" text.
bool parse_ipv4(const std::string& s, std::wstring& out) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (;;) {
        std::size_t dot = s.find('.', start);
        if (dot == std::string::npos) {
            parts.push_back(s.substr(start));
            break;
        }
        parts.push_back(s.substr(start, dot - start));
        start = dot + 1;
    }
    if (parts.size() != 4) return false;

    std::wstring result;
    for (std::size_t i = 0; i < 4; ++i) {
        unsigned octet = 0;
        if (!parse_decimal(parts[i], 255, octet)) return false;
        if (i > 0) result.push_back(L'.');
        result += std::to_wstring(octet);
    }
    out = result;
    return true;
}

} // namespace

const Provider* find_provider(const std::wstring& id) {
    for (const Provider& p : kProviders) {
        if (id == p.id) return &p;
    }
    return nullptr;
}

std::vector<Upstream> chain_for_profile(const std::wstring& profile_name,
                                        const std::wstring& fastest_first) {
    const wchar_t* a = nullptr;
    const wchar_t* b = nullptr;

    if (profile_name == L"privacy") {
        a = L"quad9";
        b = L"cloudflare";
    } else if (profile_name == L"recommended") {
        a = L"cloudflare";
        b = L"google";
    } else if (profile_name == L"performance") {
        if (fastest_first == L"google") {
            a = L"google";
            b = L"cloudflare";
        } else {
            a = L"cloudflare";
            b = L"google";
        }
    } else {
        return {};
    }

    std::vector<Upstream> chain;
    for (const wchar_t* id : {a, b}) {
        Upstream doh;
        doh.provider_id = id;
        doh.method = Method::DoH;
        chain.push_back(doh);

        Upstream dot;
        dot.provider_id = id;
        dot.method = Method::DoT;
        chain.push_back(dot);
    }
    return chain;
}

std::string serialize_config(const Config& c) {
    std::string out = std::string(kHeader) + "\n";
    out += "FAIL_THRESHOLD " + std::to_string(c.fail_threshold) + "\n";
    for (const Upstream& u : c.chain) {
        out += "UPSTREAM " + to_narrow(u.provider_id) +
               (u.method == Method::DoH ? " doh\n" : " dot\n");
    }
    for (const std::wstring& ip : c.legacy_dns) {
        out += "LEGACY " + to_narrow(ip) + "\n";
    }
    out += "END\n";
    return out;
}

std::optional<Config> parse_config(const std::string& text) {
    std::size_t pos = 0;
    // Returns the next line without its terminating LF or CRLF.
    auto next_line = [&](std::string& line) -> bool {
        if (pos >= text.size()) return false;
        std::size_t nl = text.find('\n', pos);
        std::size_t end = (nl == std::string::npos) ? text.size() : nl;
        line = text.substr(pos, end - pos);
        pos = (nl == std::string::npos) ? text.size() : nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return true;
    };

    std::string line;
    if (!next_line(line) || line != kHeader) return std::nullopt;

    Config config;
    config.chain.clear();
    config.fail_threshold = 3;
    bool seen_threshold = false;
    bool ended = false;

    while (next_line(line)) {
        if (ended) {
            // Only blank lines may follow END.
            if (line.empty()) continue;
            return std::nullopt;
        }

        std::vector<std::string> tokens = split_spaces(line);
        const std::string& keyword = tokens[0];

        if (keyword == "END") {
            if (tokens.size() != 1) return std::nullopt;
            ended = true;
        } else if (keyword == "FAIL_THRESHOLD") {
            if (tokens.size() != 2 || seen_threshold) return std::nullopt;
            unsigned value = 0;
            if (!parse_decimal(tokens[1], 100, value) || value < 1) return std::nullopt;
            config.fail_threshold = value;
            seen_threshold = true;
        } else if (keyword == "UPSTREAM") {
            if (tokens.size() != 3) return std::nullopt;
            std::wstring id = to_wide(tokens[1]);
            if (find_provider(id) == nullptr) return std::nullopt;

            Upstream u;
            u.provider_id = id;
            if (tokens[2] == "doh") {
                u.method = Method::DoH;
            } else if (tokens[2] == "dot") {
                u.method = Method::DoT;
            } else {
                return std::nullopt;
            }
            config.chain.push_back(u);
        } else if (keyword == "LEGACY") {
            if (tokens.size() != 2) return std::nullopt;
            std::wstring ip;
            if (!parse_ipv4(tokens[1], ip)) return std::nullopt;
            config.legacy_dns.push_back(ip);
        } else {
            return std::nullopt;
        }
    }

    if (!ended) return std::nullopt;
    if (config.chain.empty()) return std::nullopt;
    return config;
}

} // namespace netshutup::dns
