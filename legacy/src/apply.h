#pragma once

// apply.h — Apply and revert execution engine for NetShutUp Legacy
//
// Rules (from GEMINI.md design principles):
//  - "Her zaman önce yedek." A verified backup must exist on disk before
//    the first change is written.
//  - "A failure halfway undoes the changes made so far." (Automatic rollback).
//  - "Gerektiğinde yükseltme." apply and revert require administrator rights.
//  - "--dry-run" simulates the entire apply without writing to the registry.

#include "catalog.h"
#include "backup.h"
#include <string>
#include <vector>
#include <optional>

namespace netshutup::apply {

struct ItemResult {
    const catalog::Entry* entry;
    bool                  success;
    DWORD                 target_value;
};

struct ApplyResult {
    bool                    success;
    std::wstring            profile_name;
    std::wstring            backup_path;
    std::vector<ItemResult> item_results;
    std::size_t             applied_count = 0;
    std::size_t             skipped_count = 0;
    bool                    rolled_back = false;
    bool                    is_dry_run = false;
    std::wstring            error_message;
    std::wstring            firewall_note;   // set when firewall settings were skipped
};

struct RevertResult {
    bool         success;
    std::wstring backup_path;
    std::wstring profile_name;
    std::size_t  restored_count = 0;
    std::wstring error_message;
};

// Core apply function for a profile name ("privacy", "performance", "recommended").
//  dry_run: if true, simulates apply without modifying the registry or writing files.
//  custom_backup_path: if non-empty, writes backup to this path (useful for testing).
//  lookup: if non-null, overrides catalog lookup for sandbox tests.
ApplyResult apply_profile(const wchar_t* profile_name,
                          DWORD os_mask,
                          bool dry_run = false,
                          const std::wstring& custom_backup_path = L"",
                          backup::LookupFn lookup = nullptr);

// Applies a specific list of entries (used by apply_profile and test harness).
ApplyResult apply_entries(const std::vector<const catalog::Entry*>& entries,
                          const std::wstring& profile_name,
                          bool dry_run = false,
                          const std::wstring& custom_backup_path = L"",
                          backup::LookupFn lookup = nullptr);

// Core revert function.
// If specific_backup_path is empty, finds the most recent backup in backups\.
// lookup: if non-null, overrides catalog lookup for sandbox tests.
RevertResult revert(const std::wstring& specific_backup_path = L"",
                    backup::LookupFn lookup = nullptr);

// Pretty printing helpers
void print_apply_result(const ApplyResult& result);
void print_revert_result(const RevertResult& result);

} // namespace netshutup::apply
