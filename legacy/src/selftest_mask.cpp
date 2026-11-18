// selftest_mask.cpp — pure masking helpers for selftest reports.
// Hand-written scanners only (no <regex>). No I/O.

#include "selftest_mask.h"

#include <cstddef>

namespace netshutup::selftest::mask {

namespace {

using std::size_t;

bool is_digit(wchar_t c) { return c >= L'0' && c <= L'9'; }

bool is_ascii_alpha(wchar_t c) {
    return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z');
}

bool is_hex(wchar_t c) {
    return is_digit(c) || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
}

bool is_hex_or_colon(wchar_t c) { return is_hex(c) || c == L':'; }

wchar_t to_lower_char(wchar_t c) {
    return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c;
}

std::wstring to_lower(std::wstring s) {
    for (auto& c : s) c = to_lower_char(c);
    return s;
}

// ── IPv4 ─────────────────────────────────────────────────────────────────────

bool is_known_ipv4(const std::wstring& m) {
    static const wchar_t* const kKnown[] = {
        L"1.1.1.1", L"1.0.0.1", L"8.8.8.8", L"8.8.4.4", L"9.9.9.9", L"149.112.112.112",
    };
    for (const wchar_t* k : kKnown) {
        if (m == k) return true;
    }
    return false;
}

// Matches d{1,3}.d{1,3}.d{1,3}.d{1,3} starting exactly at i.
// Rejects matches glued to a longer number ("1.2.3.4.5", "1234.5.6.7").
bool match_ipv4_at(const std::wstring& s, size_t i, size_t& end) {
    if (i > 0) {
        if (is_digit(s[i - 1])) return false;
        if (s[i - 1] == L'.' && i > 1 && is_digit(s[i - 2])) return false;
    }
    size_t pos = i;
    for (size_t group = 0; group < 4; ++group) {
        size_t n = 0;
        while (pos < s.size() && is_digit(s[pos]) && n < 3) {
            ++pos;
            ++n;
        }
        if (n == 0) return false;
        if (group < 3) {
            if (pos >= s.size() || s[pos] != L'.') return false;
            ++pos;
        }
    }
    if (pos < s.size()) {
        if (is_digit(s[pos])) return false;
        if (s[pos] == L'.' && pos + 1 < s.size() && is_digit(s[pos + 1])) return false;
    }
    end = pos;
    return true;
}

// ── IPv6 ─────────────────────────────────────────────────────────────────────

// Counts colon-separated groups in `part`; each must be 1..4 hex digits.
// An empty part is valid and has zero groups.
bool groups_ok(const std::wstring& part, size_t& count) {
    count = 0;
    if (part.empty()) return true;
    size_t start = 0;
    while (true) {
        size_t colon = part.find(L':', start);
        size_t stop = (colon == std::wstring::npos) ? part.size() : colon;
        size_t len = stop - start;
        if (len == 0 || len > 4) return false;
        for (size_t k = start; k < stop; ++k) {
            if (!is_hex(part[k])) return false;
        }
        ++count;
        if (colon == std::wstring::npos) break;
        start = colon + 1;
    }
    return true;
}

// Syntactic check for a textual IPv6 address made only of hex digits and colons.
bool is_ipv6_address(const std::wstring& t) {
    size_t dc = t.find(L"::");
    if (dc == std::wstring::npos) {
        size_t n = 0;
        return groups_ok(t, n) && n == 8;
    }
    if (t.find(L":::") != std::wstring::npos) return false;
    if (t.find(L"::", dc + 2) != std::wstring::npos) return false;
    size_t left = 0;
    size_t right = 0;
    if (!groups_ok(t.substr(0, dc), left)) return false;
    if (!groups_ok(t.substr(dc + 2), right)) return false;
    return left + right >= 1 && left + right <= 7;
}

bool is_known_ipv6(const std::wstring& lowered) {
    static const wchar_t* const kKnown[] = {
        L"2606:4700:4700::1111", L"2606:4700:4700::1001",
        L"2001:4860:4860::8888", L"2001:4860:4860::8844",
        L"2620:fe::fe",
    };
    for (const wchar_t* k : kKnown) {
        if (lowered == k) return true;
    }
    return false;
}

// ── MAC ──────────────────────────────────────────────────────────────────────

// Matches XX<sep>XX<sep>XX<sep>XX<sep>XX<sep>XX starting exactly at i,
// with one consistent separator ('-' or ':').
bool match_mac_at(const std::wstring& s, size_t i, size_t& end) {
    if (i + 17 > s.size()) return false;
    if (i > 0 && is_hex(s[i - 1])) return false;
    if (i > 1 && (s[i - 1] == L'-' || s[i - 1] == L':') && is_hex(s[i - 2])) return false;

    const wchar_t sep = s[i + 2];
    if (sep != L'-' && sep != L':') return false;
    for (size_t g = 0; g < 6; ++g) {
        size_t p = i + g * 3;
        if (!is_hex(s[p]) || !is_hex(s[p + 1])) return false;
        if (g < 5 && s[p + 2] != sep) return false;
    }

    end = i + 17;
    if (end < s.size()) {
        if (is_hex(s[end])) return false;
        if (s[end] == sep && end + 1 < s.size() && is_hex(s[end + 1])) return false;
    }
    return true;
}

// ── GUID ─────────────────────────────────────────────────────────────────────

// Matches {8-4-4-4-12} hex with braces, starting exactly at i (38 chars).
bool match_guid_at(const std::wstring& s, size_t i) {
    static const size_t kGroups[] = {8, 4, 4, 4, 12};
    if (i + 38 > s.size()) return false;
    if (s[i] != L'{' || s[i + 37] != L'}') return false;
    size_t p = i + 1;
    for (size_t g = 0; g < 5; ++g) {
        for (size_t k = 0; k < kGroups[g]; ++k) {
            if (!is_hex(s[p++])) return false;
        }
        if (g < 4) {
            if (s[p] != L'-') return false;
            ++p;
        }
    }
    return p == i + 37;
}

// ── User paths ───────────────────────────────────────────────────────────────

bool is_path_name_stop(wchar_t c) {
    return c == L'\\' || c == L'/' || c == L'"' || c == L'\r' || c == L'\n' || c == L'\t';
}

} // namespace

std::wstring mask_ipv4(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        size_t end = 0;
        if (is_digit(s[i]) && match_ipv4_at(s, i, end)) {
            std::wstring m = s.substr(i, end - i);
            if (is_known_ipv4(m)) {
                out += m;
            } else if (m.compare(0, 3, L"10.") == 0) {
                out += L"10.0.x.x";
            } else {
                out += L"x.x.x.x";
            }
            i = end;
        } else {
            out += s[i];
            ++i;
        }
    }
    return out;
}

