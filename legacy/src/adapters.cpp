// adapters.cpp — Physical network adapter enumeration implementation for NetShutUp Legacy

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>

#include "adapters.h"
#include <algorithm>
#include <cwctype>

#pragma comment(lib, "iphlpapi.lib")

namespace netshutup::adapters {

// ── Virtual / Non-Physical Hardware Filter ────────────────────────────────────

static std::wstring to_lower(const std::wstring& s) {
    std::wstring lower = s;
    for (auto& ch : lower) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return lower;
}

static bool contains(const std::wstring& haystack, const wchar_t* needle) {
    return haystack.find(needle) != std::wstring::npos;
}

static bool is_virtual_or_pseudo_adapter(const std::wstring& desc, const std::wstring& friendly) {
    std::wstring d = to_lower(desc);
    std::wstring f = to_lower(friendly);

    const wchar_t* const k_blacklisted_keywords[] = {
        L"virtual",
        L"vmware",
        L"hyper-v",
        L"virtualbox",
        L"tap-",
        L"tap ",
        L"vpn",
        L"wireguard",
        L"pcap",
        L"loopback",
        L"wan miniport",
        L"wi-fi direct",
        L"direct virtual",
        L"bluetooth",
        L"filter miniport",
        L"ndiswan",
        L"wsl",
        L"fortinet",
        L"cisco anyconnect",
        L"openvpn"
    };

    for (const auto* kw : k_blacklisted_keywords) {
        if (contains(d, kw) || contains(f, kw)) {
            return true;
        }
    }

    return false;
}

// ── Enumeration ───────────────────────────────────────────────────────────────

std::vector<PhysicalAdapter> get_physical_adapters() {
    std::vector<PhysicalAdapter> result;

    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG buffer_size = 16384; // 16 KB initial buffer
    std::vector<BYTE> buffer(buffer_size);

    ULONG status = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr,
                                        reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()),
                                        &buffer_size);

    if (status == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(buffer_size);
        status = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr,
                                      reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()),
                                      &buffer_size);
    }

    if (status != NO_ERROR) {
        return result;
    }

    auto* curr = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
    while (curr) {
        // Only accept physical Ethernet (6) and Wi-Fi (71) interfaces
        if (curr->IfType == IF_TYPE_ETHERNET_CSMACD || curr->IfType == IF_TYPE_IEEE80211) {
            std::wstring desc = curr->Description ? curr->Description : L"";
            std::wstring friendly = curr->FriendlyName ? curr->FriendlyName : L"";

            if (!is_virtual_or_pseudo_adapter(desc, friendly)) {
                // AdapterName is the GUID string (ANSI on Win32, e.g. "{...}")
                std::string name_ansi = curr->AdapterName ? curr->AdapterName : "";
                int wide_len = MultiByteToWideChar(CP_ACP, 0, name_ansi.c_str(), -1, nullptr, 0);

                std::wstring guid_wide;
                if (wide_len > 1) {
                    guid_wide.resize(wide_len - 1);
                    MultiByteToWideChar(CP_ACP, 0, name_ansi.c_str(), -1, &guid_wide[0], wide_len);
                }

                if (!guid_wide.empty()) {
                    PhysicalAdapter adapter;
                    adapter.guid = guid_wide;
                    adapter.friendly_name = friendly;
                    adapter.description = desc;
                    adapter.type = (curr->IfType == IF_TYPE_IEEE80211) ? AdapterType::WIFI : AdapterType::ETHERNET;
                    adapter.is_connected = (curr->OperStatus == IfOperStatusUp);
                    adapter.if_index = curr->IfIndex;

                    result.push_back(adapter);
                }
            }
        }
        curr = curr->Next;
    }

    return result;
}

// ── Subkey Formatting ─────────────────────────────────────────────────────────

std::wstring format_subkey_for_adapter(const wchar_t* subkey_template,
                                       const std::wstring& adapter_guid) {
    if (!subkey_template) return {};

    std::wstring s = subkey_template;
    constexpr wchar_t k_token[] = L"{ADAPTER-GUID}";
    std::size_t pos = s.find(k_token);

    while (pos != std::wstring::npos) {
        s.replace(pos, wcslen(k_token), adapter_guid);
        pos = s.find(k_token, pos + adapter_guid.length());
    }

    return s;
}

} // namespace netshutup::adapters
