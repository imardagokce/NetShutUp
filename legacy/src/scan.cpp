// scan.cpp — Read-only system scan (NetShutUp Legacy)

#include "scan.h"
#include "store.h"
#include <cstdio>

namespace netshutup::scan {

// ── scan_all ──────────────────────────────────────────────────────────────────

std::vector<Result> scan_all() {
    std::vector<Result> results;
    results.reserve(catalog::entry_count());

    for (std::size_t i = 0; i < catalog::entry_count(); ++i) {
        const catalog::Entry& e = catalog::entries()[i];
        Result r{};
        r.entry = &e;

        if (e.scope == catalog::Scope::PER_ADAPTER) {
            r.current = std::nullopt;
            r.status  = Status::ADAPTER_PENDING;
        } else {
            r.current = store::read(e);

            if (!r.current.has_value()) {
                // Value absent in registry.
                // If the catalog says Windows does not create it by default,
                // absence is the expected default state.
                r.status = Status::NOT_SET;
            } else if (*r.current == e.default_dword) {
                r.status = Status::AT_DEFAULT;
            } else {
                r.status = Status::MODIFIED;
            }
        }

        results.push_back(r);
    }

    return results;
}

// ── print_results ─────────────────────────────────────────────────────────────

static const wchar_t* status_label(Status s) noexcept {
    switch (s) {
        case Status::AT_DEFAULT:      return L"default";
        case Status::MODIFIED:        return L"modified";
        case Status::NOT_SET:         return L"not set";
        case Status::ADAPTER_PENDING: return L"(per-adapter, pending)";
    }
    return L"?";
}

void print_results(const std::vector<Result>& results,
                   DWORD os_major, DWORD os_minor, DWORD os_build) {
    wprintf(L"\nNetShutUp Legacy -- Scan Results\n");
    wprintf(L"=================================\n");
    wprintf(L"OS: Windows %lu.%lu build %lu\n\n", os_major, os_minor, os_build);

    // Column widths
    constexpr int W_ID      = 30;
    constexpr int W_CURRENT = 12;
    constexpr int W_DEFAULT = 12;
    constexpr int W_STATUS  = 24;

    wprintf(L"%-*s  %-*s  %-*s  %s\n",
            W_ID,      L"Setting",
            W_CURRENT, L"Current",
            W_DEFAULT, L"Default",
                       L"Status");
    wprintf(L"%.*s\n", W_ID + W_CURRENT + W_DEFAULT + W_STATUS + 6,
            L"----------------------------------------------------------------------");

    for (const auto& r : results) {
        const auto& e = *r.entry;

        // Current value column
        wchar_t cur_buf[24];
        if (r.current.has_value())
            _snwprintf_s(cur_buf, _countof(cur_buf), _TRUNCATE, L"%lu", *r.current);
        else
            _snwprintf_s(cur_buf, _countof(cur_buf), _TRUNCATE, L"-");

        // Default value column
        wchar_t def_buf[24];
        if (e.default_exists)
            _snwprintf_s(def_buf, _countof(def_buf), _TRUNCATE, L"%lu", e.default_dword);
        else
            _snwprintf_s(def_buf, _countof(def_buf), _TRUNCATE, L"(absent)");

        wprintf(L"%-*ls  %-*ls  %-*ls  %ls\n",
                W_ID,      e.id,
                W_CURRENT, cur_buf,
                W_DEFAULT, def_buf,
                           status_label(r.status));
    }

    wprintf(L"\nNote: per-adapter settings are not shown until the adapter\n"
            L"      enumeration module is implemented.\n");
}

} // namespace netshutup::scan
