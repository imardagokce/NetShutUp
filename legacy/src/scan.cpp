// scan.cpp — Read-only system scan (NetShutUp Legacy)

#include "scan.h"
#include "store.h"
#include <cstdio>

namespace netshutup::scan {

// ── scan_all ──────────────────────────────────────────────────────────────────

static Result make_result(const catalog::Entry& e, const targets::Target& t) {
    Result r{};
    r.entry = &e;
    r.label = t.id;
    r.current = store::read(t);
    if (!r.current.has_value()) {
        // Value absent. If the catalog says Windows does not create it by default,
        // absence is the expected default state.
        r.status = Status::NOT_SET;
    } else if (*r.current == e.default_dword) {
        r.status = Status::AT_DEFAULT;
    } else {
        r.status = Status::MODIFIED;
    }
    return r;
}

std::vector<Result> scan_all(const std::vector<std::wstring>* adapter_guids) {
    std::vector<Result> results;
    results.reserve(catalog::entry_count());

    std::vector<std::wstring> guids;
    bool guids_loaded = false;

    for (std::size_t i = 0; i < catalog::entry_count(); ++i) {
        const catalog::Entry& e = catalog::entries()[i];

        if (e.scope == catalog::Scope::PER_ADAPTER) {
            if (!guids_loaded) {
                guids = adapter_guids ? *adapter_guids : targets::physical_adapter_guids();
                guids_loaded = true;
            }
            if (guids.empty()) {
                Result r{};
                r.entry = &e;
                r.label = e.id;
                r.status = Status::ADAPTER_PENDING;
                results.push_back(r);
            }
            for (const auto& g : guids) results.push_back(make_result(e, targets::make_target(e, g)));
        } else {
            results.push_back(make_result(e, targets::make_target(e, L"")));
        }
    }

    return results;
}

// ── print_results ─────────────────────────────────────────────────────────────

static const wchar_t* status_label(Status s) noexcept {
    switch (s) {
        case Status::AT_DEFAULT:      return L"default";
        case Status::MODIFIED:        return L"modified";
        case Status::NOT_SET:         return L"not set";
        case Status::ADAPTER_PENDING: return L"(no adapter found)";
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
                W_ID,      targets::display_label(r.label).c_str(),
                W_CURRENT, cur_buf,
                W_DEFAULT, def_buf,
                           status_label(r.status));
    }

}

} // namespace netshutup::scan
