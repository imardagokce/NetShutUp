// dns_system.cpp — real implementation of dns::System (see dns_system.h).
// NOTE: everything here changes the machine (service, adapter DNS, files). It is
// only reached from the real apply/revert commands, never from tests.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winsvc.h>

#include "dns_system.h"
#include "adapters.h"
#include "dns_backup.h"
#include "dns_frame.h"
#include "dns_plain.h"
#include "dns_real_transport.h"
#include "dns_service.h"
#include "registry.h"

#include <algorithm>
#include <chrono>

namespace netshutup::dns {

namespace {

constexpr wchar_t kExeName[] = L"netshutup-legacy.exe";

std::wstring bin_dir() { return data_dir() + L"\\bin"; }
std::wstring bin_exe() { return bin_dir() + L"\\" + kExeName; }

bool ensure_dir(const std::wstring& dir) {
    for (std::size_t i = 3; i <= dir.size(); ++i) {
        if (i == dir.size() || dir[i] == L'\\') {
            std::wstring part = dir.substr(0, i);
            if (!CreateDirectoryW(part.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
        }
    }
    return true;
}

std::wstring parent_of(const std::wstring& path) {
    std::size_t p = path.find_last_of(L'\\');
    return p == std::wstring::npos ? L"" : path.substr(0, p);
}

// Runs a system tool (System32) and returns true only if it exits with code 0.
// Output is never parsed (it is localized).
bool run_tool(const wchar_t* exe, const std::wstring& args, DWORD timeout_ms = 30000) {
    wchar_t sysdir[MAX_PATH] = {};
    GetSystemDirectoryW(sysdir, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(sysdir) + L"\\" + exe + L"\" " + args;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return false;
    }
    DWORD w = WaitForSingleObject(pi.hProcess, timeout_ms);
    DWORD code = 1;
    if (w == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    else TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return w == WAIT_OBJECT_0 && code == 0;
}

void flush_dns_cache() {
    HMODULE lib = LoadLibraryW(L"dnsapi.dll");
    if (!lib) return;
    using Fn = BOOL(WINAPI*)();
    auto fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(lib, "DnsFlushResolverCache")));
    if (fn) fn();
    FreeLibrary(lib);
}

std::vector<std::wstring> split_servers(const std::wstring& s) {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (wchar_t c : s) {
        if (c == L',' || c == L' ' || c == L';') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

packet::Bytes example_query(std::uint16_t id) {
    packet::Bytes q = {static_cast<unsigned char>(id >> 8), static_cast<unsigned char>(id & 0xFF),
                       0x01, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0,
                       7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 3, 'c', 'o', 'm', 0, 0x00, 0x01, 0x00, 0x01};
    return q;
}

// A usable answer: header present, ID matches, QR set, NOERROR, at least one answer.
bool good_answer(const packet::Bytes& r, std::uint16_t id) {
    return r.size() >= 12 && r[0] == static_cast<unsigned char>(id >> 8) &&
           r[1] == static_cast<unsigned char>(id & 0xFF) && (r[2] & 0x80) != 0 &&
           (r[3] & 0x0F) == 0 && (r[6] != 0 || r[7] != 0);
}

// One TCP query to 127.0.0.1:53 (2-byte framing).
bool tcp_query_loopback(const packet::Bytes& q, std::uint16_t id) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    DWORD t = 3000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    InetPtonW(AF_INET, L"127.0.0.1", &a.sin_addr);
    bool ok = false;
    if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) {
        packet::Bytes framed = packet::frame_tcp(q);
        if (send(s, reinterpret_cast<const char*>(framed.data()), static_cast<int>(framed.size()), 0) ==
            static_cast<int>(framed.size())) {
            FrameAssembler fa;
            unsigned char buf[2048];
            for (int i = 0; i < 20 && !ok; ++i) {
                int n = recv(s, reinterpret_cast<char*>(buf), static_cast<int>(sizeof(buf)), 0);
                if (n <= 0) break;
                fa.push(buf, static_cast<std::size_t>(n));
                if (auto m = fa.next()) ok = good_answer(*m, id);
            }
        }
    }
    closesocket(s);
    return ok;
}

bool try_bind_loopback53(int type) {
    SOCKET s = socket(AF_INET, type, type == SOCK_DGRAM ? IPPROTO_UDP : IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    BOOL excl = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof(excl));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    InetPtonW(AF_INET, L"127.0.0.1", &a.sin_addr);
    bool ok = bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0;
    closesocket(s);
    return ok;
}

struct ScHandle {
    SC_HANDLE h = nullptr;
    explicit ScHandle(SC_HANDLE x = nullptr) : h(x) {}
    ~ScHandle() { if (h) CloseServiceHandle(h); }
    ScHandle(const ScHandle&) = delete;
    ScHandle& operator=(const ScHandle&) = delete;
};

bool wait_for_state(SC_HANDLE svc, DWORD wanted, DWORD timeout_ms) {
    const DWORD start = GetTickCount();
    for (;;) {
        SERVICE_STATUS st{};
        if (!QueryServiceStatus(svc, &st)) return false;
        if (st.dwCurrentState == wanted) return true;
        if (wanted == SERVICE_RUNNING && st.dwCurrentState == SERVICE_STOPPED) return false;
        if (GetTickCount() - start > timeout_ms) return false;
        Sleep(250);
    }
}

bool delete_file_retry(const std::wstring& path) {
    for (int i = 0; i < 20; ++i) {
        if (DeleteFileW(path.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND) return true;
        Sleep(250);
    }
    return false;
}

} // namespace

// ── RealSystem ────────────────────────────────────────────────────────────────

RealSystem::RealSystem() {
    WSADATA wsa;
    wsa_ok_ = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
}

RealSystem::~RealSystem() {
    if (wsa_ok_) WSACleanup();
}

bool RealSystem::internet_ok() {
    if (!wsa_ok_) return false;
    flush_dns_cache();   // make sure the answer really comes from the current resolver
    for (int attempt = 0; attempt < 3; ++attempt) {
        ADDRINFOW hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        ADDRINFOW* res = nullptr;
        if (GetAddrInfoW(L"example.com", nullptr, &hints, &res) == 0) {
            FreeAddrInfoW(res);
            return true;
        }
        Sleep(700);
    }
    return false;
}

std::vector<AdapterDns> RealSystem::adapters() {
    std::vector<AdapterDns> out;
    const std::wstring base = L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces\\";
    for (const auto& nic : adapters::get_physical_adapters()) {
        if (nic.if_index == 0) continue;
        const std::wstring key = base + nic.guid;
        AdapterDns a;
        a.guid = nic.guid;
        a.if_index = nic.if_index;

        bool representable = true;
        auto ns = registry::read_string(HKEY_LOCAL_MACHINE, key.c_str(), L"NameServer");
        if (ns && !ns->empty()) {
            std::vector<std::wstring> list = split_servers(*ns);
            for (const auto& s : list) if (!is_ipv4(s)) representable = false;
            if (list.empty()) representable = false;
            if (representable) a.static_servers = list;
        }
        if (!representable) continue;   // cannot be restored exactly -> leave this adapter alone

        const std::vector<std::wstring>* src = a.static_servers ? &*a.static_servers : nullptr;
        std::vector<std::wstring> dhcp;
        if (!src) {
            auto d = registry::read_string(HKEY_LOCAL_MACHINE, key.c_str(), L"DhcpNameServer");
            if (d) dhcp = split_servers(*d);
            src = &dhcp;
        }
        for (const auto& s : *src) if (is_ipv4(s)) a.effective_servers.push_back(s);
        out.push_back(std::move(a));
    }
    return out;
}

bool RealSystem::tls12_ok() {
    DohTransport t;
    return t.tls12_available();
}

bool RealSystem::port53_free() {
    return wsa_ok_ && try_bind_loopback53(SOCK_DGRAM) && try_bind_loopback53(SOCK_STREAM);
}

bool RealSystem::service_installed() {
    ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm.h) return false;
    ScHandle svc(OpenServiceW(scm.h, kServiceName, SERVICE_QUERY_STATUS));
    return svc.h != nullptr;
}

std::optional<Upstream> RealSystem::probe_chain(const std::vector<Upstream>& chain, std::wstring& err) {
    RealTransport transport;
    const std::uint16_t id = 0x4E53;   // arbitrary
    const packet::Bytes q = example_query(id);
    for (const auto& u : chain) {
        QueryResult r = transport.query(u, q, 4000);
        if (r.ok && good_answer(r.response, id)) return u;
        err = u.provider_id + (u.method == Method::DoH ? L" doh: " : L" dot: ") +
              (r.ok ? L"unusable answer" : r.error);
    }
    return std::nullopt;
}

bool RealSystem::install_service(const Config& cfg, std::wstring& err) {
    const std::wstring dir = data_dir();
    if (dir.empty() || !ensure_dir(dir) || !ensure_dir(bin_dir())) { err = L"cannot create the data folder"; return false; }

    if (!write_text(config_path(), serialize_config(cfg))) { err = L"cannot write dns.cfg"; return false; }

    // Status file placeholder: the service account (LocalService) may only write THIS file.
    {
        HANDLE h = CreateFileW(status_path().c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        run_tool(L"icacls.exe", L"\"" + status_path() + L"\" /grant *S-1-5-19:(M)");   // best effort
    }

    wchar_t self[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, self, MAX_PATH) == 0) { err = L"cannot locate this program"; return false; }
    if (!CopyFileW(self, bin_exe().c_str(), FALSE)) { err = L"cannot copy the program to the data folder"; return false; }

    ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE));
    if (!scm.h) { err = L"cannot open the Service Control Manager (administrator rights needed)"; return false; }

