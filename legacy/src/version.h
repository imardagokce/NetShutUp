#pragma once

// version.h — OS version detection for NetShutUp Legacy
//
// Always use get_os_version() / OsVersion instead of GetVersionExW.
// GetVersionExW returns a compatibility-shim result on Windows 8.1+ when
// the application manifest does not declare the matching supportedOS GUID.
// RtlGetVersion (ntdll) always returns the real version.

#include <windows.h>

namespace netshutup {

struct OsVersion {
    DWORD major;   // e.g. 6
    DWORD minor;   // e.g. 1  (6.1 == Windows 7)
    DWORD build;   // e.g. 7601

    // Convenience helpers
    bool is_windows_7()   const { return major == 6 && minor == 1; }
    bool is_windows_8()   const { return major == 6 && minor == 2; }
    bool is_windows_8_1() const { return major == 6 && minor == 3; }

    bool is_supported() const {
        // Supported: Windows 7 SP1 (6.1.7601), 8 (6.2), 8.1 (6.3)
        return (major == 6 && minor >= 1 && minor <= 3);
    }
};

// Returns the real OS version via RtlGetVersion.
// Returns {0,0,0} only if ntdll cannot be loaded (should never happen).
OsVersion get_os_version();

} // namespace netshutup
