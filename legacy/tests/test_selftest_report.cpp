// test_selftest_report.cpp — Unit tests for the selftest report renderers (NetShutUp Legacy)

#include "selftest_report.h"
#include "selftest_mask.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace netshutup::selftest;

static int g_failed = 0;

#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            wprintf(L"FAIL  %hs:%d  %hs\n", __FILE__, __LINE__, #cond);       \
            ++g_failed;                                                       \
        }                                                                     \
    } while (0)

#define EXPECT_EQ(a, b) EXPECT((a) == (b))

static const std::wstring kCheck = L"\u2705";  // check mark
static const std::wstring kCross = L"\u274C";  // cross mark
static const std::wstring kSkip  = L"\u23ED";  // skip

// Sample report: privacy (Pass, Fail), performance (Skip, Info, Pass).
// Pass = 2, Fail = 1, Skip = 1, Info is not counted.
static Report sample_report(bool restored) {
    Report r;
    r.started_utc = L"2026-11-18T10:00:00Z";
    r.total_millis = 1234;
    r.all_restored = restored;
    r.manual_revert_cmd = L"netshutup-legacy revert X";

    Section privacy;
    privacy.name = L"privacy";
    privacy.checks.push_back(Check{L"p1", L"Telemetry off", L"ok", Status::Pass, 10});
    privacy.checks.push_back(Check{L"p2", L"Quoted detail", L"say \"hi\"\\path\nline\u0001",
                                   Status::Fail, 20});

    Section performance;
    performance.name = L"performance";
    performance.checks.push_back(Check{L"f1", L"Nagle off", L"skipped on this host", Status::Skip, 0});
    performance.checks.push_back(Check{L"f2", L"Info row", L"just info", Status::Info, 5});
    performance.checks.push_back(Check{L"f3", L"Throttling off", L"ok", Status::Pass, 30});

    r.sections.push_back(privacy);
    r.sections.push_back(performance);
    return r;
}

static std::wstring first_nonempty_line(const std::wstring& s) {
    size_t pos = 0;
    while (pos < s.size()) {
        size_t end = s.find(L'\n', pos);
        if (end == std::wstring::npos) end = s.size();
        std::wstring line = s.substr(pos, end - pos);
        if (!line.empty()) return line;
        pos = end + 1;
    }
    return L"";
}

// Collects every object key in compact JSON: a string immediately followed by ':'.
static std::vector<std::string> json_keys(const std::string& s) {
    std::vector<std::string> keys;
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] != '"') { ++i; continue; }
        size_t start = ++i;
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\') ++i;
            ++i;
        }
        if (i >= s.size()) break;
        std::string token = s.substr(start, i - start);
        ++i;  // closing quote
        if (i < s.size() && s[i] == ':') keys.push_back(token);
    }
    return keys;
}

static bool key_allowed(const std::string& k) {
    static const char* const allowed[] = {
        "schema", "started_utc", "total_millis", "all_restored", "manual_revert_cmd",
        "sections", "name", "checks", "id", "title", "detail", "status", "millis"};
    for (const char* a : allowed) {
        if (k == a) return true;
    }
    return false;
}

// (a) Restore state drives the first line and the manual command.
static void test_restore_state_markdown() {
    std::wstring md = render_markdown(sample_report(false));
    std::wstring first = first_nonempty_line(md);
    EXPECT(first.find(L"NOT everything was restored") != std::wstring::npos);
    EXPECT(md.find(L"netshutup-legacy revert X") != std::wstring::npos);

    std::wstring ok = render_markdown(sample_report(true));
    EXPECT(ok.find(L"All settings restored: yes") != std::wstring::npos);
    EXPECT(ok.find(L"NOT everything was restored") == std::wstring::npos);
}

