// test_backup.cpp — Backup and restore engine tests for NetShutUp Legacy
//
// All registry writes use HKCU\Software\NetShutUp-Test\backup_test.
// Tests never touch real system keys.
// Tests clean up both registry keys and temporary backup files.

#include "backup.h"
#include "registry.h"
#include "catalog.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>

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

static constexpr wchar_t TEST_SUBKEY[] = L"Software\\NetShutUp-Test\\backup_test";
static constexpr wchar_t TEST_FILE[]   = L"Software_NetShutUp_Test_backup.nsub";

static void cleanup_sandbox() {
    registry::delete_value(HKEY_CURRENT_USER, TEST_SUBKEY, L"ValA");
    registry::delete_value(HKEY_CURRENT_USER, TEST_SUBKEY, L"ValB");
    RegDeleteKeyW(HKEY_CURRENT_USER, TEST_SUBKEY);
    DeleteFileW(TEST_FILE);
}

// ── Dummy catalog entries pointing to sandbox ─────────────────────────────────

static const catalog::Entry s_entry_a = {
    L"test.val_a",
    HKEY_CURRENT_USER,
    TEST_SUBKEY,
    L"ValA",
    catalog::Scope::GLOBAL,
    0, false, 1,
    catalog::PROFILE_PRIVACY,
    catalog::OS_ALL,
    L"Test Value A",
    L"https://example.com"
};

static const catalog::Entry s_entry_b = {
    L"test.val_b",
    HKEY_CURRENT_USER,
    TEST_SUBKEY,
    L"ValB",
    catalog::Scope::GLOBAL,
    0, false, 2,
    catalog::PROFILE_PRIVACY,
    catalog::OS_ALL,
    L"Test Value B",
    L"https://example.com"
};

static const catalog::Entry* test_lookup(const wchar_t* id) {
    if (wcscmp(id, L"test.val_a") == 0) return &s_entry_a;
    if (wcscmp(id, L"test.val_b") == 0) return &s_entry_b;
    return nullptr;
}

// ── Tests ─────────────────────────────────────────────────────────────────────

static void test_save_and_verify_roundtrip() {
    cleanup_sandbox();

    // ValA exists with 42, ValB does NOT exist
    EXPECT(registry::write_dword(HKEY_CURRENT_USER, TEST_SUBKEY, L"ValA", 42u));

    std::vector<const catalog::Entry*> entries = { &s_entry_a, &s_entry_b };

    // save() must write and verify successfully
    EXPECT(backup::save(entries, L"privacy", TEST_FILE));

    // load() and inspect
    auto loaded = backup::load(TEST_FILE);
    EXPECT(loaded.has_value());
    EXPECT_EQ(loaded->profile, L"privacy");
    EXPECT_EQ(loaded->entries.size(), 2u);

    // Entry A
    EXPECT_EQ(loaded->entries[0].id, L"test.val_a");
    EXPECT(loaded->entries[0].existed);
    EXPECT_EQ(loaded->entries[0].value, 42u);

    // Entry B
    EXPECT_EQ(loaded->entries[1].id, L"test.val_b");
    EXPECT(!loaded->entries[1].existed);

    cleanup_sandbox();
}

static void test_load_corrupted_file() {
    cleanup_sandbox();

    // Write a bad file
    HANDLE hFile = CreateFileW(TEST_FILE, GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    const char bad_content[] = "NOT_A_VALID_HEADER\nfoo bar";
    DWORD written = 0;
    WriteFile(hFile, bad_content, sizeof(bad_content) - 1, &written, nullptr);
    CloseHandle(hFile);

    auto loaded = backup::load(TEST_FILE);
    EXPECT(!loaded.has_value());

    // Non-existent file
    auto missing = backup::load(L"does_not_exist_xyz.nsub");
    EXPECT(!missing.has_value());

    cleanup_sandbox();
}

static void test_restore_values_and_deletions() {
    cleanup_sandbox();

    // 1. Initial state: ValA has 100, ValB does not exist
    EXPECT(registry::write_dword(HKEY_CURRENT_USER, TEST_SUBKEY, L"ValA", 100u));

    std::vector<const catalog::Entry*> entries = { &s_entry_a, &s_entry_b };
    EXPECT(backup::save(entries, L"privacy", TEST_FILE));

    auto backup_obj = backup::load(TEST_FILE);
    EXPECT(backup_obj.has_value());

    // 2. Simulate apply modifying the system:
    // ValA is changed to 999
    // ValB is newly written as 777
    EXPECT(registry::write_dword(HKEY_CURRENT_USER, TEST_SUBKEY, L"ValA", 999u));
    EXPECT(registry::write_dword(HKEY_CURRENT_USER, TEST_SUBKEY, L"ValB", 777u));

    // 3. Restore from backup
    EXPECT(backup::restore_with_lookup(*backup_obj, test_lookup));

    // 4. Verify post-restore state:
    // ValA should be back to 100
    auto val_a = registry::read_dword(HKEY_CURRENT_USER, TEST_SUBKEY, L"ValA");
    EXPECT(val_a.has_value());
    EXPECT_EQ(*val_a, 100u);

    // ValB must NOT exist (restoring absent state, not 0!)
    auto val_b = registry::read_dword(HKEY_CURRENT_USER, TEST_SUBKEY, L"ValB");
    EXPECT(!val_b.has_value());

    cleanup_sandbox();
}

static void test_paths() {
    std::wstring p = backup::new_backup_path();
    EXPECT(!p.empty());
    EXPECT(p.find(L"backup_") != std::wstring::npos);
    EXPECT(p.find(L".nsub") != std::wstring::npos);
}

// ── Main Runner ───────────────────────────────────────────────────────────────

int main() {
    wprintf(L"Running backup engine sandbox tests...\n");

    test_save_and_verify_roundtrip();
    test_load_corrupted_file();
    test_restore_values_and_deletions();
    test_paths();

    cleanup_sandbox();

    if (g_failed == 0) {
        wprintf(L"All backup tests passed.\n");
        return EXIT_SUCCESS;
    } else {
        wprintf(L"%d backup test(s) FAILED.\n", g_failed);
        return EXIT_FAILURE;
    }
}
