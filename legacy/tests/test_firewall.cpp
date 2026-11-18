// test_firewall.cpp — firewall catalog entries and pure logic. READ-ONLY:
// never writes firewall settings or registry values.

#include "catalog.h"
#include "firewall.h"
#include "store.h"
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

static void test_third_party_names() {
    EXPECT(!firewall::is_third_party_name(L"Windows Firewall"));
    EXPECT(!firewall::is_third_party_name(L"windows defender firewall"));
    EXPECT(!firewall::is_third_party_name(L"Windows-Firewall"));
    EXPECT(firewall::is_third_party_name(L"Norton Smart Firewall"));
    EXPECT(firewall::is_third_party_name(L"Kaspersky Firewall"));
    EXPECT(!firewall::is_third_party_name(L""));
}

static void test_catalog_entries() {
    const wchar_t* profiles[] = {L"domain", L"private", L"public"};
    const wchar_t* whats[] = {L"enabled", L"inbound"};
    int found = 0;
    for (auto p : profiles) {
        for (auto w : whats) {
            std::wstring id = std::wstring(L"firewall.") + p + L"." + w;
            const catalog::Entry* e = catalog::find(id.c_str());
            EXPECT(e != nullptr);
            if (!e) continue;
            ++found;
            EXPECT(e->scope == catalog::Scope::FIREWALL);
            EXPECT(e->hive == nullptr);
            EXPECT(std::wcscmp(e->subkey, p) == 0);
            EXPECT(std::wcscmp(e->value_name, w) == 0);
            EXPECT(e->default_exists);
            EXPECT(e->target_dword == (std::wcscmp(w, L"enabled") == 0 ? 1u : 0u));
            EXPECT(e->default_dword == e->target_dword);   // tier 1 only enforces Windows defaults
            EXPECT((e->profiles & catalog::PROFILE_PRIVACY) && (e->profiles & catalog::PROFILE_PERFORMANCE) &&
                   (e->profiles & catalog::PROFILE_RECOMMENDED));
            EXPECT(e->os_mask == catalog::OS_ALL);
        }
    }
    EXPECT(found == 6);
}

static void test_read_smoke() {
    // Read-only. The firewall service may be stopped on a dev machine, so an
    // unavailable API (nullopt) is acceptable; a value, when present, must be 0 or 1.
    for (std::size_t i = 0; i < catalog::entry_count(); ++i) {
        const catalog::Entry& e = catalog::entries()[i];
        if (e.scope != catalog::Scope::FIREWALL) continue;
        auto v = store::read(e);
        if (v.has_value()) EXPECT(*v <= 1u);
    }
    // Invalid arguments never crash and yield nullopt.
    EXPECT(!firewall::read(L"bogus", L"enabled").has_value());
    EXPECT(!firewall::read(L"public", L"bogus").has_value());
    EXPECT(!firewall::read(nullptr, nullptr).has_value());
    // Values other than 0/1 are rejected before any COM write.
    EXPECT(!firewall::write(L"public", L"enabled", 2));
    EXPECT(!firewall::write(L"bogus", L"enabled", 1));
    // skip_reason must not crash whatever Security Center says.
    (void)firewall::skip_reason();
}

static void test_store_remove_firewall_is_noop() {
    const catalog::Entry* e = catalog::find(L"firewall.public.enabled");
    EXPECT(e != nullptr);
    if (e) EXPECT(store::remove(*e));   // no "not set" state; must not touch anything
}

int main() {
    test_third_party_names();
    test_catalog_entries();
    test_read_smoke();
    test_store_remove_firewall_is_noop();
    if (g_failed) { wprintf(L"%d failure(s)\n", g_failed); return 1; }
    wprintf(L"firewall: all passed\n");
    return 0;
}
