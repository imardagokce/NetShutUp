#pragma once

// selftest_mask.h — pure masking helpers for selftest reports.
// No I/O, no Windows calls. Hand-written scanners (no <regex>).

#include <string>

namespace netshutup::selftest::mask {

// Known provider addresses stay: 1.1.1.1 1.0.0.1 8.8.8.8 8.8.4.4 9.9.9.9 149.112.112.112
// 10.x.y.z -> 10.0.x.x ; every other IPv4 -> x.x.x.x
std::wstring mask_ipv4(const std::wstring& s);
// Known provider IPv6 (2606:4700:4700::1111, 2606:4700:4700::1001, 2001:4860:4860::8888,
// 2001:4860:4860::8844, 2620:fe::fe) stay; every other IPv6 -> x:x::x
std::wstring mask_ipv6(const std::wstring& s);
// AA-BB-CC-11-22-33 / aa:bb:cc:11:22:33 -> keep first 3 bytes, rest "xx" (same separator)
std::wstring mask_mac(const std::wstring& s);
// {4C1E9A02-1111-2222-3333-444455556666} -> {4C1E9A02-xxxx-xxxx-xxxx-xxxxxxxxxxxx}
std::wstring mask_guid(const std::wstring& s);
// X:\Users\<name>\... -> X:\Users\<user>\...
std::wstring mask_user_paths(const std::wstring& s);
// Applies all of the above to free text (MAC and GUID before IPv4/IPv6).
std::wstring sanitize_text(const std::wstring& s);

} // namespace netshutup::selftest::mask
