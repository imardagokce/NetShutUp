#pragma once

// dns_backup.h — the DNS side-file of a backup (<backup>.nsub.dns) and the netsh
// argument builders used to change/restore adapter DNS. Pure: no I/O, no process
// launching. Design: docs/design/2026-11-18-legacy-dns-stub.md

#include <optional>
#include <string>
#include <vector>

namespace netshutup::dns {

struct AdapterBackup {
    std::wstring                             guid;          // e.g. {B92C2366-D0BB-4241-8664-9D97B8964720}
    unsigned long                            if_index = 0;  // interface index (used as netsh name=)
    std::optional<std::vector<std::wstring>> static_servers; // nullopt = DNS came from DHCP ("not set")
    bool operator==(const AdapterBackup& o) const {
        return guid == o.guid && if_index == o.if_index && static_servers == o.static_servers;
    }
};

struct DnsBackup {
    std::vector<AdapterBackup> adapters;
    bool operator==(const DnsBackup& o) const { return adapters == o.adapters; }
};

// Text format (UTF-8 narrowed per character, LF line endings):
//   NETSHUTUP_DNS_BACKUP_V1
//   ADAPTER {GUID} 5 static 192.168.1.1,8.8.8.8      (static servers, comma separated, no spaces)
//   ADAPTER {GUID} 7 dhcp                            (automatic DNS)
//   END
std::string serialize_backup(const DnsBackup& b);

// Strict parser (CRLF accepted). nullopt on: wrong/missing header, missing END, trailing
// content after END, unknown keyword, GUID not of the form {8-4-4-4-12 hex}, if_index not a
// positive decimal number that fits in 32 bits, mode other than static/dhcp, "static" with an
// empty or malformed list, any server that is not a dotted-quad IPv4 (four decimal octets
// 0-255), "dhcp" followed by extra tokens, duplicate GUID. Zero ADAPTER lines is valid.
std::optional<DnsBackup> parse_backup(const std::string& text);

// netsh argument builders (arguments only, no "netsh.exe"). Each returns an empty
// string if `server` is not a dotted-quad IPv4 or if_index == 0. Exact outputs:
//   set static:  interface ipv4 set dnsservers name=<idx> source=static address=<ip> register=primary
//   add:         interface ipv4 add dnsservers name=<idx> address=<ip> index=<n>        (n >= 2)
//   set dhcp:    interface ipv4 set dnsservers name=<idx> source=dhcp
std::wstring netsh_set_static_args(unsigned long if_index, const std::wstring& server);
std::wstring netsh_add_args(unsigned long if_index, const std::wstring& server, unsigned index);
std::wstring netsh_set_dhcp_args(unsigned long if_index);

// Commands (in order) that make the adapter use exactly `servers`: one "set static" for the
// first server, then one "add" per further server with index 2, 3, ... Empty vector if
// `servers` is empty or any server is invalid.
std::vector<std::wstring> netsh_restore_commands(unsigned long if_index,
                                                 const std::vector<std::wstring>& servers);

// True for a dotted-quad IPv4 (four decimal octets 0-255, no leading/trailing junk).
bool is_ipv4(const std::wstring& s);

} // namespace netshutup::dns
