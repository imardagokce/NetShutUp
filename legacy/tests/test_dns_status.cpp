// test_dns_status.cpp — Unit tests for the DNS status file (serialize / parse / describe)

#include "dns_status.h"
#include <climits>
#include <cstdio>
#include <cstdlib>
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

// ── helpers ──────────────────────────────────────────────────────────────────

static ServiceStatus make_full() {
    ServiceStatus s;
    s.mode = Mode::Encrypted;
    s.upstream = Upstream{L"quad9", Method::DoT};
    s.queries = 10;
    s.encrypted = 8;
    s.legacy = 1;
    s.servfail = 1;
    s.last_error = L"timeout";
    s.updated_utc = L"2026-11-18T10:00:00Z";
    return s;
}

static ServiceStatus make_minimal() {
    ServiceStatus s;
    s.mode = Mode::LegacyFallback;
    s.queries = 3;
    s.encrypted = 0;
    s.legacy = 3;
    s.servfail = 0;
    s.updated_utc = L"2026-11-18T10:05:00Z";
    return s;
}

static std::vector<std::string> full_lines() {
    return {
        "NETSHUTUP_DNS_STATUS_V1",
        "MODE encrypted",
        "UPSTREAM quad9 dot",
        "QUERIES 10",
        "ENCRYPTED 8",
        "LEGACY 1",
        "SERVFAIL 1",
        "LAST_ERROR timeout",
        "UPDATED 2026-11-18T10:00:00Z",
        "END",
    };
}

static std::string join_lines(const std::vector<std::string>& lines) {
    std::string t;
    for (const auto& l : lines) {
        t += l;
        t += '\n';
    }
    return t;
}

// Removes every line whose key is `key` (exact line, or "key <value>").
static std::vector<std::string> without(std::vector<std::string> lines, const std::string& key) {
    std::vector<std::string> out;
    for (const auto& l : lines) {
        if (l == key || l.rfind(key + " ", 0) == 0) continue;
        out.push_back(l);
    }
    return out;
}

// Inserts `extra` as a line just before the END line.
static std::vector<std::string> with_extra(std::vector<std::string> lines, const std::string& extra) {
    lines.insert(lines.end() - 1, extra);
    return lines;
}

static bool contains(const std::wstring& hay, const wchar_t* needle) {
    return hay.find(needle) != std::wstring::npos;
}

static void expect_same(const ServiceStatus& a, const ServiceStatus& b) {
    EXPECT_EQ(a.mode, b.mode);
    EXPECT_EQ(a.upstream.has_value(), b.upstream.has_value());
    if (a.upstream && b.upstream) {
        EXPECT(a.upstream->provider_id == b.upstream->provider_id);
        EXPECT_EQ(a.upstream->method, b.upstream->method);
    }
    EXPECT_EQ(a.queries, b.queries);
    EXPECT_EQ(a.encrypted, b.encrypted);
    EXPECT_EQ(a.legacy, b.legacy);
    EXPECT_EQ(a.servfail, b.servfail);
    EXPECT(a.last_error == b.last_error);
    EXPECT(a.updated_utc == b.updated_utc);
}

// ── serialize ────────────────────────────────────────────────────────────────

static void test_serialize_full() {
    const std::string expected =
        "NETSHUTUP_DNS_STATUS_V1\n"
        "MODE encrypted\n"
        "UPSTREAM quad9 dot\n"
        "QUERIES 10\n"
        "ENCRYPTED 8\n"
        "LEGACY 1\n"
        "SERVFAIL 1\n"
        "LAST_ERROR timeout\n"
        "UPDATED 2026-11-18T10:00:00Z\n"
        "END\n";
    EXPECT_EQ(serialize_status(make_full()), expected);
}

static void test_serialize_minimal() {
    const std::string expected =
        "NETSHUTUP_DNS_STATUS_V1\n"
        "MODE legacy\n"
        "QUERIES 3\n"
        "ENCRYPTED 0\n"
        "LEGACY 3\n"
        "SERVFAIL 0\n"
        "UPDATED 2026-11-18T10:05:00Z\n"
        "END\n";
    EXPECT_EQ(serialize_status(make_minimal()), expected);
}