    const std::wstring path = L"\"" + bin_exe() + L"\" --dns-service";
    ScHandle svc(CreateServiceW(scm.h, kServiceName, L"NetShutUp encrypted DNS resolver",
                                SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
                                SERVICE_ERROR_NORMAL, path.c_str(), nullptr, nullptr, nullptr,
                                L"NT AUTHORITY\\LocalService", nullptr));
    if (!svc.h) { err = L"cannot create the service (error " + std::to_wstring(GetLastError()) + L")"; return false; }

    static wchar_t desc[] = L"Local DNS resolver that forwards queries over DoH/DoT (NetShutUp).";
    SERVICE_DESCRIPTIONW sd{desc};
    ChangeServiceConfig2W(svc.h, SERVICE_CONFIG_DESCRIPTION, &sd);
    SC_ACTION actions[3] = {{SC_ACTION_RESTART, 5000}, {SC_ACTION_RESTART, 10000}, {SC_ACTION_RESTART, 30000}};
    SERVICE_FAILURE_ACTIONSW fa{};
    fa.dwResetPeriod = 86400;
    fa.cActions = 3;
    fa.lpsaActions = actions;
    ChangeServiceConfig2W(svc.h, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);

    if (!StartServiceW(svc.h, 0, nullptr)) { err = L"cannot start the service (error " + std::to_wstring(GetLastError()) + L")"; return false; }
    if (!wait_for_state(svc.h, SERVICE_RUNNING, 15000)) { err = L"the service did not reach the running state"; return false; }
    return true;
}

