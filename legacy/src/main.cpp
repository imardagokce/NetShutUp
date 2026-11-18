// main.cpp — NetShutUp Legacy entry point and command dispatcher

#include <windows.h>
#include <cstdio>
#include <cstring>
#include "version.h"
#include "scan.h"
#include "preview.h"
#include "elevation.h"
#include "apply.h"
#include "menu.h"
#include "selftest.h"

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

    bool ignore_os = false;
    for (int i = 1; i < argc; ++i) {
        if (arg_eq(argv[i], L"--ignore-os")) {
            ignore_os = true;
            break;
        }
    }

    if (!os.is_supported() && !ignore_os) {
        wprintf(L"Error: Windows %lu.%lu is not supported by NetShutUp Legacy.\n"
                L"This program targets Windows 7 SP1, 8, and 8.1.\n"
                L"(Use --ignore-os to run read-only diagnostics on this machine)\n",
                os.major, os.minor);
        return 1;
    }

    // If no command is given, launch the interactive console menu
    if (argc < 2 || (argc == 2 && ignore_os)) {
        return menu::run(os);
    }

    const wchar_t* cmd = argv[1];

    // ── help ──────────────────────────────────────────────────────────────────
    if (arg_eq(cmd, L"help") || arg_eq(cmd, L"--help") || arg_eq(cmd, L"-h") || arg_eq(cmd, L"/?")) {
        wprintf(L"NetShutUp Legacy (pre-alpha) -- Windows %lu.%lu build %lu\n\n",
                os.major, os.minor, os.build);
        print_usage();
        return 0;
    }

    // ── scan ──────────────────────────────────────────────────────────────────
    if (arg_eq(cmd, L"scan")) {
        auto results = scan::scan_all();
        scan::print_results(results, os.major, os.minor, os.build);
        return 0;
    }

    // ── preview ───────────────────────────────────────────────────────────────
    if (arg_eq(cmd, L"preview")) {
        if (argc < 3) {
            wprintf(L"Error: 'preview' requires a profile name.\n"
                    L"Profiles: privacy | performance | recommended\n");
            return 1;
        }

        const wchar_t* profile = argv[2];
        auto preview_res = preview::generate(profile, os.os_mask());
        if (!preview_res.has_value()) {
            wprintf(L"Error: Unknown profile '%ls'.\n"
                    L"Profiles: privacy | performance | recommended\n", profile);
            return 1;
        }

        preview::print_preview(*preview_res);
        return 0;
    }

    // ── apply ─────────────────────────────────────────────────────────────────
    if (arg_eq(cmd, L"apply")) {
        if (argc < 3) {
            wprintf(L"Error: 'apply' requires a profile name.\n"
                    L"Profiles: privacy | performance | recommended\n");
            return 1;
        }

        const wchar_t* profile = argv[2];
        bool dry_run = false;
        for (int i = 3; i < argc; ++i) {
            if (arg_eq(argv[i], L"--dry-run")) {
                dry_run = true;
            }
        }

        // Elevation requirement: only real apply requires elevation
        if (!dry_run && !elevation::is_elevated()) {
            wprintf(L"Elevating to administrator rights...\n");
            return elevation::restart_elevated(argc, argv);
        }

        auto res = apply::apply_profile(profile, os.os_mask(), dry_run);
        apply::print_apply_result(res);
        return res.success ? 0 : 1;
    }

    // ── revert ────────────────────────────────────────────────────────────────
    if (arg_eq(cmd, L"revert")) {
        // Revert writes to system registry keys and requires elevation
        if (!elevation::is_elevated()) {
            wprintf(L"Elevating to administrator rights...\n");
            return elevation::restart_elevated(argc, argv);
        }

        std::wstring specific_backup;
        if (argc >= 3 && argv[2][0] != L'-') {
            specific_backup = argv[2];
        }

        auto res = apply::revert(specific_backup);
        apply::print_revert_result(res);
        return res.success ? 0 : 1;
    }

    // ── selftest ──────────────────────────────────────────────────────────────
    if (arg_eq(cmd, L"selftest")) {
        return selftest::cmd_selftest(os, argc, argv);
    }

    // Unknown command
    wprintf(L"Unknown command: %ls\n\n", cmd);
    print_usage();
    return 1;
}
