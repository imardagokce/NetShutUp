#pragma once

// adapters.h — Physical network adapter enumeration for NetShutUp Legacy
//
// Rules (from GEMINI.md design principles):
//  - "Yalnızca gerçek donanım:" Adapter settings are only applied to physical
//    Ethernet and Wi-Fi adapters.
//  - VirtualBox, VMware, Hyper-V, VPNs, TAP devices, and WAN miniports are excluded.

#include <windows.h>
#include <string>
#include <vector>

namespace netshutup::adapters {

enum class AdapterType {
    ETHERNET,
    WIFI,
    OTHER
};

struct PhysicalAdapter {
    std::wstring guid;          // Registry GUID format, e.g. L"{B92C2366-D0BB-4241-8664-9D97B8964720}"
    std::wstring friendly_name; // e.g. L"Local Area Connection"
    std::wstring description;   // e.g. L"Intel(R) 82579V Gigabit Network Connection"
    AdapterType  type;
    bool         is_connected;
    unsigned long if_index = 0; // IPv4 interface index (netsh name=<index>)
};

// Enumerates all physical Ethernet and Wi-Fi adapters.
// Returns an empty list if no physical adapters are found or if enumeration fails.
std::vector<PhysicalAdapter> get_physical_adapters();

// Replaces all occurrences of "{ADAPTER-GUID}" in a registry subkey template
// with the provided adapter GUID.
std::wstring format_subkey_for_adapter(const wchar_t* subkey_template,
                                       const std::wstring& adapter_guid);

} // namespace netshutup::adapters
