// elevation.cpp — UAC elevation implementation for NetShutUp Legacy

#include "elevation.h"
#include <cstdio>
#include <string>
#include <shellapi.h>

namespace netshutup::elevation {

bool is_elevated() noexcept {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        return false;
    }

    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    BOOL ok = GetTokenInformation(hToken, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(hToken);

    return ok && (elevation.TokenIsElevated != 0);
}

int restart_elevated(int argc, wchar_t* argv[]) noexcept {
    wchar_t exe_path[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    if (len == 0 || len == MAX_PATH) {
        return 1;
    }

    // Build argument string with quotes where necessary
    std::wstring params;
    for (int i = 1; i < argc; ++i) {
        if (!params.empty()) {
            params += L" ";
        }
        bool has_space = (wcschr(argv[i], L' ') != nullptr);
        if (has_space) params += L"\"";
        params += argv[i];
        if (has_space) params += L"\"";
    }

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = exe_path;
    sei.lpParameters = params.empty() ? nullptr : params.c_str();
    sei.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&sei)) {
        DWORD err = GetLastError();
        if (err == ERROR_CANCELLED) {
            wprintf(L"\nOperation cancelled by user (UAC prompt declined).\n");
            return 1223; // ERROR_CANCELLED
        }
        wprintf(L"\nFailed to elevate process (error %lu).\n", err);
        return 1;
    }

    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, INFINITE);
        DWORD exit_code = 0;
        GetExitCodeProcess(sei.hProcess, &exit_code);
        CloseHandle(sei.hProcess);
        return static_cast<int>(exit_code);
    }

    return 0;
}

} // namespace netshutup::elevation
