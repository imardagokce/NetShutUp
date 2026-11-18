#pragma once

// firewall.h — Windows Firewall access for NetShutUp Legacy (Vista+ COM API).
//
// Rules (see docs/design/2026-11-18-legacy-firewall-6a.md):
//  - Read/write through INetFwPolicy2 only; netsh output is never parsed.
//  - A write is read back; if the effective value differs (Group Policy
//    override) write() returns false so the apply engine rolls everything back.
//  - If a third-party firewall is registered, the firewall.* settings are left
//    alone (skip_reason() explains why).

#include <windows.h>
#include <optional>
#include <string>
#include <vector>

namespace netshutup::firewall {

// profile: L"domain" | L"private" | L"public"
// what:    L"enabled" (0 off, 1 on) | L"inbound" (0 block, 1 allow)
// Returns nullopt if the firewall service/API is unavailable or args are invalid.
std::optional<DWORD> read(const wchar_t* profile, const wchar_t* what);

// Writes the value, then reads it back. true only if the effective value equals `value`.
bool write(const wchar_t* profile, const wchar_t* what, DWORD value);

// Pure: a registered firewall whose display name does not start with "Windows"
// (case-insensitive) counts as third party.
bool is_third_party_name(const std::wstring& display_name);

// Names of firewall products registered in Windows Security Center
// (root\SecurityCenter2, FirewallProduct). Empty if the list is unreadable.
std::vector<std::wstring> registered_products();

// Empty string when Windows Firewall settings may be changed; otherwise a
// one-line English reason (third-party firewall registered).
std::wstring skip_reason();

} // namespace netshutup::firewall
