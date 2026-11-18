// selftest.cpp — real wiring for tester mode: safety net, environment facts,
// report files, and the `selftest` command.

#include "selftest.h"
#include "adapters.h"
#include "catalog.h"
#include "elevation.h"
#include "preview.h"
#include "registry.h"
#include "store.h"
#include "dns_apply.h"
#include "dns_system.h"
#include "selftest_mask.h"

#include <shellapi.h>
#include <shlobj.h>
#include <srrestoreptapi.h>

#include <cstdio>
#include <cwchar>
#include <set>

namespace netshutup::selftest {

namespace {

// ── Small helpers ────────────────────────────────────────────────────────────

std::wstring known_folder(int csidl) {
    wchar_t buf[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, csidl, nullptr, SHGFP_TYPE_CURRENT, buf))) return buf;
    return L"";
}

std::wstring timestamp_utc(bool compact) {
    SYSTEMTIME st;
    GetSystemTime(&st);
    wchar_t buf[40];
    if (compact) {
        swprintf(buf, 40, L"%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay,
                 st.wHour, st.wMinute, st.wSecond);
    } else {
        swprintf(buf, 40, L"%04u-%02u-%02uT%02u:%02u:%02uZ", st.wYear, st.wMonth, st.wDay,
                 st.wHour, st.wMinute, st.wSecond);
    }
    return buf;
}

bool ensure_dir(const std::wstring& dir) {
    // Creates each missing component; ok if it already exists.
    for (std::size_t i = 3; i <= dir.size(); ++i) {
        if (i == dir.size() || dir[i] == L'\\') {
            std::wstring part = dir.substr(0, i);
            if (!CreateDirectoryW(part.c_str(), nullptr) &&
                GetLastError() != ERROR_ALREADY_EXISTS) return false;
        }
    }
    return true;
}

std::string to_utf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

bool write_file(const std::wstring& path, const std::string& bytes) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)
              && written == bytes.size();
    CloseHandle(h);
    return ok;
}

bool is_arg(int argc, wchar_t* argv[], const wchar_t* flag) {
    for (int i = 1; i < argc; ++i) if (_wcsicmp(argv[i], flag) == 0) return true;
    return false;
}

// ── Real Env ─────────────────────────────────────────────────────────────────

struct RealCtx {
    DWORD os_mask;
    std::vector<adapters::PhysicalAdapter> nics;
    std::wstring dns_note;   // DNS stage result of the last apply/revert (see Env::take_note)
};

// 32-bit FNV-1a of a string: DNS server lists are compared across apply/revert as one value.
DWORD fnv1a(const std::wstring& s) {
    DWORD h = 2166136261u;
    for (wchar_t ch : s) {
        h ^= static_cast<DWORD>(ch);
        h *= 16777619u;
    }
    return h;
}

Snapshot real_snapshot(void* c) {
    auto* ctx = static_cast<RealCtx*>(c);
    Snapshot snap;
    const catalog::Entry* e = catalog::entries();
    for (std::size_t i = 0; i < catalog::entry_count(); ++i) {
        if (e[i].scope == catalog::Scope::GLOBAL || e[i].scope == catalog::Scope::FIREWALL) {
            snap.emplace_back(e[i].id, store::read(e[i]));
        } else {
            for (const auto& nic : ctx->nics) {
                std::wstring sub = adapters::format_subkey_for_adapter(e[i].subkey, nic.guid);
                snap.emplace_back(std::wstring(e[i].id) + L"@" + nic.guid,
                                  registry::read_dword(e[i].hive, sub.c_str(), e[i].value_name));
            }
        }
    }
    // DNS state: our service and every physical adapter's static DNS list (empty = automatic
    // = "not set"). Lists are normalised ("a,b") so formatting differences do not look like changes.
    dns::RealSystem sys;
    snap.emplace_back(L"dns.service", sys.service_installed() ? std::optional<DWORD>(1) : std::nullopt);
    for (const auto& a : sys.adapters()) {
        std::optional<DWORD> v;
        if (a.static_servers && !a.static_servers->empty()) {
            std::wstring joined;
            for (const auto& s : *a.static_servers) joined += (joined.empty() ? L"" : L",") + s;
            v = fnv1a(joined);
        }
        snap.emplace_back(L"dns.adapter." + a.guid, v);
    }
    return snap;
}

