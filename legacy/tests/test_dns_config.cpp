// test_dns_config.cpp — Unit tests for the DNS provider/chain/config module (NetShutUp Legacy)

#include "dns_config.h"
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <optional>
#include <string>
#include <vector>

using namespace netshutup::dns;

static int g_failed = 0;

#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            wprintf(L"FAIL  %hs:%d  %hs\n", __FILE__, __LINE__, #cond);       \
            ++g_failed;                                                       \
        }                                                                     \
    } while (0)

#define EXPECT_EQ(a, b) EXPECT((a) == (b))

static bool wstr_eq(const wchar_t* a, const wchar_t* b) {
    return a != nullptr && b != nullptr && std::wcscmp(a, b) == 0;
}

static Upstream up(const wchar_t* id, Method m) {
    Upstream u;
    u.provider_id = id;
    u.method = m;
    return u;
}

static bool same_config(const Config& a, const Config& b) {
    return a.chain == b.chain && a.legacy_dns == b.legacy_dns && a.fail_threshold == b.fail_threshold;
}

// ── Provider table ──────────────────────────────────────────────────────────

static void expect_provider(const wchar_t* id, const wchar_t* primary,
                            const wchar_t* secondary, const wchar_t* doh) {
    const Provider* p = find_provider(id);
    if (!p) {
        wprintf(L"FAIL  provider missing: %ls\n", id);
        ++g_failed;
        return;
    }
    EXPECT(wstr_eq(p->ipv4_primary, primary));
    EXPECT(wstr_eq(p->ipv4_secondary, secondary));
    EXPECT(wstr_eq(p->doh_url, doh));
    EXPECT_EQ(p->dot_port, 853);
    EXPECT(std::wstring(p->dot_host).size() > 3);
}

static void test_provider_table() {
    expect_provider(L"cloudflare", L"1.1.1.1", L"1.0.0.1", L"https://1.1.1.1/dns-query");
    expect_provider(L"google", L"8.8.8.8", L"8.8.4.4", L"https://8.8.8.8/dns-query");
    expect_provider(L"quad9", L"9.9.9.9", L"149.112.112.112", L"https://9.9.9.9/dns-query");

    EXPECT(find_provider(L"unknown") == nullptr);
    EXPECT(find_provider(L"") == nullptr);
}

// ── Chains ──────────────────────────────────────────────────────────────────

static void expect_chain(const wchar_t* label, const std::vector<Upstream>& got,
                         const std::vector<Upstream>& want) {
    if (got != want) {
        wprintf(L"FAIL  chain mismatch: %ls\n", label);
        ++g_failed;
    }
}

static void test_chains() {
    expect_chain(L"privacy", chain_for_profile(L"privacy"),
                 {up(L"quad9", Method::DoH), up(L"quad9", Method::DoT),
                  up(L"cloudflare", Method::DoH), up(L"cloudflare", Method::DoT)});

    expect_chain(L"recommended", chain_for_profile(L"recommended"),
                 {up(L"cloudflare", Method::DoH), up(L"cloudflare", Method::DoT),
                  up(L"google", Method::DoH), up(L"google", Method::DoT)});

    // performance: default is cloudflare first
    expect_chain(L"performance default", chain_for_profile(L"performance"),
                 {up(L"cloudflare", Method::DoH), up(L"cloudflare", Method::DoT),
                  up(L"google", Method::DoH), up(L"google", Method::DoT)});

    // performance: fastest_first=google makes google A
    expect_chain(L"performance google", chain_for_profile(L"performance", L"google"),
                 {up(L"google", Method::DoH), up(L"google", Method::DoT),
                  up(L"cloudflare", Method::DoH), up(L"cloudflare", Method::DoT)});

    // performance: fastest_first=cloudflare is the same as default
    expect_chain(L"performance cloudflare", chain_for_profile(L"performance", L"cloudflare"),
                 {up(L"cloudflare", Method::DoH), up(L"cloudflare", Method::DoT),
                  up(L"google", Method::DoH), up(L"google", Method::DoT)});

    // performance: quad9 / garbage are ignored
    expect_chain(L"performance quad9", chain_for_profile(L"performance", L"quad9"),
                 {up(L"cloudflare", Method::DoH), up(L"cloudflare", Method::DoT),
                  up(L"google", Method::DoH), up(L"google", Method::DoT)});
    expect_chain(L"performance garbage", chain_for_profile(L"performance", L"garbage!!"),
                 {up(L"cloudflare", Method::DoH), up(L"cloudflare", Method::DoT),
                  up(L"google", Method::DoH), up(L"google", Method::DoT)});

    // fastest_first is ignored for privacy and recommended
    expect_chain(L"privacy ignores fastest", chain_for_profile(L"privacy", L"google"),
                 {up(L"quad9", Method::DoH), up(L"quad9", Method::DoT),
                  up(L"cloudflare", Method::DoH), up(L"cloudflare", Method::DoT)});
    expect_chain(L"recommended ignores fastest", chain_for_profile(L"recommended", L"google"),
                 {up(L"cloudflare", Method::DoH), up(L"cloudflare", Method::DoT),
                  up(L"google", Method::DoH), up(L"google", Method::DoT)});

    // unknown profile
    EXPECT(chain_for_profile(L"nope").empty());
    EXPECT(chain_for_profile(L"").empty());
}

