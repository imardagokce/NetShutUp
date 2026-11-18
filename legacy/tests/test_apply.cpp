// test_apply.cpp — Apply and Revert engine sandbox tests for NetShutUp Legacy
//
// All registry writes use HKCU\Software\NetShutUp-Test\apply_test.
// No real system keys are ever touched.
// All temporary files and registry keys are cleaned up in teardown.

#include "apply.h"
#include "registry.h"
#include "catalog.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace netshutup;

static int g_failed = 0;

#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            wprintf(L"FAIL  %hs:%d  %hs\n", __FILE__, __LINE__, #cond);       \
            ++g_failed;                                                       \
        }                                                                     \
    } while (0)

#define EXPECT_EQ(a, b) EXPECT((a) == (b))

// ── Sandbox setup & teardown ──────────────────────────────────────────────────

static constexpr wchar_t TEST_SUBKEY[] = L"Software\\NetShutUp-Test\\apply_test";
static constexpr wchar_t TEST_BACKUP[] = L"Software_NetShutUp_Test_apply_backup.nsub";

static void cleanup_sandbox() {
    registry::delete_value(HKEY_CURRENT_USER, TEST_SUBKEY, L"Val1");
    registry::delete_value(HKEY_CURRENT_USER, TEST_SUBKEY, L"Val2");
    registry::delete_value(HKEY_CURRENT_USER, TEST_SUBKEY, L"ValReadOnly");
    RegDeleteKeyW(HKEY_CURRENT_USER, TEST_SUBKEY);
    DeleteFileW(TEST_BACKUP);
}

// ── Dummy catalog entries pointing to sandbox ─────────────────────────────────

static const catalog::Entry s_entry_1 = {
    L"test.apply.val1",
    HKEY_CURRENT_USER,
    TEST_SUBKEY,
    L"Val1",
    catalog::Scope::GLOBAL,
    0, false, 100, // target is 100
    catalog::PROFILE_PRIVACY,
    catalog::OS_ALL,
    L"Test Setting 1",
    L"https://example.com"
};

static const catalog::Entry s_entry_2 = {
    L"test.apply.val2",
    HKEY_CURRENT_USER,
    TEST_SUBKEY,
    L"Val2",
    catalog::Scope::GLOBAL,
    0, false, 200, // target is 200
    catalog::PROFILE_PRIVACY,
    catalog::OS_ALL,
    L"Test Setting 2",
    L"https://example.com"
};

// Entry that points to an invalid hive handle to simulate write failure
static const catalog::Entry s_entry_fail = {
    L"test.apply.fail",
    reinterpret_cast<HKEY>(static_cast<LONG_PTR>(0xDEADBEEF)), // invalid hive
    L"InvalidSubkey",
    L"InvalidVal",
    catalog::Scope::GLOBAL,
    0, false, 999,
    catalog::PROFILE_PRIVACY,
    catalog::OS_ALL,
    L"Test Setting Fail",
    L"https://example.com"
};

static const catalog::Entry* test_lookup(const wchar_t* id) {
    if (wcscmp(id, L"test.apply.val1") == 0) return &s_entry_1;
    if (wcscmp(id, L"test.apply.val2") == 0) return &s_entry_2;
    if (wcscmp(id, L"test.apply.fail") == 0) return &s_entry_fail;
    return nullptr;
}

// ── Tests ─────────────────────────────────────────────────────────────────────

static void test_dry_run() {
    cleanup_sandbox();

    // Val1 exists with 5, Val2 does not exist
    EXPECT(registry::write_dword(HKEY_CURRENT_USER, TEST_SUBKEY, L"Val1", 5u));

    std::vector<const catalog::Entry*> entries = { &s_entry_1, &s_entry_2 };

    auto res = apply::apply_entries(entries, L"privacy", /*dry_run=*/true, TEST_BACKUP, test_lookup);
    EXPECT(res.success);
    EXPECT(res.is_dry_run);
    EXPECT_EQ(res.applied_count, 2u);

    // Verify nothing changed in the registry
    auto val1 = registry::read_dword(HKEY_CURRENT_USER, TEST_SUBKEY, L"Val1");
    EXPECT(val1.has_value());
    EXPECT_EQ(*val1, 5u);

    auto val2 = registry::read_dword(HKEY_CURRENT_USER, TEST_SUBKEY, L"Val2");
    EXPECT(!val2.has_value());

    // Verify no backup file was created
    EXPECT_EQ(GetFileAttributesW(TEST_BACKUP), INVALID_FILE_ATTRIBUTES);

    cleanup_sandbox();
}