apply::ApplyResult real_apply(void* c, const wchar_t* profile) {
    auto* ctx = static_cast<RealCtx*>(c);
    ctx->dns_note.clear();
    apply::ApplyResult r = apply::apply_profile(profile, ctx->os_mask, false);
    if (r.success && !r.backup_path.empty()) {
        // The same DNS stage as the real `apply` command (dns_flow), recorded for the report.
        dns::RealSystem sys;
        const std::wstring fastest = (std::wcscmp(profile, L"performance") == 0) ? sys.fastest_provider() : L"";
        dns::DnsResult d = dns::apply_dns(profile, r.backup_path, sys, fastest);
        switch (d.outcome) {
            case dns::DnsOutcome::Applied: ctx->dns_note = d.message; break;
            case dns::DnsOutcome::Skipped: ctx->dns_note = L"SKIP: " + d.message; break;
            case dns::DnsOutcome::Failed:  ctx->dns_note = L"FAIL: " + d.message; break;
        }
    }
    return r;
}

apply::RevertResult real_revert(void* c, const std::wstring& path) {
    auto* ctx = static_cast<RealCtx*>(c);
    ctx->dns_note.clear();
    apply::RevertResult r = apply::revert(path);
    dns::RealSystem sys;
    dns::DnsRevertResult d = dns::revert_dns(path, sys);
    if (d.had_dns) {
        if (d.ok) {
            ctx->dns_note = L"DNS restored";
        } else {
            ctx->dns_note = L"FAIL: " + d.message;
            r.success = false;                        // makes the runner stop and print the manual command
            r.error_message += L" DNS: " + d.message;
        }
    }
    return r;
}

std::wstring real_take_note(void* c) {
    auto* ctx = static_cast<RealCtx*>(c);
    std::wstring n = std::move(ctx->dns_note);
    ctx->dns_note.clear();
    return n;
}

unsigned long real_now(void*) { return static_cast<unsigned long>(GetTickCount64()); }

// ── Safety net helpers ───────────────────────────────────────────────────────