// ── serialize_config ────────────────────────────────────────────────────────

static void test_serialize_exact() {
    Config c;
    c.chain = {up(L"quad9", Method::DoH), up(L"quad9", Method::DoT)};
    c.legacy_dns = {L"192.168.1.1"};
    c.fail_threshold = 3;

    std::string want =
        "NETSHUTUP_DNS_V1\n"
        "FAIL_THRESHOLD 3\n"
        "UPSTREAM quad9 doh\n"
        "UPSTREAM quad9 dot\n"
        "LEGACY 192.168.1.1\n"
        "END\n";
    EXPECT(serialize_config(c) == want);
}

// ── Round trip ──────────────────────────────────────────────────────────────

static void test_round_trip() {
    Config none;
    none.chain = {up(L"cloudflare", Method::DoH), up(L"google", Method::DoT)};
    none.fail_threshold = 7;

    auto p0 = parse_config(serialize_config(none));
    EXPECT(p0.has_value());
    if (p0) EXPECT(same_config(*p0, none));

    Config two;
    two.chain = chain_for_profile(L"privacy");
    two.legacy_dns = {L"1.1.1.1", L"10.0.0.2"};
    two.fail_threshold = 1;

    auto p2 = parse_config(serialize_config(two));
    EXPECT(p2.has_value());
    if (p2) EXPECT(same_config(*p2, two));
}

// ── parse_config: accepts ───────────────────────────────────────────────────

static void test_parse_accepts() {
    EXPECT(parse_config("NETSHUTUP_DNS_V1\nFAIL_THRESHOLD 3\nUPSTREAM quad9 doh\nEND\n").has_value());

    auto crlf = parse_config(
        "NETSHUTUP_DNS_V1\r\nFAIL_THRESHOLD 3\r\nUPSTREAM quad9 doh\r\n"
        "LEGACY 192.168.1.1\r\nEND\r\n");
    EXPECT(crlf.has_value());
    if (crlf) {
        Config want;
        want.chain = {up(L"quad9", Method::DoH)};
        want.legacy_dns = {L"192.168.1.1"};
        want.fail_threshold = 3;
        EXPECT(same_config(*crlf, want));
    }
}

// ── parse_config: rejects ───────────────────────────────────────────────────

static void expect_reject(const char* label, const std::string& text) {
    if (parse_config(text).has_value()) {
        wprintf(L"FAIL  accepted but should reject: %hs\n", label);
        ++g_failed;
    }
}

// Builds a document: header + body + END line.
static std::string doc(const std::string& body) {
    return "NETSHUTUP_DNS_V1\n" + body + "END\n";
}

static void test_parse_rejects() {
    expect_reject("empty string", "");
    expect_reject("wrong header", "NETSHUTUP_DNS_V2\nFAIL_THRESHOLD 3\nUPSTREAM quad9 doh\nEND\n");
    expect_reject("missing END",
                  "NETSHUTUP_DNS_V1\nFAIL_THRESHOLD 3\nUPSTREAM quad9 doh\n");
    expect_reject("unknown provider", doc("UPSTREAM foo doh\n"));
    expect_reject("unknown method", doc("UPSTREAM quad9 tcp\n"));

    expect_reject("LEGACY three octets", doc("UPSTREAM quad9 doh\nLEGACY 1.2.3\n"));
    expect_reject("LEGACY octet 256", doc("UPSTREAM quad9 doh\nLEGACY 256.1.1.1\n"));
    expect_reject("LEGACY five octets", doc("UPSTREAM quad9 doh\nLEGACY 1.2.3.4.5\n"));
    expect_reject("LEGACY letters", doc("UPSTREAM quad9 doh\nLEGACY a.b.c.d\n"));

    expect_reject("threshold 0", doc("FAIL_THRESHOLD 0\nUPSTREAM quad9 doh\n"));
    expect_reject("threshold 101", doc("FAIL_THRESHOLD 101\nUPSTREAM quad9 doh\n"));
    expect_reject("threshold abc", doc("FAIL_THRESHOLD abc\nUPSTREAM quad9 doh\n"));

    expect_reject("empty chain", doc("FAIL_THRESHOLD 3\nLEGACY 1.2.3.4\n"));
    expect_reject("unknown keyword", doc("UPSTREAM quad9 doh\nFOO bar\n"));
    expect_reject("content after END", doc("UPSTREAM quad9 doh\n") + "UPSTREAM quad9 dot\n");
    expect_reject("garbage after END", doc("UPSTREAM quad9 doh\n") + "garbage\n");
}

int main() {
    wprintf(L"Running DNS config tests...\n");

    test_provider_table();
    test_chains();
    test_serialize_exact();
    test_round_trip();
    test_parse_accepts();
    test_parse_rejects();

    if (g_failed == 0) {
        wprintf(L"All DNS config tests passed.\n");
        return EXIT_SUCCESS;
    } else {
        wprintf(L"%d DNS config test(s) FAILED.\n", g_failed);
        return EXIT_FAILURE;
    }
}
