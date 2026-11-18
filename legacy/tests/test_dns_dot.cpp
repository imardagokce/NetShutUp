// test_dns_dot.cpp — local error paths of the DoT client. NO network access:
// every query below fails before any connection is attempted. The live check
// is tests/manual_dns_probe.cpp (run by hand).

#include "dns_dot.h"
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

int main() {
    DotTransport t;
    packet::Bytes q(29, 0);

    // DoH upstream is not this transport's job.
    EXPECT(!t.query(Upstream{L"cloudflare", Method::DoH}, q, 1000).ok);
    // Unknown provider.
    EXPECT(!t.query(Upstream{L"nobody", Method::DoT}, q, 1000).ok);
    // Query too short or empty.
    EXPECT(!t.query(Upstream{L"cloudflare", Method::DoT}, packet::Bytes{}, 1000).ok);
    EXPECT(!t.query(Upstream{L"cloudflare", Method::DoT}, packet::Bytes(5, 0), 1000).ok);
    // Query larger than a DNS message can be.
    EXPECT(!t.query(Upstream{L"cloudflare", Method::DoT}, packet::Bytes(70000, 0), 1000).ok);
    // Plain DNS is not handled here.
    EXPECT(!t.query_legacy({L"192.0.2.1"}, q, 1000).ok);

    if (g_failed) { wprintf(L"%d failure(s)\n", g_failed); return 1; }
    wprintf(L"dns_dot: all passed\n");
    return 0;
}