// (b) Summary counts Pass, Fail and Skip across all sections; Info is not counted.
static void test_summary_counts() {
    std::wstring md = render_markdown(sample_report(true));
    std::wstring expected = kCheck + L" 2  " + kCross + L" 1  " + kSkip + L" 1";
    EXPECT(md.find(expected) != std::wstring::npos);
}

// (c) One heading per section, each followed by a table with one row per check.
static void test_markdown_sections_and_tables() {
    std::wstring md = render_markdown(sample_report(true));
    EXPECT(md.find(L"## privacy\n") != std::wstring::npos);
    EXPECT(md.find(L"## performance\n") != std::wstring::npos);

    size_t headers = 0;
    size_t pos = 0;
    const std::wstring header = L"| Step | Result | ms | Note |";
    while ((pos = md.find(header, pos)) != std::wstring::npos) {
        ++headers;
        pos += header.size();
    }
    EXPECT_EQ(headers, static_cast<size_t>(2));

    EXPECT(md.find(L"Telemetry off") != std::wstring::npos);
    EXPECT(md.find(L"Quoted detail") != std::wstring::npos);
    EXPECT(md.find(L"Nagle off") != std::wstring::npos);
    EXPECT(md.find(L"Throttling off") != std::wstring::npos);
}

// Free-text fields pass through mask::sanitize_text in the markdown renderer.
// Compares against the sanitizer's own output, so it holds for any masking behavior.
static void test_markdown_uses_sanitizer() {
    Report r = sample_report(true);
    const std::wstring raw = L"router 192.168.1.9 seen";
    r.sections[0].checks[0].detail = raw;
    std::wstring md = render_markdown(r);
    std::wstring expected = netshutup::selftest::mask::sanitize_text(raw);
    EXPECT(md.find(expected) != std::wstring::npos);
}

// (d) JSON: schema marker, compact output, escaping, allowed keys and status strings.
static void test_json_contract() {
    std::string js = render_json(sample_report(false));

    EXPECT(js.find("\"schema\":1") != std::string::npos);
    EXPECT(js.find("\"all_restored\":false") != std::string::npos);
    EXPECT(js.find("\"manual_revert_cmd\":\"netshutup-legacy revert X\"") != std::string::npos);
    EXPECT(js.find("\"total_millis\":1234") != std::string::npos);
    EXPECT(js.find("\"started_utc\":\"2026-11-18T10:00:00Z\"") != std::string::npos);

    // Escaping: quote, backslash, newline and control char in detail.
    EXPECT(js.find("\"detail\":\"say \\\"hi\\\"\\\\path\\nline\\u0001\"") != std::string::npos);

    // Compact: no space after colon or comma.
    EXPECT(js.find(": ") == std::string::npos);
    EXPECT(js.find(", ") == std::string::npos);

    // Status strings.
    EXPECT(js.find("\"status\":\"pass\"") != std::string::npos);
    EXPECT(js.find("\"status\":\"fail\"") != std::string::npos);
    EXPECT(js.find("\"status\":\"skip\"") != std::string::npos);
    EXPECT(js.find("\"status\":\"info\"") != std::string::npos);

    // No keys outside the allowed set.
    std::vector<std::string> keys = json_keys(js);
    EXPECT(!keys.empty());
    for (const auto& k : keys) {
        EXPECT(key_allowed(k));
        if (!key_allowed(k)) wprintf(L"  unexpected key: %hs\n", k.c_str());
    }

    std::string restored = render_json(sample_report(true));
    EXPECT(restored.find("\"all_restored\":true") != std::string::npos);
}

int main() {
    wprintf(L"Running selftest report tests...\n");

    test_restore_state_markdown();
    test_summary_counts();
    test_markdown_sections_and_tables();
    test_markdown_uses_sanitizer();
    test_json_contract();

    if (g_failed == 0) {
        wprintf(L"All selftest report tests passed.\n");
        return EXIT_SUCCESS;
    } else {
        wprintf(L"%d selftest report test(s) FAILED.\n", g_failed);
        return EXIT_FAILURE;
    }
}
