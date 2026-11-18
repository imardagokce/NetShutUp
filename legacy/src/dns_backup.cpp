// dns_backup.cpp — DNS side-file format and netsh argument builders (stage 7d).
// Pure: no I/O, no Windows calls, no process launching. Nothing here runs netsh.
#include "dns_backup.h"

#include <cstddef>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace netshutup::dns {

namespace {

const wchar_t kHeaderW[] = L"NETSHUTUP_DNS_BACKUP_V1";
const wchar_t kEndW[]    = L"END";
const wchar_t kAdapterW[] = L"ADAPTER";

std::wstring widen(const std::string& s) {
    std::wstring w;
    w.reserve(s.size());
    for (char c : s) {
        w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    }
    return w;
}

std::string narrow(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (wchar_t c : w) {
        s.push_back(static_cast<char>(c));
    }
    return s;
}

// Splits on every `sep`; empty fields are kept.
std::vector<std::wstring> split_on(const std::wstring& s, wchar_t sep) {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (wchar_t c : s) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

// Lines of a text: LF-terminated; a trailing LF does not create an extra empty line.
std::vector<std::wstring> split_lines(const std::wstring& text) {
    std::vector<std::wstring> lines = split_on(text, L'\n');
    if (!lines.empty() && lines.back().empty()) {
        lines.pop_back();
    }
    return lines;
}

bool is_hex(wchar_t c) {
    return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
}

// {8-4-4-4-12 hex}
bool is_valid_guid(const std::wstring& g) {
    if (g.size() != 38 || g.front() != L'{' || g.back() != L'}') {
        return false;
    }
    const std::size_t groups[5] = {8, 4, 4, 4, 12};
    std::size_t pos = 1;
    for (std::size_t gi = 0; gi < 5; ++gi) {
        for (std::size_t k = 0; k < groups[gi]; ++k) {
            if (!is_hex(g[pos++])) {
                return false;
            }
        }
        if (gi < 4) {
            if (g[pos++] != L'-') {
                return false;
            }
        }
    }
    return pos == 37;
}

// Upper-cased copy, for case-insensitive GUID comparison.
std::wstring guid_key(const std::wstring& g) {
    std::wstring k = g;
    for (wchar_t& c : k) {
        if (c >= L'a' && c <= L'z') {
            c = static_cast<wchar_t>(c - L'a' + L'A');
        }
    }
    return k;
}

// Positive decimal number that fits in 32 bits.
bool parse_if_index(const std::wstring& s, unsigned long& out) {
    if (s.empty()) {
        return false;
    }
    unsigned long long v = 0;
    for (wchar_t c : s) {
        if (c < L'0' || c > L'9') {
            return false;
        }
        v = v * 10 + static_cast<unsigned long long>(c - L'0');
        if (v > 0xFFFFFFFFULL) {
            return false;
        }
    }
    if (v == 0) {
        return false;
    }
    out = static_cast<unsigned long>(v);
    return true;
}

// "ADAPTER {GUID} <idx> dhcp" or "ADAPTER {GUID} <idx> static <ip>,<ip>,..."
bool parse_adapter_line(const std::wstring& line, AdapterBackup& out) {
    const std::vector<std::wstring> tok = split_on(line, L' ');
    if (tok.size() < 4 || tok[0] != kAdapterW) {
        return false;
    }
    if (!is_valid_guid(tok[1])) {
        return false;
    }
    unsigned long idx = 0;
    if (!parse_if_index(tok[2], idx)) {
        return false;
    }

    AdapterBackup a;
    a.guid = tok[1];
    a.if_index = idx;

    if (tok[3] == L"dhcp") {
        if (tok.size() != 4) {
            return false;
        }
        out = std::move(a);
        return true;
    }
    if (tok[3] != L"static" || tok.size() != 5) {
        return false;
    }
    std::vector<std::wstring> servers = split_on(tok[4], L',');
    for (const auto& s : servers) {
        if (!is_ipv4(s)) {
            return false;
        }
    }
    a.static_servers = std::move(servers);
    out = std::move(a);
    return true;
}

} // namespace

std::string serialize_backup(const DnsBackup& b) {
    std::string out = "NETSHUTUP_DNS_BACKUP_V1\n";
    for (const auto& a : b.adapters) {
        out += "ADAPTER ";
        out += narrow(a.guid);
        out += ' ';
        out += std::to_string(a.if_index);
        if (a.static_servers) {
            out += " static";
            const auto& servers = *a.static_servers;
            for (std::size_t i = 0; i < servers.size(); ++i) {
                out += (i == 0) ? ' ' : ',';
                out += narrow(servers[i]);
            }
        } else {
            out += " dhcp";
        }
        out += '\n';
    }
    out += "END\n";
    return out;
}

std::optional<DnsBackup> parse_backup(const std::string& text) {
    std::vector<std::wstring> lines = split_lines(widen(text));
    for (auto& l : lines) {
        if (!l.empty() && l.back() == L'\r') {
            l.pop_back();
        }
    }
    if (lines.empty() || lines[0] != kHeaderW) {
        return std::nullopt;
    }

    DnsBackup out;
    std::set<std::wstring> seen;
    bool ended = false;
    for (std::size_t i = 1; i < lines.size(); ++i) {
        if (ended) {
            return std::nullopt;  // content after END
        }
        if (lines[i] == kEndW) {
            ended = true;
            continue;
        }
        AdapterBackup a;
        if (!parse_adapter_line(lines[i], a)) {
            return std::nullopt;
        }
        if (!seen.insert(guid_key(a.guid)).second) {
            return std::nullopt;  // duplicate GUID
        }
        out.adapters.push_back(std::move(a));
    }
    if (!ended) {
        return std::nullopt;
    }
    return out;
}

std::wstring netsh_set_static_args(unsigned long if_index, const std::wstring& server) {
    if (if_index == 0 || !is_ipv4(server)) {
        return {};
    }
    return std::wstring(L"interface ipv4 set dnsservers name=") + std::to_wstring(if_index) +
           L" source=static address=" + server + L" register=primary";
}

std::wstring netsh_add_args(unsigned long if_index, const std::wstring& server, unsigned index) {
    if (if_index == 0 || index < 2 || !is_ipv4(server)) {
        return {};
    }
    return std::wstring(L"interface ipv4 add dnsservers name=") + std::to_wstring(if_index) +
           L" address=" + server + L" index=" + std::to_wstring(index);
}

std::wstring netsh_set_dhcp_args(unsigned long if_index) {
    if (if_index == 0) {
        return {};
    }
    return std::wstring(L"interface ipv4 set dnsservers name=") + std::to_wstring(if_index) +
           L" source=dhcp";
}

std::vector<std::wstring> netsh_restore_commands(unsigned long if_index,
                                                 const std::vector<std::wstring>& servers) {
    if (if_index == 0 || servers.empty()) {
        return {};
    }
    for (const auto& s : servers) {
        if (!is_ipv4(s)) {
            return {};
        }
    }
    std::vector<std::wstring> cmds;
    cmds.push_back(netsh_set_static_args(if_index, servers[0]));
    for (std::size_t i = 1; i < servers.size(); ++i) {
        cmds.push_back(netsh_add_args(if_index, servers[i], static_cast<unsigned>(i + 1)));
    }
    return cmds;
}

bool is_ipv4(const std::wstring& s) {
    const std::size_t n = s.size();
    std::size_t i = 0;
    for (int octet = 0; octet < 4; ++octet) {
        std::size_t digits = 0;
        unsigned value = 0;
        while (i < n && s[i] >= L'0' && s[i] <= L'9') {
            value = value * 10 + static_cast<unsigned>(s[i] - L'0');
            ++digits;
            ++i;
            if (digits > 3) {
                return false;
            }
        }
        if (digits == 0 || value > 255) {
            return false;
        }
        if (octet < 3) {
            if (i >= n || s[i] != L'.') {
                return false;
            }
            ++i;
        }
    }
    return i == n;
}

} // namespace netshutup::dns