bool RealSystem::local_resolver_ok() {
    if (!wsa_ok_) return false;
    const std::uint16_t id = 0x4E54;
    const packet::Bytes q = example_query(id);
    for (int attempt = 0; attempt < 6; ++attempt) {
        PlainTransport plain(53);
        QueryResult r = plain.query_legacy({L"127.0.0.1"}, q, 2000);
        if (r.ok && good_answer(r.response, id) && tcp_query_loopback(q, id)) return true;
        Sleep(500);
    }
    return false;
}

bool RealSystem::set_dns(const AdapterDns& a, const std::vector<std::wstring>& servers, std::wstring& err) {
    auto cmds = netsh_restore_commands(a.if_index, servers);
    if (cmds.empty()) { err = L"invalid DNS server list"; return false; }
    for (const auto& c : cmds) {
        if (!run_tool(L"netsh.exe", c)) { err = L"netsh failed for interface " + std::to_wstring(a.if_index); return false; }
    }
    flush_dns_cache();
    return true;
}

bool RealSystem::set_dns_automatic(const AdapterDns& a, std::wstring& err) {
    if (!run_tool(L"netsh.exe", netsh_set_dhcp_args(a.if_index))) {
        err = L"netsh failed for interface " + std::to_wstring(a.if_index);
        return false;
    }
    flush_dns_cache();
    return true;
}

