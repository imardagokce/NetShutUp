// dns_status.cpp — serialize / parse / describe for the DNS status file (pure: no I/O).

#include "dns_status.h"

#include <cstddef>
#include <limits>
#include <string>
#include <vector>

namespace netshutup::dns {
namespace {

constexpr const char* kHeader = "NETSHUTUP_DNS_STATUS_V1";

// Replaces CR and LF with spaces so a value always fits on one line.
std::wstring sanitized(std::wstring s) {
    for (auto& ch : s) {
        if (ch == L'\r' || ch == L'\n') ch = L' ';
    }
    return s;
}

void append_utf8(std::string& out, unsigned long cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// wchar_t -> UTF-8. A valid surrogate pair becomes one code point; a lone surrogate
// is written as its own 3-byte sequence so that from_utf8 restores it unchanged.
std::string to_utf8(const std::wstring& w) {
    std::string out;
    for (std::size_t i = 0; i < w.size(); ++i) {
        unsigned long cp = static_cast<unsigned long>(w[i]);
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < w.size()) {
            unsigned long lo = static_cast<unsigned long>(w[i + 1]);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
            }
        }
        append_utf8(out, cp);
    }
    return out;
}

// UTF-8 -> wchar_t. False on malformed input.
bool from_utf8(const std::string& s, std::wstring& out) {
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        unsigned long cp = 0;
        std::size_t extra = 0;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            extra = 2;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            extra = 3;
        } else {
            return false;
        }
        if (extra >= s.size() - i) return false;
        for (std::size_t k = 1; k <= extra; ++k) {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (cp > 0xFFFF && sizeof(wchar_t) < 4) {
            cp -= 0x10000;
            out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<wchar_t>(cp));
        }
        i += extra + 1;
    }
    return true;
}

// Decimal digits only, no sign, no overflow past unsigned long long.
bool parse_counter(const std::string& v, unsigned long long& out) {
    if (v.empty()) return false;
    constexpr unsigned long long kMax = std::numeric_limits<unsigned long long>::max();
    unsigned long long n = 0;
    for (const char c : v) {
        if (c < '0' || c > '9') return false;
        const unsigned long long d = static_cast<unsigned long long>(c - '0');
        if (n > (kMax - d) / 10) return false;
        n = n * 10 + d;
    }
    out = n;
    return true;
}

const char* method_text(Method m) {
    return m == Method::DoH ? "doh" : "dot";
}

const wchar_t* method_wtext(Method m) {
    return m == Method::DoH ? L"doh" : L"dot";
}

} // namespace

std::string serialize_status(const ServiceStatus& s) {
    std::string out;
    out += kHeader;
    out += '\n';
    out += (s.mode == Mode::Encrypted) ? "MODE encrypted\n" : "MODE legacy\n";
    if (s.upstream) {
        out += "UPSTREAM " + to_utf8(s.upstream->provider_id) + ' ' + method_text(s.upstream->method) + '\n';
    }
    out += "QUERIES " + std::to_string(s.queries) + '\n';
    out += "ENCRYPTED " + std::to_string(s.encrypted) + '\n';
    out += "LEGACY " + std::to_string(s.legacy) + '\n';
    out += "SERVFAIL " + std::to_string(s.servfail) + '\n';
    if (!s.last_error.empty()) {
        out += "LAST_ERROR " + to_utf8(sanitized(s.last_error)) + '\n';
    }
    out += "UPDATED " + to_utf8(sanitized(s.updated_utc)) + '\n';
    out += "END\n";
    return out;
}