static void test_apply_and_revert() {
    cleanup_sandbox();

    // 1. Initial state: Val1 = 10, Val2 is absent
    EXPECT(registry::write_dword(HKEY_CURRENT_USER, TEST_SUBKEY, L"Val1", 10u));

    std::vector<const catalog::Entry*> entries = { &s_entry_1, &s_entry_2 };

    // 2. Real apply: Val1 -> 100, Val2 -> 200
    auto res = apply::apply_entries(entries, L"privacy", /*dry_run=*/false, TEST_BACKUP, test_lookup);
    EXPECT(res.success);
    EXPECT(!res.is_dry_run);
    EXPECT_EQ(res.applied_count, 2u);

    // Check registry values updated
    auto val1_applied = registry::read_dword(HKEY_CURRENT_USER, TEST_SUBKEY, L"Val1");
    EXPECT(val1_applied.has_value());
    EXPECT_EQ(*val1_applied, 100u);

    auto val2_applied = registry::read_dword(HKEY_CURRENT_USER, TEST_SUBKEY, L"Val2");
    EXPECT(val2_applied.has_value());
    EXPECT_EQ(*val2_applied, 200u);

    // 3. Revert from backup
    auto rev_res = apply::revert(TEST_BACKUP, test_lookup);
    EXPECT(rev_res.success);
    EXPECT_EQ(rev_res.restored_count, 2u);

    // Check registry values restored
    // Val1 should be 10 again
    auto val1_reverted = registry::read_dword(HKEY_CURRENT_USER, TEST_SUBKEY, L"Val1");
    EXPECT(val1_reverted.has_value());
    EXPECT_EQ(*val1_reverted, 10u);

    // Val2 must NOT exist ("not set" state)
    auto val2_reverted = registry::read_dword(HKEY_CURRENT_USER, TEST_SUBKEY, L"Val2");
    EXPECT(!val2_reverted.has_value());

    cleanup_sandbox();
}

static void test_automatic_rollback_on_failure() {
    cleanup_sandbox();

    // Initial state: Val1 = 77
    EXPECT(registry::write_dword(HKEY_CURRENT_USER, TEST_SUBKEY, L"Val1", 77u));

    // Entry 1 succeeds, Entry Fail triggers a write error
    std::vector<const catalog::Entry*> entries = { &s_entry_1, &s_entry_fail };

    auto res = apply::apply_entries(entries, L"privacy", /*dry_run=*/false, TEST_BACKUP, test_lookup);

    // Must report failure and rolled_back = true
    EXPECT(!res.success);
    EXPECT(res.rolled_back);

    // Val1 must have been restored back to 77 by the automatic rollback!
    auto val1 = registry::read_dword(HKEY_CURRENT_USER, TEST_SUBKEY, L"Val1");
    EXPECT(val1.has_value());
    EXPECT_EQ(*val1, 77u);

    cleanup_sandbox();
}

// ── Runner ────────────────────────────────────────────────────────────────────

int main() {
    wprintf(L"Running apply and revert engine sandbox tests...\n");

    test_dry_run();
    test_apply_and_revert();
    test_automatic_rollback_on_failure();

    cleanup_sandbox();

    if (g_failed == 0) {
        wprintf(L"All apply/revert tests passed.\n");
        return EXIT_SUCCESS;
    } else {
        wprintf(L"%d apply/revert test(s) FAILED.\n", g_failed);
        return EXIT_FAILURE;
    }
}