static void test_serialize_sanitizes_line_breaks() {
    ServiceStatus s = make_full();
    s.last_error = L"line one\r\nline two\nend";
    const std::string text = serialize_status(s);
    EXPECT(text.find("LAST_ERROR line one  line two end\n") != std::string::npos);
    EXPECT(text.find('\r') == std::string::npos);

    auto back = parse_status(text);
    EXPECT(back.has_value());
    if (back) {
        EXPECT(back->last_error == L"line one  line two end");
    }
}

// ── round trip ───────────────────────────────────────────────────────────────

static void test_round_trip_full() {
    const ServiceStatus s = make_full();
    auto back = parse_status(serialize_status(s));
    EXPECT(back.has_value());
    if (back) expect_same(*back, s);
}

static void test_round_trip_minimal() {
    const ServiceStatus s = make_minimal();
    auto back = parse_status(serialize_status(s));
    EXPECT(back.has_value());
    if (back) expect_same(*back, s);
}

static void test_round_trip_keeps_spaces() {
    ServiceStatus s = make_full();
    s.last_error = L"  two  words  ";
    auto back = parse_status(serialize_status(s));
    EXPECT(back.has_value());
    if (back) EXPECT(back->last_error == L"  two  words  ");
}

static void test_round_trip_non_ascii() {
    ServiceStatus s = make_full();
    s.last_error = L"çıkış hata";
    auto back = parse_status(serialize_status(s));
    EXPECT(back.has_value());
    if (back) EXPECT(back->last_error == s.last_error);
}

// ── parse: accepted input ────────────────────────────────────────────────────

static void test_parse_fields() {
    auto p = parse_status(join_lines(full_lines()));
    EXPECT(p.has_value());
    if (!p) return;
    EXPECT_EQ(p->mode, Mode::Encrypted);
    EXPECT(p->upstream.has_value());
    if (p->upstream) {
        EXPECT(p->upstream->provider_id == L"quad9");
        EXPECT_EQ(p->upstream->method, Method::DoT);
    }
    EXPECT_EQ(p->queries, 10ULL);
    EXPECT_EQ(p->encrypted, 8ULL);
    EXPECT_EQ(p->legacy, 1ULL);
    EXPECT_EQ(p->servfail, 1ULL);
    EXPECT(p->last_error == L"timeout");
    EXPECT(p->updated_utc == L"2026-11-18T10:00:00Z");
}

static void test_parse_accepts_crlf() {
    std::string crlf;
    for (const auto& l : full_lines()) {
        crlf += l;
        crlf += "\r\n";
    }
    auto p = parse_status(crlf);
    EXPECT(p.has_value());
    if (p) expect_same(*p, make_full());
}

static void test_parse_accepts_max_counter() {
    auto lines = full_lines();
    for (auto& l : lines) {
        if (l == "QUERIES 10") l = "QUERIES 18446744073709551615";
    }
    auto q = parse_status(join_lines(lines));
    EXPECT(q.has_value());
    if (q) EXPECT_EQ(q->queries, ULLONG_MAX);
}

// ── parse: rejected input ────────────────────────────────────────────────────

static void test_parse_rejects_empty() {
    EXPECT(!parse_status("").has_value());
}

static void test_parse_rejects_wrong_header() {
    auto lines = full_lines();
    lines[0] = "NETSHUTUP_DNS_STATUS_V0";
    EXPECT(!parse_status(join_lines(lines)).has_value());
}

static void test_parse_rejects_missing_end() {
    EXPECT(!parse_status(join_lines(without(full_lines(), "END"))).has_value());
}

static void test_parse_rejects_unknown_key() {
    EXPECT(!parse_status(join_lines(with_extra(full_lines(), "FOO 1"))).has_value());
}

static void test_parse_rejects_duplicate_key() {
    EXPECT(!parse_status(join_lines(with_extra(full_lines(), "QUERIES 1"))).has_value());
    EXPECT(!parse_status(join_lines(with_extra(full_lines(), "UPSTREAM quad9 doh"))).has_value());
}

static void test_parse_rejects_bad_counter() {
    const char* bad[] = {"QUERIES 1x", "QUERIES -1", "QUERIES +1", "QUERIES ", "QUERIES", "QUERIES 1.5"};
    for (const char* b : bad) {
        auto lines = full_lines();
        for (auto& l : lines) {
            if (l.rfind("QUERIES", 0) == 0) l = b;
        }
        EXPECT(!parse_status(join_lines(lines)).has_value());
    }
}

