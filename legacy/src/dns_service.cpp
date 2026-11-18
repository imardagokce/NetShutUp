// dns_service.cpp — service host and foreground runner for the local DNS server.

#include <windows.h>
#include <shlobj.h>

#include "dns_service.h"
#include "dns_real_transport.h"
#include "dns_server.h"

#include <cstdio>
#include <string>

namespace netshutup::dns {

namespace {

std::wstring utc_now() {
    SYSTEMTIME st;
    GetSystemTime(&st);
    wchar_t buf[40];
    swprintf(buf, 40, L"%04u-%02u-%02uT%02u:%02u:%02uZ", st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond);
    return buf;
}

std::string to_utf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::optional<std::string> read_file(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
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

// ── Service plumbing ──────────────────────────────────────────────────────────

SERVICE_STATUS_HANDLE g_status_handle = nullptr;
HANDLE                g_stop_event = nullptr;
DWORD                 g_state = SERVICE_STOPPED;

void report(DWORD state, DWORD exit_code = NO_ERROR, DWORD wait_hint = 0) {
    static DWORD checkpoint = 1;
    SERVICE_STATUS ss{};
    ss.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    ss.dwCurrentState = state;
    ss.dwControlsAccepted = (state == SERVICE_START_PENDING) ? 0 : (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN);
    ss.dwWin32ExitCode = exit_code;
    ss.dwWaitHint = wait_hint;
    ss.dwCheckPoint = (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0 : checkpoint++;
    g_state = state;
    if (g_status_handle) SetServiceStatus(g_status_handle, &ss);
}

DWORD WINAPI control_handler(DWORD control, DWORD, LPVOID, LPVOID) {
    switch (control) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            report(SERVICE_STOP_PENDING, NO_ERROR, 10000);
            if (g_stop_event) SetEvent(g_stop_event);
            return NO_ERROR;
        case SERVICE_CONTROL_INTERROGATE:
            return NO_ERROR;
        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

void WINAPI service_main(DWORD, LPWSTR*) {
    g_status_handle = RegisterServiceCtrlHandlerExW(kServiceName, control_handler, nullptr);
    if (!g_status_handle) return;
    report(SERVICE_START_PENDING, NO_ERROR, 10000);

    g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::wstring err;
    auto cfg = load_config(config_path(), err);
    if (!cfg || !g_stop_event) { report(SERVICE_STOPPED, ERROR_BAD_CONFIGURATION); return; }

    RealTransport transport;
    ServerOptions opts;   // 127.0.0.1:53
    Server server(*cfg, transport, opts);
    if (!server.start(err)) { report(SERVICE_STOPPED, ERROR_ADDRESS_ALREADY_ASSOCIATED); return; }

    report(SERVICE_RUNNING);
    do {
        ServiceStatus st = server.status();
        st.updated_utc = utc_now();
        write_status_file(status_path(), st);
    } while (WaitForSingleObject(g_stop_event, 5000) == WAIT_TIMEOUT);

    server.stop();
    ServiceStatus st = server.status();
    st.updated_utc = utc_now();
    write_status_file(status_path(), st);
    CloseHandle(g_stop_event);
    g_stop_event = nullptr;
    report(SERVICE_STOPPED);
}

HANDLE g_fg_stop = nullptr;
BOOL WINAPI console_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        if (g_fg_stop) SetEvent(g_fg_stop);
        return TRUE;
    }
    return FALSE;
}

} // namespace

// ── Paths and files ───────────────────────────────────────────────────────────

std::wstring data_dir() {
    wchar_t buf[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, SHGFP_TYPE_CURRENT, buf))) return L"";
    return std::wstring(buf) + L"\\NetShutUp";
}
std::wstring config_path() { return data_dir() + L"\\dns.cfg"; }
std::wstring status_path() { return data_dir() + L"\\dns.status"; }

std::optional<Config> load_config(const std::wstring& path, std::wstring& err) {
    auto text = read_file(path);
    if (!text) { err = L"cannot read " + path; return std::nullopt; }
    auto cfg = parse_config(*text);
    if (!cfg) { err = L"invalid DNS configuration file"; return std::nullopt; }
    return cfg;
}

bool write_status_file(const std::wstring& path, const ServiceStatus& s) {
    const std::string bytes = serialize_status(s);
    const std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        // The service account may only write the status file itself, not create files in the
        // folder: overwrite the existing file in place (it is a few hundred bytes).
        HANDLE in_place = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (in_place == INVALID_HANDLE_VALUE) return false;
        DWORD w = 0;
        bool ok2 = WriteFile(in_place, bytes.data(), static_cast<DWORD>(bytes.size()), &w, nullptr) && w == bytes.size();
        if (ok2) SetEndOfFile(in_place);
        CloseHandle(in_place);
        return ok2;
    }
    DWORD written = 0;
    bool ok = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size();
    CloseHandle(h);
    if (!ok) { DeleteFileW(tmp.c_str()); return false; }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) { DeleteFileW(tmp.c_str()); return false; }
    return true;
}

std::optional<ServiceStatus> read_status_file(const std::wstring& path) {
    auto text = read_file(path);
    if (!text) return std::nullopt;
    return parse_status(*text);
}

// ── Entry points ──────────────────────────────────────────────────────────────

int run_service() {
    SERVICE_TABLE_ENTRYW table[] = {
        {const_cast<LPWSTR>(kServiceName), service_main},
        {nullptr, nullptr},
    };
    if (!StartServiceCtrlDispatcherW(table)) {
        wprintf(L"This mode is started by the Service Control Manager (error %lu).\n", GetLastError());
        return 1;
    }
    return 0;
}

int run_foreground(const std::wstring& cfg_path, const std::wstring& bind_ip, unsigned short port) {
    std::wstring err;
    auto cfg = load_config(cfg_path, err);
    if (!cfg) { wprintf(L"Error: %ls\n", err.c_str()); return 1; }

    RealTransport transport;
    ServerOptions opts;
    opts.bind_ip = bind_ip;
    opts.port = port;
    Server server(*cfg, transport, opts);
    if (!server.start(err)) { wprintf(L"Error: %ls\n", err.c_str()); return 1; }

    g_fg_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetConsoleCtrlHandler(console_handler, TRUE);
    wprintf(L"DNS server listening on %ls:%u (UDP+TCP). Press Ctrl+C to stop.\n",
            bind_ip.c_str(), static_cast<unsigned>(server.bound_port()));
    while (WaitForSingleObject(g_fg_stop, 5000) == WAIT_TIMEOUT) {
        wprintf(L"%ls\n", describe_status(server.status()).c_str());
    }
    wprintf(L"Stopping...\n");
    server.stop();
    SetConsoleCtrlHandler(console_handler, FALSE);
    CloseHandle(g_fg_stop);
    g_fg_stop = nullptr;
    return 0;
}

} // namespace netshutup::dns
