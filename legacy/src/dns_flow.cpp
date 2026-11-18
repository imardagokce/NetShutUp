// dns_flow.cpp — real DNS stage after apply/revert (see dns_flow.h).

#include <windows.h>

#include "dns_flow.h"
#include "dns_apply.h"
#include "dns_service.h"
#include "dns_system.h"

#include <cstdio>
#include <cwchar>

namespace netshutup::dnsflow {

namespace {

// Newest "<something>.nsub.dns" in the folder of `backup_path` (or of any backup).
// Returns the matching backup path (without the ".dns" suffix) or empty.
std::wstring newest_sidecar_backup(const std::wstring& backup_path) {
    std::size_t slash = backup_path.find_last_of(L'\\');
    if (slash == std::wstring::npos) return L"";
    const std::wstring dir = backup_path.substr(0, slash);
    WIN32_FIND_DATAW ffd;
    HANDLE h = FindFirstFileW((dir + L"\\*.nsub.dns").c_str(), &ffd);
    if (h == INVALID_HANDLE_VALUE) return L"";
    FILETIME newest{0, 0};
    std::wstring name;
    do {
        if (CompareFileTime(&ffd.ftLastWriteTime, &newest) > 0) {
            newest = ffd.ftLastWriteTime;
            name = ffd.cFileName;
        }
    } while (FindNextFileW(h, &ffd));
    FindClose(h);
    if (name.size() <= 4) return L"";
    return dir + L"\\" + name.substr(0, name.size() - 4);   // strip ".dns"
}

} // namespace

void after_apply(const apply::ApplyResult& res, const wchar_t* profile) {
    if (!res.success || res.is_dry_run || res.backup_path.empty() || !profile) return;

    wprintf(L"Checking encrypted DNS (this can take a few seconds)...\n");
    dns::RealSystem sys;
    const std::wstring fastest = (std::wcscmp(profile, L"performance") == 0) ? sys.fastest_provider() : L"";
    dns::DnsResult r = dns::apply_dns(profile, res.backup_path, sys, fastest);

    switch (r.outcome) {
        case dns::DnsOutcome::Applied:
            wprintf(L"DNS: %ls\n", r.message.c_str());
            wprintf(L"     Undo with: netshutup-legacy revert\n");
            break;
        case dns::DnsOutcome::Skipped:
            wprintf(L"DNS: skipped - %ls\n", r.message.c_str());
            break;
        case dns::DnsOutcome::Failed:
            wprintf(L"DNS: FAILED - %ls\n", r.message.c_str());
            if (!r.rolled_back) wprintf(L"     Run: netshutup-legacy revert\n");
            break;
    }
    wprintf(L"\n");
}

void after_revert(const apply::RevertResult& res) {
    if (res.backup_path.empty()) return;

    dns::RealSystem sys;
    std::wstring target = res.backup_path;
    dns::DnsRevertResult r = dns::revert_dns(target, sys);

    if (!r.had_dns && sys.service_installed()) {
        // The reverted backup has no DNS part, but our service is still installed: it came
        // from an earlier apply. Undo that one so DNS can never get stuck.
        std::wstring older = newest_sidecar_backup(res.backup_path);
        if (!older.empty()) r = dns::revert_dns(older, sys);
    }

    if (!r.had_dns) return;   // nothing DNS-related to report
    if (r.ok) wprintf(L"DNS: restored to the original settings.\n\n");
    else      wprintf(L"DNS: revert INCOMPLETE - %ls\n     Run the revert again or restore the DNS servers manually.\n\n", r.message.c_str());
}

int print_status() {
    wprintf(L"DNS service: %ls\n", dns::service_state().c_str());
    auto st = dns::read_status_file(dns::status_path());
    if (st) wprintf(L"%ls\n", dns::describe_status(*st).c_str());
    else    wprintf(L"No status file yet (%ls).\n", dns::status_path().c_str());
    return 0;
}

} // namespace netshutup::dnsflow
