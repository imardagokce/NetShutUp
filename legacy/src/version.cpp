// version.cpp — OS version detection via RtlGetVersion (ntdll)
//
// RtlGetVersion is an ntdll export available since Windows 2000.
// We load it at runtime with GetProcAddress to avoid a hard import that
// would require linking against ntdll.lib directly.

#include "version.h"

// NTSTATUS is LONG; STATUS_SUCCESS is 0.
constexpr LONG STATUS_SUCCESS_VAL = 0;

// Keep ntdll.h out of the global namespace — just use the function signature.
using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);

namespace netshutup {

OsVersion get_os_version() {
    OsVersion v{};

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) {
        // ntdll is always loaded; this branch should never be reached.
        return v;
    }

    auto fn = reinterpret_cast<RtlGetVersionFn>(
        GetProcAddress(ntdll, "RtlGetVersion"));

    if (!fn) {
        return v;
    }

    RTL_OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);

    if (fn(&info) == STATUS_SUCCESS_VAL) {
        v.major = info.dwMajorVersion;
        v.minor = info.dwMinorVersion;
        v.build = info.dwBuildNumber;
    }

    return v;
}

} // namespace netshutup
