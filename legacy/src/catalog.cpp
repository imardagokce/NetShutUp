// catalog.cpp — Settings catalog data for NetShutUp Legacy
//
// Adding a new entry checklist:
//   1. Verify the registry path and value name in Microsoft documentation.
//   2. Confirm the Windows default value and whether the key/value exists
//      out-of-box on each supported OS (Win7 SP1, Win8, Win8.1).
//   3. Record the doc_ref URL or KB article.
//   4. Update docs/compatibility-matrix.md if OS support differs by version.
//   5. Add or update the relevant unit test.

#include "catalog.h"
#include <cstddef>
#include <cwchar>

namespace netshutup::catalog {

// ── Catalog data ──────────────────────────────────────────────────────────────
//
// Column order: id | hive | subkey | value_name | scope
//             | default_dword | default_exists | target_dword
//             | profiles | os_mask | description | doc_ref
//
// Formatting note: subkeys use Windows forward-slashes only in comments;
// the actual strings use backslashes as required by Win32 API.

static const Entry k_entries[] = {

    // ── Privacy: LLMNR ────────────────────────────────────────────────────────
    // Link-Local Multicast Name Resolution broadcasts the unqualified host
    // name on the local network segment, leaking it to all LAN hosts.
    // Setting EnableMulticast to 0 disables LLMNR.
    // This is a Group Policy-backed key; creating it without a policy is
    // supported and effective on Vista+ (including Win7/8/8.1).
    //
    // Default: key/value typically absent (treated as 1 = enabled).
    // Source: https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-llmnrp
    //         Group Policy: Computer\Admin Templates\Network\DNS Client\Turn off multicast name resolution
    {
        L"privacy.llmnr",
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Policies\\Microsoft\\Windows NT\\DNSClient",
        L"EnableMulticast",
        Scope::GLOBAL,
        /*default_dword*/ 1,
        /*default_exists*/ false,       // key absent = LLMNR enabled
        /*target_dword*/  0,            // 0 = disabled
        PROFILE_PRIVACY | PROFILE_RECOMMENDED,
        OS_ALL,
        L"Disable LLMNR (Link-Local Multicast Name Resolution)",
        L"https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-llmnrp"
    },

    // ── Privacy: Customer Experience Improvement Program (CEIP) ──────────────
    // Controls whether Windows collects and sends usage data to Microsoft.
    // Setting CEIPEnable to 0 opts out of CEIP.
    //
    // Default: 1 (opted in, though the out-of-box installer asks the user).
    // Source: https://learn.microsoft.com/en-us/windows/privacy/configure-windows-diagnostic-data-in-your-organization
    //         KB2693671
    {
        L"privacy.ceip",
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\SQMClient\\Windows",
        L"CEIPEnable",
        Scope::GLOBAL,
        /*default_dword*/ 1,
        /*default_exists*/ true,
        /*target_dword*/  0,            // 0 = opted out
        PROFILE_PRIVACY,
        OS_ALL,
        L"Disable CEIP (Customer Experience Improvement Program)",
        L"KB2693671; https://learn.microsoft.com/en-us/windows/privacy"
    },

    // ── Privacy: Windows Error Reporting ─────────────────────────────────────
    // When enabled, crash data is sent to Microsoft. Setting Disabled to 1
    // prevents WER from sending any reports.
    //
    // Default: 0 (enabled, key may or may not exist).
    // Source: https://learn.microsoft.com/en-us/windows/win32/wer/wer-settings
    {
        L"privacy.wer",
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\Windows Error Reporting",
        L"Disabled",
        Scope::GLOBAL,
        /*default_dword*/ 0,
        /*default_exists*/ false,       // key may be absent = WER enabled
        /*target_dword*/  1,            // 1 = disabled
        PROFILE_PRIVACY,
        OS_ALL,
        L"Disable Windows Error Reporting (WER)",
        L"https://learn.microsoft.com/en-us/windows/win32/wer/wer-settings"
    },

    // ── Performance: Multimedia Network Throttling ────────────────────────────
    // Windows throttles non-multimedia network traffic during media playback
    // to protect audio/video streams. NetworkThrottlingIndex controls this;
    // 0xFFFFFFFF disables throttling entirely.
    //
    // Default: 10 (throttled to ~10 packets per millisecond).
    // Source: https://learn.microsoft.com/en-us/windows/win32/procthread/multimedia-class-scheduler-service
    //         KB2578159
    {
        L"perf.net_throttle",
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile",
        L"NetworkThrottlingIndex",
        Scope::GLOBAL,
        /*default_dword*/ 10,
        /*default_exists*/ true,
        /*target_dword*/  0xFFFFFFFF,   // disabled
        PROFILE_PERFORMANCE,
        OS_ALL,
        L"Disable multimedia network throttling (NetworkThrottlingIndex)",
        L"https://learn.microsoft.com/en-us/windows/win32/procthread/multimedia-class-scheduler-service"
    },

    // ── Performance: TCP No Delay (per adapter) ───────────────────────────────
    // Disables the Nagle algorithm for the adapter, sending small packets
    // immediately instead of coalescing them. Reduces latency at the cost
    // of slightly higher overhead.
    //
    // Applied per physical adapter; {ADAPTER-GUID} is substituted at runtime.
    // Default: 0 (Nagle enabled).
    // Source: https://learn.microsoft.com/en-us/troubleshoot/windows-server/networking/registry-entry-control-tcp-delayed-acknowledgment
    //         KB328890
    {
        L"perf.tcp_nodelay",
        HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces\\{ADAPTER-GUID}",
        L"TCPNoDelay",
        Scope::PER_ADAPTER,
        /*default_dword*/ 0,
        /*default_exists*/ false,
        /*target_dword*/  1,            // 1 = no delay (Nagle disabled)
        PROFILE_PERFORMANCE,
        OS_ALL,
        L"Disable Nagle algorithm per adapter (TCPNoDelay)",
        L"KB328890"
    },

    // ── Performance: TCP ACK Frequency (per adapter) ─────────────────────────
    // Controls how many TCP segments are received before sending an ACK.
    // Default is 2 (or 200 ms, whichever comes first).  Setting to 1 sends
    // an ACK after every segment, reducing round-trip latency for real-time
    // traffic.
    //
    // Applied per physical adapter; {ADAPTER-GUID} is substituted at runtime.
    // Default: 2 segments (key usually absent; absence = default behaviour).
    // Source: KB328890
    {
        L"perf.tcp_ack_freq",
        HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces\\{ADAPTER-GUID}",
        L"TcpAckFrequency",
        Scope::PER_ADAPTER,
        /*default_dword*/ 2,
        /*default_exists*/ false,
        /*target_dword*/  1,            // 1 = ACK immediately
        PROFILE_PERFORMANCE,
        OS_ALL,
        L"ACK every TCP segment per adapter (TcpAckFrequency)",
        L"KB328890"
    },

}; // k_entries[]

// ── Catalog access ────────────────────────────────────────────────────────────

const Entry* entries() noexcept {
    return k_entries;
}

std::size_t entry_count() noexcept {
    return sizeof(k_entries) / sizeof(k_entries[0]);
}

const Entry* find(const wchar_t* id) noexcept {
    for (std::size_t i = 0; i < entry_count(); ++i) {
        if (std::wcscmp(k_entries[i].id, id) == 0)
            return &k_entries[i];
    }
    return nullptr;
}

} // namespace netshutup::catalog
