// main.cpp — NetShutUp Legacy entry point and command dispatcher

#include <windows.h>
#include <cstdio>
#include <cstring>
#include "version.h"
#include "scan.h"

// ── Usage ─────────────────────────────────────────────────────────────────────

static void print_usage() {
    wprintf(
        L"NetShutUp Legacy (pre-alpha)\n"
        L"Usage:\n"
        L"  netshutup-legacy                      Open the interactive menu\n"
        L"  netshutup-legacy scan                 Show current network settings\n"
        L"  netshutup-legacy preview <profile>    Show what a profile would change\n"
        L"  netshutup-legacy apply <profile>      Apply a profile (asks first)\n"
        L"  netshutup-legacy apply <profile> --dry-run\n"
        L"                                        Simulate apply without changes\n"
        L"  netshutup-legacy revert               Undo the most recent apply\n"
        L"  netshutup-legacy selftest             Test every profile and write report\n"
        L"\n"
        L"Profiles: privacy | performance | recommended\n"
        L"\n"
        L"apply, revert and selftest require administrator rights.\n"
        L"scan and preview do not.\n");
}

// ── Helpers ───────────────────────────────────────────────────────────────────

static bool arg_eq(const wchar_t* arg, const wchar_t* expected) noexcept {
    return _wcsicmp(arg, expected) == 0;
}

// ── Entry point ───────────────────────────────────────────────────────────────

int wmain(int argc, wchar_t* argv[]) {
    // Always print English text; never parse localized output.
    using namespace netshutup;

    OsVersion os = get_os_version();

    if (!os.is_supported()) {
        wprintf(L"Error: Windows %lu.%lu is not supported by NetShutUp Legacy.\n"
                L"This program targets Windows 7 SP1, 8, and 8.1.\n",
                os.major, os.minor);
        return 1;
    }

    // No arguments: show usage (menu not yet implemented).
    if (argc < 2) {
        wprintf(L"NetShutUp Legacy (pre-alpha) -- Windows %lu.%lu build %lu\n\n",
                os.major, os.minor, os.build);
        print_usage();
        return 0;
    }

    const wchar_t* cmd = argv[1];

    // ── scan ──────────────────────────────────────────────────────────────────
    if (arg_eq(cmd, L"scan")) {
        auto results = scan::scan_all();
        scan::print_results(results, os.major, os.minor, os.build);
        return 0;
    }

    // ── preview ───────────────────────────────────────────────────────────────
    if (arg_eq(cmd, L"preview")) {
        wprintf(L"preview: not yet implemented.\n");
        return 0;
    }

    // ── apply ─────────────────────────────────────────────────────────────────
    if (arg_eq(cmd, L"apply")) {
        wprintf(L"apply: not yet implemented.\n");
        return 0;
    }

    // ── revert ────────────────────────────────────────────────────────────────
    if (arg_eq(cmd, L"revert")) {
        wprintf(L"revert: not yet implemented.\n");
        return 0;
    }

    // ── selftest ──────────────────────────────────────────────────────────────
    if (arg_eq(cmd, L"selftest")) {
        wprintf(L"selftest: not yet implemented.\n");
        return 0;
    }

    // Unknown command
    wprintf(L"Unknown command: %ls\n\n", cmd);
    print_usage();
    return 1;
}
