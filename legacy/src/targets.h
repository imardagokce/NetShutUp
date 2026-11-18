#pragma once

// targets.h — turns catalog entries into concrete registry/firewall targets.
//
// Most entries map to exactly one location. A PER_ADAPTER entry maps to one location
// per physical adapter: its subkey template contains "{ADAPTER-GUID}", which is
// replaced by each adapter's GUID, and its target id becomes "<entry id>@<guid>"
// (the id format used in backup files and in the selftest snapshot).

#include "catalog.h"
#include <string>
#include <utility>
#include <vector>

namespace netshutup::targets {

struct Target {
    const catalog::Entry* entry = nullptr;
    std::wstring          id;       // entry id, or "<entry id>@{GUID}" for per-adapter targets
    std::wstring          subkey;   // resolved subkey (FIREWALL: the firewall profile name)
};

// Expands `entries` (nullptr entries are skipped) in order. GLOBAL and FIREWALL entries give
// one target each. A PER_ADAPTER entry gives one target per GUID, in GUID order; with an empty
// GUID list it gives none.
std::vector<Target> expand(const std::vector<const catalog::Entry*>& entries,
                           const std::vector<std::wstring>& adapter_guids);

// "perf.tcp_nodelay@{GUID}" -> {"perf.tcp_nodelay", "{GUID}"}; an id without '@' -> {id, L""}.
std::pair<std::wstring, std::wstring> split_id(const std::wstring& id);

// Builds the target for one entry and one GUID (empty GUID for non-per-adapter entries).
Target make_target(const catalog::Entry& entry, const std::wstring& guid);

// Display form of a target id: the GUID of a per-adapter id is shortened to its first group,
// "perf.tcp_nodelay@{D7747AC7-885F-4A75-8F0C-84BA153614EC}" -> "perf.tcp_nodelay@{D7747AC7..}".
// Ids without '@' (and malformed ones) are returned unchanged. Display only: never use it as an id.
std::wstring display_label(const std::wstring& id);

// GUIDs of the physical Ethernet/Wi-Fi adapters (real enumeration, see adapters.h).
std::vector<std::wstring> physical_adapter_guids();

} // namespace netshutup::targets