bool run_reg_export(const std::wstring& key, const std::wstring& file) {
    wchar_t sysdir[MAX_PATH] = {};
    GetSystemDirectoryW(sysdir, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(sysdir) + L"\\reg.exe\" export \"" + key +
                       L"\" \"" + file + L"\" /y";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::wstring mutable_cmd = cmd;
    if (!CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    DWORD w = WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 1;
    if (w == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return w == WAIT_OBJECT_0 && code == 0;   // exit code only; output is localized
}

std::wstring hive_name(HKEY h) {
    return h == HKEY_LOCAL_MACHINE ? L"HKLM" : L"HKCU";
}

// Distinct registry roots touched by the catalog (adapter placeholder stripped).
std::vector<std::pair<HKEY, std::wstring>> catalog_roots() {
    std::set<std::pair<HKEY, std::wstring>> roots;
    const catalog::Entry* e = catalog::entries();
    for (std::size_t i = 0; i < catalog::entry_count(); ++i) {
        if (e[i].scope == catalog::Scope::FIREWALL) continue;   // exported with netsh below
        std::wstring sub = e[i].subkey;
        const std::wstring tag = L"\\{ADAPTER-GUID}";
        auto pos = sub.find(tag);
        if (pos != std::wstring::npos) sub.erase(pos);
        roots.emplace(e[i].hive, sub);
    }
    return {roots.begin(), roots.end()};
}

// ── Environment section (facts only, no identifiers) ─────────────────────────

Section environment_section(const OsVersion& os, const std::wstring& safety_dir) {
    Section s;
    s.name = L"Environment";
    auto info = [&](const wchar_t* id, const wchar_t* title, std::wstring d) {
        Check c; c.id = id; c.title = title; c.status = Status::Info; c.detail = std::move(d);
        s.checks.push_back(std::move(c));
    };
    info(L"os", L"Windows", std::to_wstring(os.major) + L"." + std::to_wstring(os.minor) +
                            L" build " + std::to_wstring(os.build));
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    info(L"arch", L"Architecture",
         si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? L"x64" :
         si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL ? L"x86" : L"other");
    wchar_t loc[LOCALE_NAME_MAX_LENGTH] = {};
    GetUserDefaultLocaleName(loc, LOCALE_NAME_MAX_LENGTH);
    info(L"locale", L"Locale", loc);
    info(L"elevated", L"Administrator", elevation::is_elevated() ? L"yes" : L"no");
    {
        dns::RealSystem dsys;
        info(L"dns_tls12", L"TLS 1.2 for encrypted DNS", dsys.tls12_ok() ? L"available" : L"NOT available (Windows 7 needs KB3140245)");
        info(L"dns_port53", L"Local DNS port 53", dsys.port53_free() ? L"free" : L"in use");
        info(L"dns_service", L"DNS service", dns::service_state());
    }

    auto nics = adapters::get_physical_adapters();
    std::size_t eth = 0, wifi = 0;
    for (auto& n : nics) {
        if (n.type == adapters::AdapterType::ETHERNET) ++eth;
        else if (n.type == adapters::AdapterType::WIFI) ++wifi;
    }
    info(L"nics", L"Physical adapters",
         std::to_wstring(eth) + L" ethernet, " + std::to_wstring(wifi) + L" wi-fi");

    Check w;
    w.id = L"safety_dir";
    w.title = L"Safety folder writable";
    w.status = ensure_dir(safety_dir) ? Status::Pass : Status::Fail;
    s.checks.push_back(std::move(w));
    return s;
}

Section readonly_section(DWORD os_mask, const std::vector<std::wstring>& profiles) {
    Section s;
    s.name = L"Read-only checks";
    for (const auto& p : profiles) {
        Check c;
        c.id = L"preview." + p;
        c.title = L"Preview " + p;
        c.status = preview::generate(p.c_str(), os_mask).has_value() ? Status::Pass : Status::Fail;
        s.checks.push_back(c);
        Check d;
        d.id = L"dryrun." + p;
        d.title = L"Dry-run apply " + p;
        auto r = apply::apply_profile(p.c_str(), os_mask, true);
        d.status = r.success ? Status::Pass : Status::Fail;
        d.detail = r.error_message;
        s.checks.push_back(d);
    }
    return s;
}

bool confirm_cli() {
    wprintf(L"\nNetShutUp Legacy self-test\n"
            L"  - Applies and reverts every profile one by one (takes a minute or two).\n"
            L"  - Network may drop briefly; everything is reverted; backups are kept.\n"
            L"  - Collects hardware/network facts WITHOUT personal identifiers into a\n"
            L"    local report file. Nothing is sent anywhere.\n"
            L"Run the self-test now? [y/N]: ");
    wchar_t line[16] = {};
    if (!fgetws(line, 16, stdin)) return false;
    return line[0] == L'y' || line[0] == L'Y';
}

void pause_if_requested(bool pause) {
    if (!pause) return;
    wprintf(L"\nPress Enter to close...");
    wchar_t line[8];
    fgetws(line, 8, stdin);
}

} // namespace

// ── Safety net ───────────────────────────────────────────────────────────────

SafetyNet create_safety_net() {
    SafetyNet sn;
    const std::wstring dir = known_folder(CSIDL_COMMON_APPDATA) + L"\\NetShutUp\\safety\\" +
                             timestamp_utc(true);
    if (!ensure_dir(dir)) { sn.error = L"cannot create safety folder"; return sn; }
    sn.reg_export_path = dir;

    int n = 0;
    for (const auto& [hive, sub] : catalog_roots()) {
        if (!registry::key_exists(hive, sub.c_str())) continue;   // absent == "not set"; compare step covers it
        std::wstring file = dir + L"\\key" + std::to_wstring(++n) + L".reg";
        if (!run_reg_export(hive_name(hive) + L"\\" + sub, file)) {
            sn.error = L"reg export failed for " + hive_name(hive) + L"\\" + sub;
            return sn;
        }
    }
    // Windows Firewall policy (not in the registry export): netsh, exit code only.
    {
        std::wstring file = dir + L"\\firewall.wfw";
        wchar_t sysdir[MAX_PATH] = {};
        GetSystemDirectoryW(sysdir, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(sysdir) + L"\\netsh.exe\" advfirewall export \"" + file + L"\"";
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        bool exported = false;
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                           nullptr, nullptr, &si, &pi)) {
            DWORD code = 1;
            if (WaitForSingleObject(pi.hProcess, 30000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            exported = (code == 0);
        }
        if (!exported) { sn.error = L"netsh advfirewall export failed"; return sn; }
    }
    // Adapter DNS before the test, as plain text for manual recovery (reg + netsh exports do not
    // cover it in a readable form). Best effort: not having it never blocks the test.
    {
        std::wstring text = L"Adapter DNS before the self-test (interface index, GUID, servers):\r\n";
        dns::RealSystem dsys;
        for (const auto& a : dsys.adapters()) {
            text += std::to_wstring(a.if_index) + L"  " + a.guid + L"  ";
            if (a.static_servers) {
                text += L"static ";
                for (std::size_t i = 0; i < a.static_servers->size(); ++i) text += (i ? L"," : L"") + (*a.static_servers)[i];
            } else {
                text += L"automatic (DHCP)";
            }
            text += L"\r\n";
        }
        write_file(dir + L"\\dns-before.txt", to_utf8(text));
    }
    sn.reg_export_ok = true;

    // System restore point (optional; may be disabled or throttled).
    HMODULE lib = LoadLibraryW(L"srclient.dll");
    if (!lib) { sn.restore_point_note = L"System Restore not available"; return sn; }
    using SetFn = BOOL(WINAPI*)(PRESTOREPOINTINFOW, PSTATEMGRSTATUS);
    auto set_rp = reinterpret_cast<SetFn>(reinterpret_cast<void*>(GetProcAddress(lib, "SRSetRestorePointW")));
    if (set_rp) {
        RESTOREPOINTINFOW rp{};
        STATEMGRSTATUS st{};
        rp.dwEventType = BEGIN_SYSTEM_CHANGE;
        rp.dwRestorePtType = MODIFY_SETTINGS;
        wcscpy_s(rp.szDescription, L"NetShutUp self-test");
        if (set_rp(&rp, &st) && st.nStatus == ERROR_SUCCESS) {
            sn.restore_point_ok = true;
            rp.dwEventType = END_SYSTEM_CHANGE;
            rp.llSequenceNumber = st.llSequenceNumber;
            set_rp(&rp, &st);
        } else {
            sn.restore_point_note = L"restore point not created (System Restore off or throttled)";
        }
    } else {
        sn.restore_point_note = L"System Restore API missing";
    }
    FreeLibrary(lib);
    return sn;
}

// ── Command ──────────────────────────────────────────────────────────────────

int cmd_selftest(const OsVersion& os, int argc, wchar_t* argv[]) {
    const bool yes   = is_arg(argc, argv, L"--yes");
    const bool pause = is_arg(argc, argv, L"--pause");

    if (!yes && !confirm_cli()) {
        wprintf(L"Self-test cancelled. Nothing was changed.\n");
        return 0;
    }

    if (!elevation::is_elevated()) {
        wprintf(L"Elevating to administrator rights...\n");
        std::vector<wchar_t*> args(argv, argv + argc);
        wchar_t y[] = L"--yes";
        wchar_t p[] = L"--pause";
        if (!yes) args.push_back(y);
        if (!pause) args.push_back(p);
        return elevation::restart_elevated(static_cast<int>(args.size()), args.data());
    }

    const std::vector<std::wstring> profiles{L"privacy", L"performance", L"recommended"};
    const std::wstring safety_root = known_folder(CSIDL_COMMON_APPDATA) + L"\\NetShutUp\\safety";

    Report rep;
    rep.started_utc = timestamp_utc(false);

    wprintf(L"Creating safety net (registry export + restore point)...\n");
    Section env_sec = environment_section(os, safety_root);
    SafetyNet sn = create_safety_net();
    {
        Check c;
        c.id = L"reg_export";
        c.title = L"Registry export (safety net)";
        c.status = sn.reg_export_ok ? Status::Pass : Status::Fail;
        c.detail = sn.error;
        env_sec.checks.push_back(c);
        Check r;
        r.id = L"restore_point";
        r.title = L"System restore point";
        r.status = sn.restore_point_ok ? Status::Pass : Status::Skip;
        r.detail = sn.restore_point_note;
        env_sec.checks.push_back(r);
    }
    rep.sections.push_back(env_sec);

    int exit_code = 0;
    if (!sn.reg_export_ok) {
        wprintf(L"Safety net failed (%ls). Nothing was changed.\n", sn.error.c_str());
        rep.all_restored = true;   // nothing was written
        exit_code = 1;
    } else {
        rep.sections.push_back(readonly_section(os.os_mask(), profiles));
        wprintf(L"Running profiles (apply -> verify -> revert -> compare)...\n");

        RealCtx ctx{os.os_mask(), adapters::get_physical_adapters()};
        Env env{real_snapshot, real_apply, real_revert, real_now, &ctx, real_take_note};
        Report loop = run(env, profiles);
        rep.total_millis = loop.total_millis;
        rep.all_restored = loop.all_restored;
        rep.manual_revert_cmd = loop.manual_revert_cmd;
        for (auto& s : loop.sections) rep.sections.push_back(std::move(s));
        exit_code = rep.all_restored ? 0 : 1;
        for (auto& s : rep.sections)
            for (auto& c : s.checks)
                if (c.status == Status::Fail) exit_code = 1;
    }

    // Write report: ProgramData\NetShutUp\reports + desktop copy.
    const std::wstring base = L"NetShutUp-selftest-" + timestamp_utc(true);
    const std::string md   = to_utf8(render_markdown(rep));
    const std::string json = render_json(rep);
    const std::wstring dir = known_folder(CSIDL_COMMON_APPDATA) + L"\\NetShutUp\\reports";
    if (ensure_dir(dir) && write_file(dir + L"\\" + base + L".md", md) &&
        write_file(dir + L"\\" + base + L".json", json)) {
        wprintf(L"\nReport written to: %ls\\%ls.md (+ .json)\n", dir.c_str(), base.c_str());
        std::wstring desk = known_folder(CSIDL_DESKTOPDIRECTORY);
        if (!desk.empty() && write_file(desk + L"\\" + base + L".md", md) &&
            write_file(desk + L"\\" + base + L".json", json)) {
            wprintf(L"Copy on your Desktop.\n");
        }
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else {
        wprintf(L"\nERROR: could not write the report to %ls\n", dir.c_str());
        exit_code = 1;
    }

    if (!rep.all_restored) {
        wprintf(L"\n!! NOT everything was restored. Run: %ls\n", rep.manual_revert_cmd.c_str());
    } else {
        wprintf(L"\nAll settings restored: yes. Please send the .md/.json report to the developer.\n");
    }
    pause_if_requested(pause);
    return exit_code;
}

} // namespace netshutup::selftest
