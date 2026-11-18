// test_registry.cpp — Registry sandbox tests for NetShutUp Legacy
//
// All writes go to HKCU\Software\NetShutUp-Test.
// Tests clean up after themselves regardless of pass/fail.
// No real system registry key is ever touched.

#include "registry.h"
#include <cstdio>
#include <cstdlib>
#include <cassert>

using namespace netshutup::registry;

// ── Minimal test harness ──────────────────────────────────────────────────────

static int g_failed = 0;

#define EXPECT(cond)                                              \
    do {                                                          \
        if (!(cond)) {                                            \
            wprintf(L"FAIL  %hs:%d  %hs\n", __FILE__, __LINE__, #cond); \
            ++g_failed;                                           \
        }                                                         \
    } while (0)

#define EXPECT_EQ(a, b) EXPECT((a) == (b))

// ── Helpers ───────────────────────────────────────────────────────────────────

// Subkey used by all tests; cleaned up in teardown().
static constexpr wchar_t TESTKEY[] = L"Software\\NetShutUp-Test\\registry_test";

static void teardown() {
    // Best-effort cleanup; ignore errors.
    RegDeleteKeyW(HKEY_CURRENT_USER, TESTKEY);
}

// ── Tests ─────────────────────────────────────────────────────────────────────

static void test_read_absent() {
    // A value that has never been written must not be found.
    auto val = read_dword(HKEY_CURRENT_USER, TESTKEY, L"TestValue");
    EXPECT(!val.has_value());
}

static void test_write_and_read() {
    EXPECT(write_dword(HKEY_CURRENT_USER, TESTKEY, L"TestValue", 42u));

    auto val = read_dword(HKEY_CURRENT_USER, TESTKEY, L"TestValue");
    EXPECT(val.has_value());
    EXPECT_EQ(*val, 42u);
}

static void test_overwrite() {
    // Write once, then overwrite; the second value must win.
    write_dword(HKEY_CURRENT_USER, TESTKEY, L"Overwrite", 1u);
    write_dword(HKEY_CURRENT_USER, TESTKEY, L"Overwrite", 99u);

    auto val = read_dword(HKEY_CURRENT_USER, TESTKEY, L"Overwrite");
    EXPECT(val.has_value());
    EXPECT_EQ(*val, 99u);
}

static void test_delete_existing() {
    write_dword(HKEY_CURRENT_USER, TESTKEY, L"ToDelete", 7u);
    EXPECT(delete_value(HKEY_CURRENT_USER, TESTKEY, L"ToDelete"));

    auto val = read_dword(HKEY_CURRENT_USER, TESTKEY, L"ToDelete");
    EXPECT(!val.has_value());
}

static void test_delete_absent() {
    // Deleting a non-existent value must succeed (idempotent).
    EXPECT(delete_value(HKEY_CURRENT_USER, TESTKEY, L"NeverWritten"));
}

static void test_key_exists() {
    // Before writing, the test key might not exist yet.
    // Write something to ensure it exists, then check.
    write_dword(HKEY_CURRENT_USER, TESTKEY, L"Probe", 0u);
    EXPECT(key_exists(HKEY_CURRENT_USER, TESTKEY));

    // A key that definitely does not exist.
    EXPECT(!key_exists(HKEY_CURRENT_USER,
                       L"Software\\NetShutUp-Test\\does_not_exist_xyz"));
}

static void test_enum_subkeys() {
    // Create two child keys.
    write_dword(HKEY_CURRENT_USER,
                L"Software\\NetShutUp-Test\\registry_test\\child_a", L"v", 1u);
    write_dword(HKEY_CURRENT_USER,
                L"Software\\NetShutUp-Test\\registry_test\\child_b", L"v", 2u);

    auto subs = enum_subkeys(HKEY_CURRENT_USER, TESTKEY);
    // We expect at least child_a and child_b.
    bool found_a = false, found_b = false;
    for (const auto& s : subs) {
        if (s == L"child_a") found_a = true;
        if (s == L"child_b") found_b = true;
    }
    EXPECT(found_a);
    EXPECT(found_b);
}

// ── Runner ────────────────────────────────────────────────────────────────────

int main() {
    teardown(); // start clean

    wprintf(L"Running registry sandbox tests...\n");

    test_read_absent();
    test_write_and_read();
    test_overwrite();
    test_delete_existing();
    test_delete_absent();
    test_key_exists();
    test_enum_subkeys();

    teardown(); // clean up after ourselves

    if (g_failed == 0) {
        wprintf(L"All registry tests passed.\n");
        return EXIT_SUCCESS;
    } else {
        wprintf(L"%d test(s) FAILED.\n", g_failed);
        return EXIT_FAILURE;
    }
}