bool RealSystem::remove_service(std::wstring& err) {
    bool ok = true;
    {
        ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
        if (!scm.h) {
            err = L"cannot open the Service Control Manager";
            ok = false;
        } else {
            ScHandle svc(OpenServiceW(scm.h, kServiceName, DELETE | SERVICE_STOP | SERVICE_QUERY_STATUS));
            if (svc.h) {
                SERVICE_STATUS st{};
                ControlService(svc.h, SERVICE_CONTROL_STOP, &st);
                wait_for_state(svc.h, SERVICE_STOPPED, 10000);
                if (!DeleteService(svc.h) && GetLastError() != ERROR_SERVICE_MARKED_FOR_DELETE) {
                    err = L"cannot delete the service (error " + std::to_wstring(GetLastError()) + L")";
                    ok = false;
                }
            } else if (GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST) {
                err = L"cannot open the service (error " + std::to_wstring(GetLastError()) + L")";
                ok = false;
            }
        }
    }
    // Files: removed even if the service step failed, so nothing stale is left behind.
    if (!delete_file_retry(bin_exe())) { err += L" cannot delete the program copy;"; ok = false; }
    RemoveDirectoryW(bin_dir().c_str());
    delete_file_retry(config_path());
    delete_file_retry(status_path());
    delete_file_retry(status_path() + L".tmp");
    return ok;
}

bool RealSystem::write_text(const std::wstring& path, const std::string& bytes) {
    std::wstring parent = parent_of(path);
    if (!parent.empty() && !ensure_dir(parent)) return false;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size();
    if (ok) FlushFileBuffers(h);
    CloseHandle(h);
    return ok;
}

std::optional<std::string> RealSystem::read_text(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::nullopt;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size) || size.QuadPart > 1024 * 1024) { CloseHandle(h); return std::nullopt; }
    std::string data(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD got = 0;
    bool ok = data.empty() || (ReadFile(h, data.data(), static_cast<DWORD>(data.size()), &got, nullptr) && got == data.size());
    CloseHandle(h);
    if (!ok) return std::nullopt;
    return data;
}

std::wstring RealSystem::fastest_provider() {
    RealTransport transport;
    const packet::Bytes q = example_query(0x4E55);
    std::wstring best;
    long long best_ms = -1;
    for (const wchar_t* id : {L"cloudflare", L"google"}) {
        Upstream u{id, Method::DoH};
        transport.query(u, q, 3000);   // warm-up: TLS handshake and connection setup
        long long total = 0;
        int good = 0;
        for (int i = 0; i < 2; ++i) {
            auto t0 = std::chrono::steady_clock::now();
            QueryResult r = transport.query(u, q, 3000);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            if (r.ok) { total += ms; ++good; }
        }
        if (good > 0 && (best_ms < 0 || total / good < best_ms)) { best_ms = total / good; best = id; }
    }
    return best;
}

std::wstring service_state() {
    ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm.h) return L"unknown";
    ScHandle svc(OpenServiceW(scm.h, kServiceName, SERVICE_QUERY_STATUS));
    if (!svc.h) return GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST ? L"not installed" : L"unknown";
    SERVICE_STATUS st{};
    if (!QueryServiceStatus(svc.h, &st)) return L"unknown";
    switch (st.dwCurrentState) {
        case SERVICE_RUNNING: return L"running";
        case SERVICE_STOPPED: return L"stopped";
        case SERVICE_START_PENDING: return L"starting";
        case SERVICE_STOP_PENDING: return L"stopping";
        default: return L"unknown";
    }
}

} // namespace netshutup::dns
