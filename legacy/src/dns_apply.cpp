// dns_apply.cpp — DNS stage of apply/revert, written against the abstract System.
// Rules: see dns_apply.h. Pure orchestration: every side effect goes through System.

#include "dns_apply.h"
#include <algorithm>
#include <utility>

namespace netshutup::dns {

namespace {

const wchar_t* const kLoopback = L"127.0.0.1";
const wchar_t* const kRollbackOk = L"; everything was rolled back";
const wchar_t* const kRollbackBad = L"; ROLLBACK INCOMPLETE: run netshutup-legacy revert";

DnsResult skipped(std::wstring message) {
    DnsResult r;
    r.outcome = DnsOutcome::Skipped;
    r.message = std::move(message);
    return r;
}

DnsResult failed(std::wstring message, bool rolled_back) {
    DnsResult r;
    r.outcome = DnsOutcome::Failed;
    r.message = std::move(message);
    r.rolled_back = rolled_back;
    return r;
}

// Restores every adapter in `list` (static -> its list, automatic -> DHCP), continuing after
// failures. Returns true only if every call succeeded.
bool restore_all(const std::vector<AdapterDns>& list, System& sys) {
    bool ok = true;
    for (const auto& a : list) {
        std::wstring err;
        bool r = a.static_servers ? sys.set_dns(a, *a.static_servers, err) : sys.set_dns_automatic(a, err);
        if (!r) ok = false;
    }
    return ok;
}

// Full rollback: restore `touched`, then remove the service. True only if all of it succeeded.
bool rollback(const std::vector<AdapterDns>& touched, System& sys) {
    bool ok = restore_all(touched, sys);
    std::wstring err;
    if (!sys.remove_service(err)) ok = false;
    return ok;
}

// After a COMPLETE rollback nothing changed on the machine, so the side-file must not
// make a later `revert` rewrite DNS settings. Replace it with an empty backup (no
// adapters). Best effort; never used when the rollback was incomplete (then the
// side-file is exactly what `revert` needs).
void neutralize_sidecar(System& sys, const std::wstring& side) {
    sys.write_text(side, serialize_backup(DnsBackup{}));
}

DnsBackup backup_of(const std::vector<AdapterDns>& adapters) {
    DnsBackup b;
    for (const auto& a : adapters) {
        AdapterBackup ab;
        ab.guid = a.guid;
        ab.if_index = a.if_index;
        ab.static_servers = a.static_servers;
        b.adapters.push_back(ab);
    }
    return b;
}

// Chain rotated so `working` comes first, the rest keeps its order (wrapping).
std::vector<Upstream> rotate_to(const std::vector<Upstream>& chain, const Upstream& working) {
    auto it = std::find(chain.begin(), chain.end(), working);
    if (it == chain.end()) return chain;
    std::vector<Upstream> out(it, chain.end());
    out.insert(out.end(), chain.begin(), it);
    return out;
}

} // namespace

std::wstring sidecar_path_for(const std::wstring& backup_path) {
    return backup_path + L".dns";
}

DnsResult apply_dns(const std::wstring& profile_name,
                    const std::wstring& backup_path,
                    System& sys,
                    const std::wstring& fastest_first) {
    // 1
    const std::vector<Upstream> chain = chain_for_profile(profile_name, fastest_first);
    if (chain.empty()) return skipped(L"unknown profile");

    // 2
    if (sys.service_installed())
        return skipped(L"DNS service is already installed; revert first");

    // 3
    if (!sys.internet_ok())
        return skipped(L"no internet connection; DNS left untouched");

    // 4
    const std::vector<AdapterDns> adapters = sys.adapters();
    if (adapters.empty())
        return skipped(L"no physical network adapter found");

    // 5
    if (!sys.tls12_ok())
        return skipped(L"TLS 1.2 is not available (Windows 7 needs update KB3140245); DNS left untouched");

    // 6
    if (!sys.port53_free())
        return skipped(L"port 53 on 127.0.0.1 is in use; DNS left untouched");

    // 7
    std::wstring err;
    const std::optional<Upstream> working = sys.probe_chain(chain, err);
    if (!working) {
        std::wstring msg = L"no encrypted DNS method works on this network; original DNS kept";
        if (!err.empty()) msg += L" (" + err + L")";
        return skipped(msg);
    }

    // 8
    Config cfg;
    cfg.chain = rotate_to(chain, *working);
    cfg.fail_threshold = 3;
    for (const auto& a : adapters) {
        for (const auto& s : a.effective_servers) {
            if (is_ipv4(s) && std::find(cfg.legacy_dns.begin(), cfg.legacy_dns.end(), s) == cfg.legacy_dns.end())
                cfg.legacy_dns.push_back(s);
        }
    }

    // 9
    const DnsBackup backup = backup_of(adapters);
    const std::wstring side = sidecar_path_for(backup_path);
    const std::string text = serialize_backup(backup);
    const wchar_t* const backup_fail = L"could not write the DNS backup; DNS left untouched";
    if (!sys.write_text(side, text)) return skipped(backup_fail);
    const std::optional<std::string> back = sys.read_text(side);
    if (!back) return skipped(backup_fail);
    const std::optional<DnsBackup> parsed = parse_backup(*back);
    if (!parsed || !(*parsed == backup)) {
        neutralize_sidecar(sys, side);
        return skipped(backup_fail);
    }

    // 10
    if (!sys.install_service(cfg, err)) {
        std::wstring ignored;
        sys.remove_service(ignored);
        neutralize_sidecar(sys, side);
        return failed(L"could not install the DNS service: " + err, true);
    }

    // 11
    if (!sys.local_resolver_ok()) {
        std::wstring ignored;
        sys.remove_service(ignored);
        neutralize_sidecar(sys, side);
        return failed(L"the local DNS resolver did not answer; everything was rolled back", true);
    }

    // 12
    std::vector<AdapterDns> touched;
    for (const auto& a : adapters) {
        touched.push_back(a);  // recorded before the call: a failing adapter is restored too
        std::wstring set_err;
        if (!sys.set_dns(a, std::vector<std::wstring>{kLoopback}, set_err)) {
            const bool ok = rollback(touched, sys);
            if (ok) neutralize_sidecar(sys, side);
            std::wstring msg = L"could not set DNS on adapter " + a.guid + L": " + set_err;
            msg += ok ? kRollbackOk : kRollbackBad;
            return failed(msg, ok);
        }
    }

    // 13
    if (!sys.internet_ok()) {
        const bool ok = rollback(touched, sys);
        if (ok) neutralize_sidecar(sys, side);
        std::wstring msg = L"internet stopped working after the DNS switch";
        msg += ok ? kRollbackOk : kRollbackBad;
        return failed(msg, ok);
    }

    // 14
    DnsResult r;
    r.outcome = DnsOutcome::Applied;
    r.upstream = *working;
    r.message = L"Encrypted DNS active: " + working->provider_id +
                (working->method == Method::DoH ? L" over doh" : L" over dot") +
                L" via the local resolver (127.0.0.1)";
    return r;
}

DnsRevertResult revert_dns(const std::wstring& backup_path, System& sys) {
    DnsRevertResult r;
    const std::optional<std::string> text = sys.read_text(sidecar_path_for(backup_path));
    if (!text) return r;  // no DNS side-file: nothing to restore

    r.had_dns = true;
    const std::optional<DnsBackup> parsed = parse_backup(*text);
    if (!parsed) {
        r.ok = false;
        r.message = L"DNS backup is corrupt";
        return r;
    }

    std::vector<std::wstring> failures;
    for (const auto& ab : parsed->adapters) {
        AdapterDns a;
        a.guid = ab.guid;
        a.if_index = ab.if_index;
        a.static_servers = ab.static_servers;
        std::wstring err;
        bool ok = ab.static_servers ? sys.set_dns(a, *ab.static_servers, err) : sys.set_dns_automatic(a, err);
        if (!ok) failures.push_back(L"adapter " + ab.guid + (err.empty() ? L"" : L": " + err));
    }
    std::wstring err;
    if (!sys.remove_service(err))
        failures.push_back(L"service removal" + (err.empty() ? L"" : L": " + err));

    if (failures.empty()) {
        r.ok = true;
        r.message = L"DNS restored";
        return r;
    }
    r.ok = false;
    r.message = L"could not restore DNS: ";
    for (size_t i = 0; i < failures.size(); ++i) {
        if (i) r.message += L"; ";
        r.message += failures[i];
    }
    return r;
}

} // namespace netshutup::dns
