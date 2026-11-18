#pragma once

// dns_apply.h — the DNS stage of apply/revert, written against an abstract System so
// the whole flow (including every rollback path) is tested with a fake and never
// touches the machine. The real System is in dns_system.h.
// Design: docs/design/2026-11-18-legacy-dns-stub.md ("Apply akışı", "Revert")

#include "dns_backup.h"
#include "dns_config.h"
#include <optional>
#include <string>
#include <vector>

namespace netshutup::dns {

struct AdapterDns {
    std::wstring                             guid;
    unsigned long                            if_index = 0;
    std::optional<std::vector<std::wstring>> static_servers;    // nullopt = automatic (DHCP)
    std::vector<std::wstring>                effective_servers; // what the adapter uses now (static, else DHCP-provided); IPv4 only
};

class System {
public:
    virtual ~System() = default;

    virtual bool internet_ok() = 0;                        // basic reachability with the CURRENT DNS
    virtual std::vector<AdapterDns> adapters() = 0;        // physical adapters with their DNS
    virtual bool tls12_ok() = 0;                           // TLS 1.2 usable (Win7 needs KB3140245)
    virtual bool port53_free() = 0;                        // 127.0.0.1:53 (UDP and TCP) not in use
    virtual bool service_installed() = 0;                  // our DNS service already exists
    // First chain entry that really answers a query end to end; nullopt if none.
    virtual std::optional<Upstream> probe_chain(const std::vector<Upstream>& chain, std::wstring& err) = 0;
    // Writes dns.cfg, copies the exe, creates and starts the service (all or nothing).
    virtual bool install_service(const Config& cfg, std::wstring& err) = 0;
    virtual bool local_resolver_ok() = 0;                  // 127.0.0.1:53 answers a UDP and a TCP query
    virtual bool set_dns(const AdapterDns& a, const std::vector<std::wstring>& servers, std::wstring& err) = 0;
    virtual bool set_dns_automatic(const AdapterDns& a, std::wstring& err) = 0;
    virtual bool remove_service(std::wstring& err) = 0;    // stop + delete + remove files; true if already absent
    virtual bool write_text(const std::wstring& path, const std::string& bytes) = 0;
    virtual std::optional<std::string> read_text(const std::wstring& path) = 0;
};

enum class DnsOutcome { Skipped, Applied, Failed };

struct DnsResult {
    DnsOutcome              outcome = DnsOutcome::Skipped;
    std::wstring            message;               // short English text for the user
    std::optional<Upstream> upstream;              // set when Applied
    bool                    rolled_back = false;   // set when Failed and everything was undone
};

struct DnsRevertResult {
    bool         ok = true;
    bool         had_dns = false;   // a DNS side-file existed for this backup
    std::wstring message;
};

// Side-file path for a backup file: backup_path + L".dns".
std::wstring sidecar_path_for(const std::wstring& backup_path);

// Rules (in this order; each "Skipped" leaves the machine untouched):
//  1. chain = chain_for_profile(profile_name, fastest_first); empty -> Skipped
//     "unknown profile".
//  2. sys.service_installed() -> Skipped "DNS service is already installed; revert first".
//  3. !sys.internet_ok() -> Skipped "no internet connection; DNS left untouched".
//  4. adapters = sys.adapters(); empty -> Skipped "no physical network adapter found".
//  5. !sys.tls12_ok() -> Skipped "TLS 1.2 is not available (Windows 7 needs update
//     KB3140245); DNS left untouched".
//  6. !sys.port53_free() -> Skipped "port 53 on 127.0.0.1 is in use; DNS left untouched".
//  7. working = sys.probe_chain(chain, err); none -> Skipped "no encrypted DNS method works
//     on this network; original DNS kept" (append " (" + err + ")" when err is not empty).
//  8. Config cfg: chain rotated so `working` is first (order otherwise kept, wrapping);
//     legacy_dns = unique IPv4 servers from all adapters' effective_servers in first-seen
//     order; fail_threshold 3.
//  9. Backup side-file: DnsBackup from the adapters (guid, if_index, static_servers), written
//     with sys.write_text(sidecar_path_for(backup_path), serialize_backup(...)), then read back
//     with sys.read_text and parsed; it must equal what was written. Any failure -> Skipped
//     "could not write the DNS backup; DNS left untouched".
// 10. sys.install_service(cfg, err) fails -> remove_service (ignore its result) ->
//     Failed, rolled_back = true, message "could not install the DNS service: <err>".
// 11. !sys.local_resolver_ok() -> remove_service -> Failed, rolled_back = true,
//     message "the local DNS resolver did not answer; everything was rolled back".
// 12. For each adapter in order: sys.set_dns(a, {L"127.0.0.1"}, err). On failure restore every
//     adapter changed so far AND the failing one (static -> set_dns(original static list),
//     automatic -> set_dns_automatic), then remove_service -> Failed. rolled_back is true only
//     if every restore call and remove_service returned true; the message says so
//     ("... rolled back" / "... ROLLBACK INCOMPLETE: run netshutup-legacy revert").
// 13. After all adapters are switched: !sys.internet_ok() -> same full rollback as in 12 ->
//     Failed "internet stopped working after the DNS switch; everything was rolled back".
// 14. Success: Applied, upstream = working, message
//     "Encrypted DNS active: <provider id> over <doh|dot> via the local resolver (127.0.0.1)".
DnsResult apply_dns(const std::wstring& profile_name,
                    const std::wstring& backup_path,
                    System& sys,
                    const std::wstring& fastest_first = L"");

// Rules:
//  - sys.read_text(sidecar) empty (no file) -> ok = true, had_dns = false, message empty.
//  - parse fails -> ok = false, had_dns = true, message "DNS backup is corrupt".
//  - otherwise restore EVERY adapter (static -> set_dns(list); automatic -> set_dns_automatic),
//    continuing after failures, then sys.remove_service(); ok only if all calls returned true.
//    message: "DNS restored" or a list of what failed (never empty when !ok).
//  - Idempotent: running it twice is harmless (the side-file is left in place).
DnsRevertResult revert_dns(const std::wstring& backup_path, System& sys);

} // namespace netshutup::dns
