// manual_dns_probe.cpp — MANUAL live check (not registered with ctest).
// Sends one A query for example.com to each provider over DoH and DoT and
// prints the outcome. Run by hand: build/tests/Debug/manual_dns_probe.exe

#include "dns_doh.h"
#include "dns_dot.h"
#include <cstdio>

using namespace netshutup::dns;

static packet::Bytes make_query() {
    packet::Bytes q = {0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0};
    const char* labels[] = {"example", "com"};
    for (const char* l : labels) {
        q.push_back(static_cast<unsigned char>(std::char_traits<char>::length(l)));
        for (const char* c = l; *c; ++c) q.push_back(static_cast<unsigned char>(*c));
    }
    q.push_back(0);
    q.insert(q.end(), {0x00, 0x01, 0x00, 0x01});
    return q;
}

static void report(const wchar_t* who, const wchar_t* how, const QueryResult& r) {
    if (r.ok) {
        wprintf(L"%-11ls %-4ls OK   %zu bytes, id=%02X%02X rcode=%u answers=%u\n", who, how,
                r.response.size(), r.response[0], r.response[1], r.response[3] & 0x0F,
                (r.response[6] << 8) | r.response[7]);
    } else {
        wprintf(L"%-11ls %-4ls FAIL %ls\n", who, how, r.error.c_str());
    }
}

int main() {
    const packet::Bytes q = make_query();
    DohTransport doh;
    DotTransport dot;
    wprintf(L"WinHTTP TLS 1.2 available: %ls\n", doh.tls12_available() ? L"yes" : L"NO");
    const wchar_t* ids[] = {L"cloudflare", L"google", L"quad9"};
    for (const wchar_t* id : ids) {
        report(id, L"DoH", doh.query({id, Method::DoH}, q, 5000));
        report(id, L"DoT", dot.query({id, Method::DoT}, q, 5000));
        report(id, L"DoT", dot.query({id, Method::DoT}, q, 5000));   // second call reuses the connection
    }
    return 0;
}
