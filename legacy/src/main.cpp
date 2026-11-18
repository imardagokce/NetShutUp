// main.cpp — NetShutUp Legacy entry point (placeholder)
//
// This file will be replaced with the full CLI / menu dispatcher.
// For now it detects the OS version and prints a status line so the
// build pipeline can verify the binary runs on the target OS.

#include <windows.h>
#include <cstdio>
#include "version.h"

int wmain(int /*argc*/, wchar_t* /*argv*/[]) {
    using namespace netshutup;

    OsVersion os = get_os_version();

    // Localized output is never parsed — keep all program text in English.
    wprintf(
        L"NetShutUp Legacy (pre-alpha)\n"
        L"OS: Windows %lu.%lu build %lu",
        os.major, os.minor, os.build);

    if (os.is_windows_7())   wprintf(L" (Windows 7)\n");
    else if (os.is_windows_8())   wprintf(L" (Windows 8)\n");
    else if (os.is_windows_8_1()) wprintf(L" (Windows 8.1)\n");
    else                          wprintf(L" (unsupported)\n");

    if (!os.is_supported()) {
        wprintf(L"Error: this OS version is not supported by NetShutUp Legacy.\n");
        return 1;
    }

    wprintf(L"\nNothing to do yet — full implementation coming soon.\n");
    return 0;
}