static void test_parse_rejects_counter_overflow() {
    auto lines = full_lines();
    for (auto& l : lines) {
        if (l == "QUERIES 10") l = "QUERIES 18446744073709551616";
    }
    EXPECT(!parse_status(join_lines(lines)).has_value());

    lines = full_lines();
    for (auto& l : lines) {
        if (l == "SERVFAIL 1") l = "SERVFAIL 99999999999999999999999";
    }
    EXPECT(!parse_status(join_lines(lines)).has_value());
}

static void test_parse_rejects_unknown_mode() {
    auto lines = full_lines();
    lines[1] = "MODE foo";
    EXPECT(!parse_status(join_lines(lines)).has_value());
}

static void test_parse_rejects_unknown_provider() {
    auto lines = full_lines();
    lines[2] = "UPSTREAM nosuch dot";
    EXPECT(!parse_status(join_lines(lines)).has_value());
}

static void test_parse_rejects_unknown_method() {
    auto lines = full_lines();
    lines[2] = "UPSTREAM quad9 tcp";
    EXPECT(!parse_status(join_lines(lines)).has_value());
}

static void test_parse_rejects_missing_required() {
    const char* required[] = {"MODE", "QUERIES", "ENCRYPTED", "LEGACY", "SERVFAIL", "UPDATED"};
    for (const char* key : required) {
        EXPECT(!parse_status(join_lines(without(full_lines(), key))).has_value());
    }
}

// ── describe_status ──────────────────────────────────────────────────────────

static void test_describe_full() {
    const std::wstring text = describe_status(make_full());
    EXPECT(contains(text, L"2026-11-18T10:00:00Z"));
    EXPECT(contains(text, L"encrypted (quad9 over dot)"));
    EXPECT(contains(text, L"10 (encrypted 8, legacy 1, servfail 1)"));
    EXPECT(contains(text, L"Last error:"));
    EXPECT(contains(text, L"timeout"));
}

static void test_describe_upstream_doh() {
    ServiceStatus s = make_full();
    s.upstream = Upstream{L"cloudflare", Method::DoH};
    EXPECT(contains(describe_status(s), L"encrypted (cloudflare over doh)"));
}

static void test_describe_no_upstream_yet() {
    ServiceStatus s = make_full();
    s.upstream.reset();
    EXPECT(contains(describe_status(s), L"(no upstream answered yet)"));
}

static void test_describe_legacy_fallback() {
    EXPECT(contains(describe_status(make_minimal()), L"FALLBACK"));
}

static void test_describe_omits_empty_last_error() {
    ServiceStatus s = make_full();
    s.last_error.clear();
    EXPECT(!contains(describe_status(s), L"Last error:"));
}

// ── main ─────────────────────────────────────────────────────────────────────

int main() {
    wprintf(L"Running DNS status tests...\n");

    test_serialize_full();
    test_serialize_minimal();
    test_serialize_sanitizes_line_breaks();

    test_round_trip_full();
    test_round_trip_minimal();
    test_round_trip_keeps_spaces();
    test_round_trip_non_ascii();

    test_parse_fields();
    test_parse_accepts_crlf();
    test_parse_accepts_max_counter();

    test_parse_rejects_empty();
    test_parse_rejects_wrong_header();
    test_parse_rejects_missing_end();
    test_parse_rejects_unknown_key();
    test_parse_rejects_duplicate_key();
    test_parse_rejects_bad_counter();
    test_parse_rejects_counter_overflow();
    test_parse_rejects_unknown_mode();
    test_parse_rejects_unknown_provider();
    test_parse_rejects_unknown_method();
    test_parse_rejects_missing_required();

    test_describe_full();
    test_describe_upstream_doh();
    test_describe_no_upstream_yet();
    test_describe_legacy_fallback();
    test_describe_omits_empty_last_error();

    if (g_failed == 0) {
        wprintf(L"All DNS status tests passed.\n");
        return EXIT_SUCCESS;
    } else {
        wprintf(L"%d DNS status test(s) FAILED.\n", g_failed);
        return EXIT_FAILURE;
    }
}
