#pragma once

// backup.h — Backup and restore engine for NetShutUp Legacy
//
// Rules (from GEMINI.md design principles):
//  - save() writes the backup to disk and verifies it (reads back + compares)
//    before returning. No apply may start until save() returns true.
//  - A setting that was absent must be restored as absent, not as zero.
//    "Not set" is a distinct state from "set to 0".
//  - restore() is the only code path that writes to real system registry keys
//    during revert; apply uses a separate write path.
//  - Backup files live in a "backups\" folder next to the executable.
//  - File format: plain-text, UTF-8, human-readable (.nsub extension).

#include "catalog.h"
#include <optional>
#include <string>
#include <vector>
#include <windows.h>

namespace netshutup::backup {

// One saved registry value.
struct BackupEntry {
    std::wstring id;       // Catalog ID (e.g. L"privacy.llmnr")
    bool         existed;  // Was the value present in the registry?
    DWORD        value;    // Saved value. Meaningful only when existed == true.
};

struct Backup {
    std::wstring             profile;  // Profile name that was applied.
    std::wstring             timestamp;
    std::vector<BackupEntry> entries;
};

// Reads current registry values for the given catalog entries, writes them
// to `path`, then verifies by reading the file back and comparing.
//
// PER_ADAPTER entries are silently skipped (adapter module not yet built).
// Returns false if reading, writing, or verification fails.
// The caller must not proceed with apply if this returns false.
bool save(const std::vector<const catalog::Entry*>& entries,
          const std::wstring&                        profile_name,
          const std::wstring&                        path);

// Loads a backup file from `path`.
// Returns std::nullopt if the file cannot be opened, is empty, or is
// structurally invalid (wrong magic, truncated, unrecognised tokens).
std::optional<Backup> load(const std::wstring& path);

// Restores all entries in `backup` to the registry.
//   existed == true  → write the saved DWORD value
//   existed == false → delete the value (restore "not set" state)
// Returns true only if every entry was restored without error.
// Entries whose catalog ID is no longer in the catalog are skipped
// (catalog may have changed between apply and revert).
bool restore(const Backup& backup);

// Restores entries using a custom lookup function (primarily for tests to
// redirect writes to sandbox keys).
using LookupFn = const catalog::Entry* (*)(const wchar_t*);
bool restore_with_lookup(const Backup& backup, LookupFn lookup);

// Returns the path of the most recently created backup file in the
// executable's backups\ directory, or an empty string if none exists.
std::wstring latest_backup_path();

// Builds a unique path for a new backup file (timestamped, in backups\).
// Creates the backups\ directory if it does not exist.
std::wstring new_backup_path();

} // namespace netshutup::backup