std::wstring mask_ipv6(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        if (!is_hex_or_colon(s[i])) {
            out += s[i];
            ++i;
            continue;
        }
        size_t j = i;
        while (j < s.size() && is_hex_or_colon(s[j])) ++j;
        std::wstring tok = s.substr(i, j - i);
        if (is_ipv6_address(tok) && !is_known_ipv6(to_lower(tok))) {
            out += L"x:x::x";
        } else {
            out += tok;
        }
        i = j;
    }
    return out;
}

std::wstring mask_mac(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        size_t end = 0;
        if (match_mac_at(s, i, end)) {
            const wchar_t sep = s[i + 2];
            out += s.substr(i, 8);  // first three groups kept as written
            out += sep;
            out += L"xx";
            out += sep;
            out += L"xx";
            out += sep;
            out += L"xx";
            i = end;
        } else {
            out += s[i];
            ++i;
        }
    }
    return out;
}

std::wstring mask_guid(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        if (match_guid_at(s, i)) {
            out += L'{';
            out += s.substr(i + 1, 8);
            out += L"-xxxx-xxxx-xxxx-xxxxxxxxxxxx}";
            i += 38;
        } else {
            out += s[i];
            ++i;
        }
    }
    return out;
}

std::wstring mask_user_paths(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        // Prefix "X:\Users\" is 9 characters.
        bool prefix = i + 9 <= s.size() && is_ascii_alpha(s[i]) && s[i + 1] == L':' &&
                      s[i + 2] == L'\\' && s[i + 8] == L'\\' &&
                      to_lower(s.substr(i + 3, 5)) == L"users";
        if (prefix) {
            size_t name_start = i + 9;
            size_t q = name_start;
            while (q < s.size() && !is_path_name_stop(s[q])) ++q;
            if (q > name_start) {
                out += s.substr(i, 9);
                out += L"<user>";
                i = q;
                continue;
            }
        }
        out += s[i];
        ++i;
    }
    return out;
}

std::wstring sanitize_text(const std::wstring& s) {
    // GUID and MAC first, so their output cannot be re-read as IPv4/IPv6.
    return mask_user_paths(mask_ipv6(mask_ipv4(mask_mac(mask_guid(s)))));
}

} // namespace netshutup::selftest::mask
