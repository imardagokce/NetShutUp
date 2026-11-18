#pragma once

// selftest.h — Tester mode: run every profile through apply -> verify ->
// revert -> compare and write a masked report.
//
// The runner (selftest_runner.cpp) is pure: all side effects come in through
// Env, so unit tests drive it with an in-memory fake and never touch the
// registry. selftest.cpp wires the real engine and the safety net.

#include "apply.h"
#include "selftest_report.h"
#include "version.h"
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace netshutup::selftest {

// Catalog id -> current value (nullopt = "not set"). Same order every call.
using Snapshot = std::vector<std::pair<std::wstring, std::optional<DWORD>>>;

struct Env {
    Snapshot            (*snapshot)(void* ctx);
    apply::ApplyResult  (*apply)(void* ctx, const wchar_t* profile);
    apply::RevertResult (*revert)(void* ctx, const std::wstring& backup_path);
    unsigned long       (*now_ms)(void* ctx);
    void*               ctx;
    // Optional (may be null). Returns and clears a one-line note about the DNS stage of the
    // LAST apply/revert call. Prefix "FAIL: " -> Fail check, "SKIP: " -> Skip check, any other
    // non-empty text -> Pass check; empty -> no check is added.
    std::wstring        (*take_note)(void* ctx) = nullptr;
};

// Ids whose value differs between a and b ("not set" and 0 are different).
std::vector<std::wstring> diff(const Snapshot& a, const Snapshot& b);

// Runs the loop for each profile. Stops (all_restored=false, manual_revert_cmd
// set) if a revert fails. Never throws for engine failures.
Report run(const Env& env, const std::vector<std::wstring>& profiles);

// ── Real wiring (selftest.cpp) ───────────────────────────────────────────────

// Registry .reg exports + system restore point. A failed export aborts the
// selftest; a missing/disabled restore point is only reported as Skip.
struct SafetyNet {
    bool         reg_export_ok = false;
    std::wstring reg_export_path;
    bool         restore_point_ok = false;
    std::wstring restore_point_note;
    std::wstring error;
};
SafetyNet create_safety_net();

// Entry point for `selftest` and the menu item.
int cmd_selftest(const OsVersion& os, int argc, wchar_t* argv[]);

} // namespace netshutup::selftest
