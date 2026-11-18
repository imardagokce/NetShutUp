// dns_doh.cpp — DoH over WinHTTP (see dns_doh.h).

#include "dns_doh.h"

#include <windows.h>
#include <winhttp.h>
#include <cwchar>

#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 0x00000800
#endif

namespace netshutup::dns {

// ── Pure helpers ──────────────────────────────────────────────────────────────

bool parse_https_url(const std::wstring& url, UrlParts& out) {
    static const wchar_t kScheme[] = L"https://";
    const std::size_t kSchemeLen = 8;
    if (url.size() <= kSchemeLen || _wcsnicmp(url.c_str(), kScheme, kSchemeLen) != 0) return false;
    for (wchar_t c : url) {
        if (c <= L' ' || c == 0x7F) return false;   // whitespace / control characters
    }

    std::wstring rest = url.substr(kSchemeLen);
    std::size_t slash = rest.find(L'/');
    std::wstring authority = rest.substr(0, slash);
    std::wstring path = (slash == std::wstring::npos) ? L"/" : rest.substr(slash);
    if (authority.empty() || authority.find(L'@') != std::wstring::npos) return false;

    std::wstring host;
    std::wstring port_text;
    if (authority.front() == L'[') {
        std::size_t close = authority.find(L']');
        if (close == std::wstring::npos) return false;
        host = authority.substr(1, close - 1);
        std::wstring tail = authority.substr(close + 1);
        if (!tail.empty()) {
            if (tail.front() != L':') return false;
            port_text = tail.substr(1);
            if (port_text.empty()) return false;
        }
    } else {
        std::size_t colon = authority.find(L':');
        if (colon == std::wstring::npos) {
            host = authority;
        } else {
            host = authority.substr(0, colon);
            port_text = authority.substr(colon + 1);
            if (port_text.empty()) return false;
        }
    }
    if (host.empty()) return false;

    unsigned long port = 443;
    if (!port_text.empty()) {
        port = 0;
        for (wchar_t c : port_text) {
            if (c < L'0' || c > L'9') return false;
            port = port * 10 + static_cast<unsigned long>(c - L'0');
            if (port > 65535) return false;
        }
        if (port == 0) return false;
    }

    out.host = host;
    out.port = static_cast<unsigned short>(port);
    out.path = path;
    return true;
}

bool is_dns_message_content_type(const std::wstring& value) {
    std::size_t b = 0;
    while (b < value.size() && value[b] == L' ') ++b;
    std::size_t e = value.find(L';', b);
    if (e == std::wstring::npos) e = value.size();
    while (e > b && value[e - 1] == L' ') --e;
    static const wchar_t kType[] = L"application/dns-message";
    const std::size_t kLen = 23;
    return (e - b) == kLen && _wcsnicmp(value.c_str() + b, kType, kLen) == 0;
}

// ── WinHTTP transport ─────────────────────────────────────────────────────────

namespace {

struct Handle {
    HINTERNET h = nullptr;
    explicit Handle(HINTERNET x = nullptr) : h(x) {}
    ~Handle() { if (h) WinHttpCloseHandle(h); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

QueryResult fail(const wchar_t* why) {
    QueryResult r;
    r.ok = false;
    r.error = why;
    return r;
}

} // namespace

DohTransport::DohTransport() {
    HINTERNET s = WinHttpOpen(L"NetShutUp-DNS/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return;
    session_ = s;

    // TLS 1.2 only. On Windows 7 without KB3140245 this option is rejected.
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    tls12_ok_ = WinHttpSetOption(s, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols)) != FALSE;

    // Never follow redirects: a DoH endpoint that redirects is misconfigured or hostile.
    DWORD disable = WINHTTP_DISABLE_REDIRECTS;
    WinHttpSetOption(s, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable));
}

DohTransport::~DohTransport() {
    if (session_) WinHttpCloseHandle(static_cast<HINTERNET>(session_));
}

QueryResult DohTransport::query(const Upstream& upstream, const packet::Bytes& wire,
                                unsigned timeout_ms) {
    if (upstream.method != Method::DoH) return fail(L"DohTransport handles DoH only");
    const Provider* prov = find_provider(upstream.provider_id);
    if (!prov) return fail(L"unknown provider");
    if (wire.size() < 12 || wire.size() > 65535) return fail(L"query size out of range");
    if (!session_) return fail(L"WinHTTP session could not be opened");
    if (!tls12_ok_) return fail(L"TLS 1.2 not available in WinHTTP (Windows 7 needs KB3140245)");

    UrlParts url;
    if (!parse_https_url(prov->doh_url, url)) return fail(L"bad DoH URL in provider table");

    HINTERNET session = static_cast<HINTERNET>(session_);
    const int t = static_cast<int>(timeout_ms);
    WinHttpSetTimeouts(session, t, t, t, t);

    Handle conn(WinHttpConnect(session, url.host.c_str(), url.port, 0));
    if (!conn.h) return fail(L"connect failed");

    Handle req(WinHttpOpenRequest(conn.h, L"POST", url.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                  WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!req.h) return fail(L"request could not be created");

    static const wchar_t kHeaders[] =
        L"Content-Type: application/dns-message\r\nAccept: application/dns-message\r\n";
    // Copy: WinHttpSendRequest takes a non-const buffer pointer.
    packet::Bytes body = wire;
    if (!WinHttpSendRequest(req.h, kHeaders, static_cast<DWORD>(-1L), body.data(),
                            static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0)) {
        return fail(L"send failed (TLS or network error)");
    }
    if (!WinHttpReceiveResponse(req.h, nullptr)) return fail(L"no response");

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (!WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                             WINHTTP_NO_HEADER_INDEX) || status != 200) {
        return fail(L"HTTP status was not 200");
    }

    wchar_t ctype[128] = {};
    DWORD ctype_size = sizeof(ctype);
    if (!WinHttpQueryHeaders(req.h, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX,
                             ctype, &ctype_size, WINHTTP_NO_HEADER_INDEX) ||
        !is_dns_message_content_type(ctype)) {
        return fail(L"unexpected Content-Type");
    }

    QueryResult res;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req.h, &avail)) return fail(L"read failed");
        if (avail == 0) break;
        if (res.response.size() + avail > 65535) return fail(L"response too large");
        const std::size_t old = res.response.size();
        res.response.resize(old + avail);
        DWORD got = 0;
        if (!WinHttpReadData(req.h, res.response.data() + old, avail, &got)) return fail(L"read failed");
        res.response.resize(old + got);
    }
    if (res.response.size() < 12) return fail(L"response too short");
    res.ok = true;
    return res;
}

QueryResult DohTransport::query_legacy(const std::vector<std::wstring>&,
                                       const packet::Bytes&, unsigned) {
    return fail(L"plain DNS is not handled by DohTransport");
}

} // namespace netshutup::dns
