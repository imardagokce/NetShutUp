// test_dns_doh.cpp — pure DoH helpers and local error paths. NO network access:
// every query below fails before any connection is attempted.

#include "dns_doh.h"
#include <cstdio>

using namespace netshutup::dns;

static int g_failed = 0;
#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            wprintf(L"FAIL  %hs:%d  %hs\n", __FILE__, __LINE__, #cond);       \
            ++g_failed;                                                       \
        }                                                                     \
    } while (0)

static void test_url_ok() {
    UrlParts u;
    EXPECT(parse_https_url(L"https://1.1.1.1/dns-query", u));
    EXPECT(u.host == L"1.1.1.1" && u.port == 443 && u.path == L"/dns-query");

    EXPECT(parse_https_url(L"HTTPS://dns.example:8443/x?y=1", u));
    EXPECT(u.host == L"dns.example" && u.port == 8443 && u.path == L"/x?y=1");

    EXPECT(parse_https_url(L"https://dns.example", u));
    EXPECT(u.path == L"/" && u.port == 443);

    EXPECT(parse_https_url(L"https://[2606:4700:4700::1111]:853/q", u));
    EXPECT(u.host == L"2606:4700:4700::1111" && u.port == 853 && u.path == L"/q");
}

static void test_url_rejects() {
    UrlParts u;
    EXPECT(!parse_https_url(L"", u));
    EXPECT(!parse_https_url(L"http://1.1.1.1/dns-query", u));
    EXPECT(!parse_https_url(L"https://", u));
    EXPECT(!parse_https_url(L"https:///path", u));
    EXPECT(!parse_https_url(L"https://user@host/", u));
    EXPECT(!parse_https_url(L"https://host:0/", u));
    EXPECT(!parse_https_url(L"https://host:65536/", u));
    EXPECT(!parse_https_url(L"https://host:abc/", u));
    EXPECT(!parse_https_url(L"https://host:/", u));
    EXPECT(!parse_https_url(L"https://ho st/", u));
    EXPECT(!parse_https_url(L"https://[::1/", u));
    EXPECT(!parse_https_url(L"ftp://host/", u));
}

static void test_content_type() {
    EXPECT(is_dns_message_content_type(L"application/dns-message"));
    EXPECT(is_dns_message_content_type(L"Application/DNS-Message"));
    EXPECT(is_dns_message_content_type(L" application/dns-message ; charset=x"));
    EXPECT(!is_dns_message_content_type(L"application/json"));
    EXPECT(!is_dns_message_content_type(L"application/dns-messagex"));
    EXPECT(!is_dns_message_content_type(L""));
}

static void test_local_failures() {
    DohTransport t;
    packet::Bytes q(29, 0);
    // DoT is not this transport's job.
    Upstream dot{L"cloudflare", Method::DoT};
    EXPECT(!t.query(dot, q, 1000).ok);
    // Unknown provider fails before any network use.
    Upstream bogus{L"nobody", Method::DoH};
    EXPECT(!t.query(bogus, q, 1000).ok);
    // Query too short / empty fails locally.
    Upstream cf{L"cloudflare", Method::DoH};
    EXPECT(!t.query(cf, packet::Bytes{}, 1000).ok);
    EXPECT(!t.query(cf, packet::Bytes(5, 0), 1000).ok);
    // Plain DNS is not handled here.
    EXPECT(!t.query_legacy({L"192.0.2.1"}, q, 1000).ok);
}

int main() {
    test_url_ok();
    test_url_rejects();
    test_content_type();
    test_local_failures();
    if (g_failed) { wprintf(L"%d failure(s)\n", g_failed); return 1; }
    wprintf(L"dns_doh: all passed\n");
    return 0;
}
