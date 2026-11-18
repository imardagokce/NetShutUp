// apply.cpp — Apply and revert execution engine implementation for NetShutUp Legacy

#include "apply.h"
#include "preview.h"
#include "firewall.h"
#include "registry.h"
#include "store.h"
#include <cstdio>
#include <cwchar>

namespace netshutup::apply {

// ── apply_entries ─────────────────────────────────────────────────────────────

ApplyResult apply_entries(const std::vector<const catalog::Entry*>& entries,
                          const std::wstring& profile_name,
                          bool dry_run,
                          const std::wstring& custom_backup_path,
                          backup::LookupFn lookup,
                          const std::vector<std::wstring>* adapter_guids) {
    ApplyResult res{};
    res.profile_name = profile_name;
    res.is_dry_run = dry_run;

    if (entries.empty()) {
        res.success = false;
        res.error_message = L"No catalog settings provided to apply.";
        return res;
    }

    // Resolve per-adapter entries against the physical adapters (real enumeration unless injected).
    std::size_t per_adapter_entries = 0;
    for (const auto* e : entries) {
        if (e && e->scope == catalog::Scope::PER_ADAPTER) ++per_adapter_entries;
    }
    std::vector<std::wstring> guids;
    if (per_adapter_entries > 0) {
        guids = adapter_guids ? *adapter_guids : targets::physical_adapter_guids();
        if (guids.empty()) res.skipped_count += per_adapter_entries;   // nothing to apply them to
    }
    const std::vector<targets::Target> list = targets::expand(entries, guids);

    // ── Dry-Run Mode ──────────────────────────────────────────────────────────
    if (dry_run) {
        res.success = true;
        for (const auto& t : list) {
            res.applied_count++;
            res.item_results.push_back({t.entry, true, t.entry->target_dword});
        }
        return res;
    }

    // ── 1. Create and Verify Backup Before First Write ─────────────────────────
    std::wstring b_path = custom_backup_path.empty() ? backup::new_backup_path() : custom_backup_path;

    if (!backup::save(list, profile_name, b_path)) {
        res.success = false;
        res.error_message = L"Failed to create or verify backup file. No changes were made.";
        return res;
    }
    res.backup_path = b_path;

    // ── 2. Apply Each Setting ─────────────────────────────────────────────────
    for (const auto& t : list) {
        const catalog::Entry* entry = t.entry;

        // Firewall: already at target -> nothing to write (avoids needless COM writes).
        bool ok;
        if (entry->scope == catalog::Scope::FIREWALL &&
            store::read(t) == std::optional<DWORD>(entry->target_dword)) {
            ok = true;
        } else {
            ok = store::write(t, entry->target_dword);
        }
        res.item_results.push_back({entry, ok, entry->target_dword});

        if (ok) {
            res.applied_count++;
        } else {
            // ── 3. Critical Failure: Atomic Rollback ───────────────────────────
            res.success = false;
            res.error_message = L"Failed to write registry setting: " + t.id;

            // Roll back all changes made so far using the verified backup
            auto b = backup::load(b_path);
            if (b) {
                if (lookup) {
                    backup::restore_with_lookup(*b, lookup);
                } else {
                    backup::restore(*b);
                }
                res.rolled_back = true;
            }
            return res;
        }
    }

    res.success = true;
    return res;
}

// ── apply_profile ─────────────────────────────────────────────────────────────

ApplyResult apply_profile(const wchar_t* profile_name,
                          DWORD os_mask,
                          bool dry_run,
                          const std::wstring& custom_backup_path,
                          backup::LookupFn lookup,
                          const std::vector<std::wstring>* adapter_guids) {
    ApplyResult res{};
    res.profile_name = profile_name ? profile_name : L"";
    res.is_dry_run = dry_run;

    DWORD flag = preview::parse_profile_flag(profile_name);
    if (flag == 0) {
        res.success = false;
        res.error_message = L"Unknown profile name: " + res.profile_name;
        return res;
    }

    const std::wstring fw_skip = firewall::skip_reason();

    std::vector<const catalog::Entry*> entries;
    for (std::size_t i = 0; i < catalog::entry_count(); ++i) {
        const auto& entry = catalog::entries()[i];
        if ((entry.profiles & flag) == 0) continue;
        if ((entry.os_mask & os_mask) == 0) continue;
        if (entry.scope == catalog::Scope::FIREWALL && !fw_skip.empty()) continue;
        entries.push_back(&entry);
    }

    ApplyResult result = apply_entries(entries, profile_name, dry_run, custom_backup_path, lookup, adapter_guids);
    result.firewall_note = fw_skip;
    return result;
}

// ── revert ────────────────────────────────────────────────────────────────────

RevertResult revert(const std::wstring& specific_backup_path,
                    backup::LookupFn lookup) {
    RevertResult res{};

    std::wstring path = specific_backup_path.empty() ? backup::latest_backup_path() : specific_backup_path;
    if (path.empty()) {
        res.success = false;
        res.error_message = L"No backup file found in backups/ directory to restore.";
        return res;
    }
    res.backup_path = path;

    auto b = backup::load(path);
    if (!b.has_value()) {
        res.success = false;
        res.error_message = L"Backup file could not be read or is corrupt: " + path;
        return res;
    }

    res.profile_name = b->profile;
    res.restored_count = b->entries.size();

    bool ok = lookup ? backup::restore_with_lookup(*b, lookup) : backup::restore(*b);
    if (!ok) {
        res.success = false;
        res.error_message = L"One or more settings could not be restored to the registry.";
        return res;
    }

    res.success = true;
    return res;
}

// ── Pretty Printing ───────────────────────────────────────────────────────────

void print_apply_result(const ApplyResult& result) {
    if (result.is_dry_run) {
        wprintf(L"\n[DRY-RUN] Simulated apply for profile '%ls':\n", result.profile_name.c_str());
        wprintf(L"  - %zu setting(s) would be applied.\n", result.applied_count);
        wprintf(L"  - %zu setting(s) skipped (no physical network adapter found).\n", result.skipped_count);
        if (!result.firewall_note.empty()) wprintf(L"  Note: %ls\n", result.firewall_note.c_str());
        wprintf(L"  (No registry keys were modified and no backup file was created).\n\n");
        return;
    }

    if (result.success) {
        wprintf(L"\nNetShutUp Legacy -- Profile Applied Successfully\n");
        wprintf(L"======================================================================\n");
        wprintf(L"Profile:           %ls\n", result.profile_name.c_str());
        wprintf(L"Settings applied:  %zu\n", result.applied_count);
        if (result.skipped_count > 0) {
            wprintf(L"Settings deferred: %zu (per-adapter settings: no physical network adapter found)\n", result.skipped_count);
        }
        if (!result.firewall_note.empty()) wprintf(L"Note:              %ls\n", result.firewall_note.c_str());
        wprintf(L"Backup saved to:   %ls\n\n", result.backup_path.c_str());
        wprintf(L"To undo all changes at any time, run:\n"
                L"  netshutup-legacy revert\n\n");
    } else {
        wprintf(L"\nNetShutUp Legacy -- Apply FAILED\n");
        wprintf(L"======================================================================\n");
        wprintf(L"Error: %ls\n", result.error_message.c_str());
        if (result.rolled_back) {
            wprintf(L"\n[SAFETY ROLLBACK] All changes were automatically rolled back.\n"
                    L"The system was restored to its exact previous state from the backup.\n\n");
        } else {
            wprintf(L"No changes were written to the registry.\n\n");
        }
    }
}

void print_revert_result(const RevertResult& result) {
    if (result.success) {
        wprintf(L"\nNetShutUp Legacy -- Revert Successful\n");
        wprintf(L"======================================================================\n");
        wprintf(L"Restored profile: %ls\n", result.profile_name.c_str());
        wprintf(L"Settings restored: %zu\n", result.restored_count);
        wprintf(L"From backup file:  %ls\n\n", result.backup_path.c_str());
    } else {
        wprintf(L"\nNetShutUp Legacy -- Revert FAILED\n");
        wprintf(L"======================================================================\n");
        wprintf(L"Error: %ls\n\n", result.error_message.c_str());
    }
}

} // namespace netshutup::apply
