#pragma once

// dns_service.h — runs the local DNS server as a Windows service (`--dns-service`,
// started by the Service Control Manager) or in the foreground (`dns-serve`, a
// developer/manual-test command). Installing the service is stage 7d.
// Design: docs/design/2026-11-18-legacy-dns-stub.md

#include "dns_config.h"
#include "dns_status.h"
#include <optional>
#include <string>

namespace netshutup::dns {

inline constexpr wchar_t kServiceName[] = L"NetShutUpDns";

std::wstring data_dir();      // %ProgramData%\NetShutUp (not created here)
std::wstring config_path();   // data_dir() + \dns.cfg
std::wstring status_path();   // data_dir() + \dns.status

// Reads and parses a dns.cfg file. nullopt + `err` (short English) on failure.
std::optional<Config> load_config(const std::wstring& path, std::wstring& err);

// Writes the status file via a temp file + replace (readers never see a partial file).
// Best effort: returns false if the folder is not writable.
bool write_status_file(const std::wstring& path, const ServiceStatus& s);
std::optional<ServiceStatus> read_status_file(const std::wstring& path);

// SCM entry point for `netshutup-legacy --dns-service`: blocks until the service
// stops. Loads config_path(), listens on 127.0.0.1:53, writes status_path() every
// 5 seconds. Returns a process exit code (0 = clean stop).
int run_service();

// Foreground server for manual testing: listens on bind_ip:port using the config at
// cfg_path until Ctrl+C, printing a one-line status every 5 seconds. Returns 0 on a
// clean stop, 1 on a startup failure (reason printed).
int run_foreground(const std::wstring& cfg_path, const std::wstring& bind_ip, unsigned short port);

} // namespace netshutup::dns
