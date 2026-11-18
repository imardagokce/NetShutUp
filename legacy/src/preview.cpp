// preview.cpp — Read-only profile preview implementation for NetShutUp Legacy

#include "preview.h"
#include "dns_config.h"
#include "firewall.h"
#include "store.h"
#include <cstdio>
#include <cwchar>

namespace netshutup::preview {

// ── Profile name parsing ──────────────────────────────────────────────────────

DWORD parse_profile_flag(const wchar_t* profile_name) noexcept {
    if (!profile_name) return 0;
    if (_wcsicmp(profile_name, L"privacy") == 0)     return catalog::PROFILE_PRIVACY;
    if (_wcsicmp(profile_name, L"performance") == 0) return catalog::PROFILE_PERFORMANCE;
    if (_wcsicmp(profile_name, L"recommended") == 0) return catalog::PROFILE_RECOMMENDED;
    return 0;
}

// ── Preview generation ────────────────────────────────────────────────────────

std::optional<ProfilePreview> generate(const wchar_t* profile_name, DWORD os_mask,
                                       const std::vector<std::wstring>* adapter_guids) {
    DWORD flag = parse_profile_flag(profile_name);
    if (flag == 0) {
        return std::nullopt;
    }

    ProfilePreview preview;
    preview.profile_name = profile_name;
    preview.profile_flag = flag;

    preview.firewall_note = firewall::skip_reason();

    std::vector<std::wstring> guids;
    bool guids_loaded = false;

    for (std::size_t i = 0; i < catalog::entry_count(); ++i) {
        const auto& entry = catalog::entries()[i];

        // Check profile membership and OS compatibility
        if ((entry.profiles & flag) == 0) continue;
        if ((entry.os_mask & os_mask) == 0) continue;
        if (entry.scope == catalog::Scope::FIREWALL && !preview.firewall_note.empty()) continue;

        // One row per concrete target (per-adapter entries: one per physical adapter).
        std::vector<targets::Target> list;
        if (entry.scope == catalog::Scope::PER_ADAPTER) {
            if (!guids_loaded) {
                guids = adapter_guids ? *adapter_guids : targets::physical_adapter_guids();
                guids_loaded = true;
            }
            for (const auto& g : guids) list.push_back(targets::make_target(entry, g));
            if (list.empty()) {
                Item pending;
                pending.entry = &entry;
                pending.label = entry.id;
                pending.target_value = entry.target_dword;
                pending.action = Action::ADAPTER_PENDING;
                preview.pending_adapter_count++;
                preview.items.push_back(pending);
                continue;
            }
        } else {
            list.push_back(targets::make_target(entry, L""));
        }

        for (const auto& t : list) {
            Item item;
            item.entry = &entry;
            item.label = t.id;
            item.target_value = entry.target_dword;
            item.current_value = store::read(t);
            if (item.current_value.has_value()) {
                if (*item.current_value == entry.target_dword) {
                    item.action = Action::NO_CHANGE;
                    preview.already_set_count++;
                } else {
                    item.action = Action::MODIFY;
                    preview.to_change_count++;
                }
            } else {
                item.action = Action::ADD;
                preview.to_change_count++;
            }
            preview.items.push_back(item);
        }
    }

    return preview;
}

// ── Pretty printing ───────────────────────────────────────────────────────────

static const wchar_t* profile_description(DWORD flag) noexcept {
    switch (flag) {
        case catalog::PROFILE_PRIVACY:
            return L"Telemetry and local network name leakage protections";
        case catalog::PROFILE_PERFORMANCE:
            return L"Low latency tweaks for gaming and real-time networking";
        case catalog::PROFILE_RECOMMENDED:
            return L"Safe, broadly beneficial network settings";
    }
    return L"";
}

void print_preview(const ProfilePreview& preview) {
    wprintf(L"\nNetShutUp Legacy -- Preview: %ls\n", preview.profile_name.c_str());
    wprintf(L"======================================================================\n");
    wprintf(L"Purpose: %ls\n\n", profile_description(preview.profile_flag));
    if (!preview.firewall_note.empty()) wprintf(L"Note: %ls\n\n", preview.firewall_note.c_str());

    constexpr int W_ID      = 28;
    constexpr int W_CURRENT = 12;
    constexpr int W_TARGET  = 10;
    constexpr int W_ACTION  = 26;

    wprintf(L"%-*s  %-*s  %-*s  %s\n",
            W_ID,      L"Setting",
            W_CURRENT, L"Current",
            W_TARGET,  L"Target",
                       L"Action");
    wprintf(L"%.*s\n", W_ID + W_CURRENT + W_TARGET + W_ACTION + 6,
            L"--------------------------------------------------------------------------------");

    for (const auto& item : preview.items) {
        wchar_t cur_buf[24];
        if (item.current_value.has_value()) {
            _snwprintf_s(cur_buf, _countof(cur_buf), _TRUNCATE, L"%lu", *item.current_value);
        } else {
            _snwprintf_s(cur_buf, _countof(cur_buf), _TRUNCATE, L"(absent)");
        }

        wchar_t tgt_buf[24];
        _snwprintf_s(tgt_buf, _countof(tgt_buf), _TRUNCATE, L"%lu", item.target_value);

        wchar_t act_buf[48];
        switch (item.action) {
            case Action::NO_CHANGE:
                _snwprintf_s(act_buf, _countof(act_buf), _TRUNCATE, L"No change (already %lu)", item.target_value);
                break;
            case Action::MODIFY:
                _snwprintf_s(act_buf, _countof(act_buf), _TRUNCATE, L"Modify -> %lu", item.target_value);
                break;
            case Action::ADD:
                _snwprintf_s(act_buf, _countof(act_buf), _TRUNCATE, L"Add -> %lu", item.target_value);
                break;
            case Action::ADAPTER_PENDING:
                _snwprintf_s(act_buf, _countof(act_buf), _TRUNCATE, L"(no adapter found)");
                break;
        }

        wprintf(L"%-*ls  %-*ls  %-*ls  %ls\n",
                W_ID,      targets::display_label(item.label).c_str(),
                W_CURRENT, cur_buf,
                W_TARGET,  tgt_buf,
                           act_buf);
    }

    wprintf(L"--------------------------------------------------------------------------------\n");
    {
        const auto chain = dns::chain_for_profile(preview.profile_name);
        if (!chain.empty()) {
            wprintf(L"DNS:     encrypted DNS through a local resolver service (%ls first, then %ls;\n"
                    L"         DoH, then DoT); the original DNS stays as the last resort.\n"
                    L"         Skipped automatically if it cannot be verified on this network.\n\n",
                    chain.front().provider_id.c_str(), chain[chain.size() - 1].provider_id.c_str());
        }
    }
    wprintf(L"Summary: %zu total setting(s) in profile.\n"
            L"         %zu to change, %zu already set, %zu pending adapter support.\n\n",
            preview.items.size(),
            preview.to_change_count,
            preview.already_set_count,
            preview.pending_adapter_count);

    if (preview.to_change_count > 0) {
        wprintf(L"To apply these changes, run:\n"
                L"  netshutup-legacy apply %ls\n"
                L"(Requires administrator rights)\n\n",
                preview.profile_name.c_str());
    } else {
        wprintf(L"All settings in this profile are already configured.\n\n");
    }
}

} // namespace netshutup::preview
