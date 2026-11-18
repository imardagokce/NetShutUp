// test_targets.cpp — expansion of catalog entries into concrete targets (pure, no registry access).

#include "targets.h"
#include <cstdio>
#include <cwchar>

using namespace netshutup;

static int g_failed = 0;
#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            wprintf(L"FAIL  %hs:%d  %hs\n", __FILE__, __LINE__, #cond);       \
            ++g_failed;                                                       \
        }                                                                     \
    } while (0)

static const wchar_t kGuidA[] = L"{AAAAAAAA-0000-0000-0000-00000000000A}";
static const wchar_t kGuidB[] = L"{BBBBBBBB-0000-0000-0000-00000000000B}";

static void test_split_id() {
    auto p = targets::split_id(L"perf.tcp_nodelay@{GUID}");
    EXPECT(p.first == L"perf.tcp_nodelay" && p.second == L"{GUID}");
    auto q = targets::split_id(L"privacy.llmnr");
    EXPECT(q.first == L"privacy.llmnr" && q.second.empty());
    auto r = targets::split_id(L"");
    EXPECT(r.first.empty() && r.second.empty());
}

static void test_make_target() {
    const catalog::Entry* nd = catalog::find(L"perf.tcp_nodelay");
    EXPECT(nd != nullptr);
    if (!nd) return;
    targets::Target t = targets::make_target(*nd, kGuidA);
    EXPECT(t.id == std::wstring(L"perf.tcp_nodelay@") + kGuidA);
    EXPECT(t.subkey.find(kGuidA) != std::wstring::npos);
    EXPECT(t.subkey.find(L"{ADAPTER-GUID}") == std::wstring::npos);

    const catalog::Entry* llmnr = catalog::find(L"privacy.llmnr");
    EXPECT(llmnr != nullptr);
    if (!llmnr) return;
    targets::Target g = targets::make_target(*llmnr, L"");
    EXPECT(g.id == L"privacy.llmnr" && g.subkey == llmnr->subkey);

    const catalog::Entry* fw = catalog::find(L"firewall.public.enabled");
    EXPECT(fw != nullptr);
    if (fw) {
        targets::Target f = targets::make_target(*fw, L"");
        EXPECT(f.id == L"firewall.public.enabled" && f.subkey == L"public");
    }
}

static void test_expand() {
    const catalog::Entry* llmnr = catalog::find(L"privacy.llmnr");
    const catalog::Entry* nd = catalog::find(L"perf.tcp_nodelay");
    const catalog::Entry* ack = catalog::find(L"perf.tcp_ack_freq");
    EXPECT(llmnr && nd && ack);
    if (!(llmnr && nd && ack)) return;

    std::vector<const catalog::Entry*> entries = {llmnr, nullptr, nd, ack};
    auto two = targets::expand(entries, {kGuidA, kGuidB});
    // llmnr (1) + nodelay (2) + ack_freq (2); the nullptr is skipped; order = entry order, then adapters.
    EXPECT(two.size() == 5);
    if (two.size() == 5) {
        EXPECT(two[0].id == L"privacy.llmnr");
        EXPECT(two[1].id == std::wstring(L"perf.tcp_nodelay@") + kGuidA);
        EXPECT(two[2].id == std::wstring(L"perf.tcp_nodelay@") + kGuidB);
        EXPECT(two[3].id == std::wstring(L"perf.tcp_ack_freq@") + kGuidA);
        EXPECT(two[4].id == std::wstring(L"perf.tcp_ack_freq@") + kGuidB);
    }
    auto none = targets::expand(entries, {});
    EXPECT(none.size() == 1 && none[0].id == L"privacy.llmnr");   // per-adapter entries give nothing without adapters
}

static void test_display_label() {
    EXPECT(targets::display_label(L"perf.tcp_nodelay@{D7747AC7-885F-4A75-8F0C-84BA153614EC}") == L"perf.tcp_nodelay@{D7747AC7..}");
    EXPECT(targets::display_label(L"privacy.llmnr") == L"privacy.llmnr");
    EXPECT(targets::display_label(L"x@short") == L"x@short");
    EXPECT(targets::display_label(L"x@{NOTAGUID}") == L"x@{NOTAGUID}");
    EXPECT(targets::display_label(L"") == L"");
}

int main() {
    test_display_label();
    test_split_id();
    test_make_target();
    test_expand();
    if (g_failed) { wprintf(L"%d failure(s)\n", g_failed); return 1; }
    wprintf(L"targets: all passed\n");
    return 0;
}
