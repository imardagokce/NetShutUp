#pragma once

// catalog.h — Settings catalog for NetShutUp Legacy
//
// Every entry in this catalog has been verified against Microsoft
// documentation before being added.  Unverified settings are not included.
//
// Documentation references are stored in the doc_ref field of each entry.
// The compatibility matrix (docs/compatibility-matrix.md) is the authoritative
// source of OS-level compatibility claims.

#include <windows.h>
#include <cstddef>

namespace netshutup::catalog {

// ── OS compatibility flags ────────────────────────────────────────────────────
constexpr DWORD OS_WIN7  = 1u << 0;   // Windows 7 SP1  (6.1.7601)
constexpr DWORD OS_WIN8  = 1u << 1;   // Windows 8      (6.2)
constexpr DWORD OS_WIN81 = 1u << 2;   // Windows 8.1    (6.3)
constexpr DWORD OS_ALL   = OS_WIN7 | OS_WIN8 | OS_WIN81;

// ── Profile membership flags ─────────────────────────────────────────────────
constexpr DWORD PROFILE_PRIVACY     = 1u << 0;
constexpr DWORD PROFILE_PERFORMANCE = 1u << 1;
constexpr DWORD PROFILE_RECOMMENDED = 1u << 2;

// ── Setting scope ─────────────────────────────────────────────────────────────
enum class Scope {
    // Single global registry key; applies once.
    GLOBAL,

    // Applied once per physical network adapter.
    // The subkey contains "{ADAPTER-GUID}" as a placeholder; the apply
    // engine substitutes each physical adapter's GUID at runtime.
    // Adapter enumeration is implemented in adapter.h (future module).
    PER_ADAPTER,
};

// ── Catalog entry ─────────────────────────────────────────────────────────────
struct Entry {
    const wchar_t* id;          // Unique dot-separated ID, e.g. L"privacy.llmnr"
    HKEY           hive;        // HKEY_LOCAL_MACHINE or HKEY_CURRENT_USER
    const wchar_t* subkey;      // Registry subkey path (no hive prefix).
                                // For PER_ADAPTER scope, "{ADAPTER-GUID}" is a
                                // literal placeholder for the adapter GUID.
    const wchar_t* value_name;  // Registry value name.
    Scope          scope;

    // Windows out-of-box default for this value.
    // default_exists = false means Windows does not create the key/value by
    // default; absence is treated as the default state.
    DWORD default_dword;
    bool  default_exists;

    // Value written by the apply engine when this entry is activated.
    DWORD target_dword;

    // Bitmask of profiles that include this setting.
    DWORD profiles;

    // Bitmask of OS versions on which this setting is known to exist.
    DWORD os_mask;

    // Short English description shown in preview / scan output.
    const wchar_t* description;

    // Microsoft documentation reference (URL or KB article number).
    const wchar_t* doc_ref;
};

// ── Catalog access ────────────────────────────────────────────────────────────

// Returns a pointer to the beginning of the catalog array.
const Entry* entries() noexcept;

// Returns the number of entries in the catalog.
std::size_t  entry_count() noexcept;

// Looks up an entry by ID. Returns nullptr if not found.
const Entry* find(const wchar_t* id) noexcept;

} // namespace netshutup::catalog
