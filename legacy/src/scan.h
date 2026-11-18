#pragma once

// scan.h — Read-only system scan for NetShutUp Legacy
//
// scan_global() reads every GLOBAL-scope catalog entry from the registry
// without writing anything.  No elevation required.
//
// PER_ADAPTER entries are skipped until the adapter enumeration module is
// implemented (future milestone); they appear in the results with
// status = Status::ADAPTER_PENDING.

#include "catalog.h"
#include <optional>
#include <vector>

namespace netshutup::scan {

enum class Status {
    AT_DEFAULT,       // Current value matches the catalog default.
    MODIFIED,         // Current value differs from the catalog default.
    NOT_SET,          // Value absent (key does not exist); treated as default.
    ADAPTER_PENDING,  // PER_ADAPTER entry — not scanned yet.
};

struct Result {
    const catalog::Entry* entry;    // Pointer into the static catalog array.
    std::optional<DWORD>  current;  // Current registry value (nullopt = absent).
    Status                status;
};

// Scans all GLOBAL-scope catalog entries.
// Returns one Result per entry in catalog order.
// PER_ADAPTER entries are included with status = ADAPTER_PENDING.
// Never writes to the registry.
std::vector<Result> scan_all();

// Prints a formatted scan report to stdout.
// os_major / os_minor are passed for the header line.
void print_results(const std::vector<Result>& results,
                   DWORD os_major, DWORD os_minor, DWORD os_build);

} // namespace netshutup::scan
