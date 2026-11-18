#pragma once

// preview.h — Read-only profile preview engine for NetShutUp Legacy
//
// Rules:
//  - Never writes to the registry or files.
//  - No administrator privileges required.
//  - Evaluates what changes would be made if `apply <profile>` were executed.

#include "catalog.h"
#include <optional>
#include <string>
#include <vector>

namespace netshutup::preview {

enum class Action {
    NO_CHANGE,        // Registry value already equals profile target value
    MODIFY,           // Registry value exists but differs from target
    ADD,              // Registry value does not exist (will be added)
    ADAPTER_PENDING,  // PER_ADAPTER scope setting (adapter module not yet active)
};

struct Item {
    const catalog::Entry* entry;
    std::optional<DWORD>  current_value;
    DWORD                 target_value;
    Action                action;
};

struct ProfilePreview {
    std::wstring      profile_name;
    DWORD             profile_flag;
    std::vector<Item> items;
    std::size_t       to_change_count = 0;
    std::size_t       already_set_count = 0;
    std::size_t       pending_adapter_count = 0;
    std::wstring      firewall_note;   // set when firewall settings were skipped
};

// Converts profile name ("privacy", "performance", "recommended") to bitmask.
// Returns 0 if invalid name.
DWORD parse_profile_flag(const wchar_t* profile_name) noexcept;

// Generates preview items for the given profile name and OS version mask.
// Returns std::nullopt if the profile name is unrecognized.
std::optional<ProfilePreview> generate(const wchar_t* profile_name, DWORD os_mask);

// Prints a formatted preview table and summary to stdout.
void print_preview(const ProfilePreview& preview);

} // namespace netshutup::preview