std::optional<ServiceStatus> parse_status(const std::string& text) {
    // Split on LF; one trailing CR per line is dropped (CRLF accepted).
    std::vector<std::string> lines;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t nl = text.find('\n', pos);
        std::string line = (nl == std::string::npos) ? text.substr(pos) : text.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    if (lines.empty() || lines[0] != kHeader) return std::nullopt;

    ServiceStatus s;
    bool seen_mode = false, seen_upstream = false, seen_queries = false, seen_encrypted = false;
    bool seen_legacy = false, seen_servfail = false, seen_last_error = false, seen_updated = false;
    bool ended = false;

    for (std::size_t i = 1; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        if (ended) {
            if (!line.empty()) return std::nullopt;   // nothing but blank lines after END
            continue;
        }

        // Key is everything before the first space; value is the rest of the line verbatim.
        const std::size_t sp = line.find(' ');
        const std::string key = (sp == std::string::npos) ? line : line.substr(0, sp);
        const std::string value = (sp == std::string::npos) ? std::string() : line.substr(sp + 1);

        if (key == "END") {
            if (sp != std::string::npos) return std::nullopt;
            ended = true;
        } else if (key == "MODE") {
            if (seen_mode) return std::nullopt;
            seen_mode = true;
            if (value == "encrypted") {
                s.mode = Mode::Encrypted;
            } else if (value == "legacy") {
                s.mode = Mode::LegacyFallback;
            } else {
                return std::nullopt;
            }
        } else if (key == "UPSTREAM") {
            if (seen_upstream) return std::nullopt;
            seen_upstream = true;
            const std::size_t msp = value.find(' ');
            if (msp == std::string::npos) return std::nullopt;
            const std::string pid = value.substr(0, msp);
            const std::string method = value.substr(msp + 1);

            Upstream up;
            if (!from_utf8(pid, up.provider_id)) return std::nullopt;
            if (find_provider(up.provider_id) == nullptr) return std::nullopt;
            if (method == "doh") {
                up.method = Method::DoH;
            } else if (method == "dot") {
                up.method = Method::DoT;
            } else {
                return std::nullopt;
            }
            s.upstream = up;
        } else if (key == "QUERIES") {
            if (seen_queries || !parse_counter(value, s.queries)) return std::nullopt;
            seen_queries = true;
        } else if (key == "ENCRYPTED") {
            if (seen_encrypted || !parse_counter(value, s.encrypted)) return std::nullopt;
            seen_encrypted = true;
        } else if (key == "LEGACY") {
            if (seen_legacy || !parse_counter(value, s.legacy)) return std::nullopt;
            seen_legacy = true;
        } else if (key == "SERVFAIL") {
            if (seen_servfail || !parse_counter(value, s.servfail)) return std::nullopt;
            seen_servfail = true;
        } else if (key == "LAST_ERROR") {
            if (seen_last_error || !from_utf8(value, s.last_error)) return std::nullopt;
            seen_last_error = true;
        } else if (key == "UPDATED") {
            if (seen_updated || !from_utf8(value, s.updated_utc)) return std::nullopt;
            seen_updated = true;
        } else {
            return std::nullopt;   // unknown key (includes a blank line)
        }
    }

    if (!ended) return std::nullopt;
    if (!(seen_mode && seen_queries && seen_encrypted && seen_legacy && seen_servfail && seen_updated)) {
        return std::nullopt;
    }
    return s;
}

std::wstring describe_status(const ServiceStatus& s) {
    std::wstring out;
    out += L"DNS service status (updated " + sanitized(s.updated_utc) + L")\n";

    out += L"Mode:      ";
    if (s.mode == Mode::LegacyFallback) {
        out += L"FALLBACK - using the original unencrypted DNS";
    } else if (s.upstream) {
        out += L"encrypted (" + s.upstream->provider_id + L" over " + method_wtext(s.upstream->method) + L")";
    } else {
        out += L"encrypted (no upstream answered yet)";
    }
    out += L"\n";

    out += L"Queries:   " + std::to_wstring(s.queries) +
           L" (encrypted " + std::to_wstring(s.encrypted) +
           L", legacy " + std::to_wstring(s.legacy) +
           L", servfail " + std::to_wstring(s.servfail) + L")\n";

    if (!s.last_error.empty()) {
        out += L"Last error: " + sanitized(s.last_error) + L"\n";
    }
    return out;
}

} // namespace netshutup::dns
