<div align="center">

# NetShutUp — Legacy

**Pick a profile. See exactly what changes. Apply it, and undo it with one command.**

A single-file Windows tool that tunes network privacy and performance settings — built for Windows 7, 8, and 8.1.

[![License: GPL v3](https://img.shields.io/badge/license-GPL%20v3-blue.svg?style=flat-square)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows%207%20%7C%208%20%7C%208.1-0078D6?style=flat-square&logo=windows&logoColor=white)](#requirements)
[![Language](https://img.shields.io/badge/language-C%2B%2B20%20%2F%20Win32-00599C?style=flat-square&logo=cplusplus&logoColor=white)](docs/design/ADR-001-legacy-language-selection.md)
[![Status](https://img.shields.io/badge/status-in%20development-orange?style=flat-square)](#status)
[![Branch](https://img.shields.io/badge/branch-legacy-8A2BE2?style=flat-square)](https://github.com/imardagokce/NetShutUp/tree/legacy)

[Features](#features) · [Profiles](#profiles) · [Quick start](#quick-start) · [Safety](#safety-first) · [Roadmap](#roadmap) · [Contributing](#contributing)

</div>

---

> [!NOTE]
> This is the **Legacy** branch of NetShutUp, targeting **Windows 7 SP1, 8, and 8.1**.
> For Windows 10/11, see the [`main`](https://github.com/imardagokce/NetShutUp/tree/main) branch (Rust).
> The two product lines are **fully independent** — no shared code or runtime.

## What is it?

NetShutUp Legacy is the network-focused cousin of tools like O&O ShutUp10, rebuilt for older Windows.
You choose a profile — **Privacy**, **Performance** or **Recommended** — and the program shows you every registry value and service setting it would touch, asks for confirmation, saves a backup, applies the change and checks the result.

It exists because "network optimization" tips on the internet are a mixed bag: some help, some do nothing, some quietly make things worse. NetShutUp only includes tweaks whose registry path, default value and effect were checked against Microsoft's documentation.

> [!NOTE]
> **Active Development (Pre-Alpha).** Core modules (`scan`, `preview`, `backup`, and registry engine) are implemented with complete sandbox unit test coverage. Full `apply` / `revert` execution and the terminal menu are currently being developed.

## Features

- **Read-only first.** `scan` and `preview` show the current state and planned changes without touching anything, and without administrator rights.
- **Backup before every change.** The previous value of every touched setting is saved and verified on disk before the first write. A setting that was *not set* is restored as *not set*, not as zero.
- **One-command undo.** `revert` puts everything back from the backup; a failed write rolls back everything done so far.
- **Accurate version detection.** Uses `RtlGetVersion` to read the real Windows version — `GetVersionExW` lies on Windows 8.1+ due to the compatibility shim.
- **Interactive menu.** Run the `.exe` for an arrow-key menu; or use plain commands in a terminal or script.
- **One file.** No installer, no extra DLLs, no background service, nothing sent over the network.

## Profiles

| Profile | For | What it does |
|---|---|---|
| **Privacy** | Less telemetry, fewer leaks | Disables telemetry services, turns off LLMNR name broadcasts and peer-to-peer update sharing, sets diagnostic-data policy to the minimum. May also ask optional firewall questions, each answered No by default. |
| **Performance** | Games and real-time apps | Turns off delayed ACK per adapter, removes the multimedia network-throttling limit, turns off peer-to-peer update sharing. |
| **Recommended** | Everyday use | Turns off LLMNR and peer-to-peer update sharing. Safe, widely useful, nothing that gets in the way. |

> [!NOTE]
> DNS-over-HTTPS (DoH) and DNS-over-TLS (DoT) are **not available** on Windows 7/8/8.1 at the OS level and are not part of Legacy profiles. Classic DNS server settings may be added in a future milestone.

Every profile also ensures Windows Firewall is on and blocks inbound connections by default (your existing allow rules keep working). If a third-party firewall is registered with Windows Security Center, the firewall step is skipped automatically.

## Quick start

> [!IMPORTANT]
> No release is available yet. The instructions below describe the intended workflow once the first build is ready.

```text
netshutup-legacy                        Open the interactive menu
netshutup-legacy scan                   Show the current network settings
netshutup-legacy preview <profile>      Show what a profile would change
netshutup-legacy apply <profile>        Apply a profile (asks first, saves a backup)
netshutup-legacy apply <profile> --dry-run
                                        Go through apply without changing anything
netshutup-legacy revert                 Undo the most recent apply
netshutup-legacy selftest               Test every profile for real and write a report
```

`apply`, `revert` and `selftest` require administrator rights; the program asks through the standard UAC prompt, and only when you run one of those commands.

### Requirements

| | |
|---|---|
| **Windows 7 SP1** | x86 and x64 (architecture support to be confirmed — see [ADR-001](docs/design/ADR-001-legacy-language-selection.md)) |
| **Windows 8** | Tested separately from 8.1 |
| **Windows 8.1** | Tested separately from 8 |
| .NET / runtime | None required — single native `.exe`, Win32 only |

## Safety first

The rules the code is built around (and tests enforce):

1. Nothing changes before you have seen a preview and confirmed it.
2. A verified backup exists before the first change.
3. A failure halfway undoes the changes made so far.
4. Settings are only written for physical Ethernet and Wi-Fi adapters; virtual adapters, VPNs and tunnels are left alone.
5. Nothing leaves your computer.
6. Program text is always English — localized Windows output is never parsed.

## Roadmap

- [x] Project skeleton, CMake build system (C++20, static CRT `/MT`)
- [x] Real Windows version detection via `RtlGetVersion` (bypassing `GetVersionExW` shims)
- [x] Settings catalog (verified against Microsoft documentation) and read-only `scan` command
- [x] Verified backup engine (`.nsub` UTF-8 plain text with atomic verification and "not set" restoration)
- [x] Built-in profile preview engine (`preview <profile>`) with change diff summary
- [x] Apply and revert execution engine with automatic UAC elevation and safety rollback
- [x] Interactive terminal menu (arrow-key navigation, color-coded, zero-ANSI via native Win32 console APIs)
- [ ] Physical network adapter enumeration (`GetAdaptersAddresses`)
- [ ] Self-test mode with a masked report
- [ ] Native Win32 desktop GUI (O&O ShutUp10-style native interface)
- [ ] First alpha release with a prebuilt `.exe`

## Repository layout

```text
legacy/      C++20/Win32 application for Windows 7/8/8.1 (this branch)
docs/        Design notes and compatibility matrix (shared documentation)
  design/    Architecture Decision Records (ADRs) — source of truth for decisions
```

## Status

**In active development (pre-alpha).** Built with **C++20** and native Win32 APIs targeting Windows 7 SP1, 8, and 8.1. Static CRT linkage ensures no external VC++ runtime DLLs are needed on target machines. `scan`, `preview`, and `backup` engines are implemented and verified with 100% passing sandbox tests. The Modern line (`main` branch) is at `v0.1.0-alpha.1`, in testing.

## Contributing

Testers and contributors are welcome once the first build is available. See [CONTRIBUTING.md](CONTRIBUTING.md) for the full guide: how to test safely, how to send a report, and how to work on the code.

Security problems should be reported privately — see [SECURITY.md](SECURITY.md).

## License

NetShutUp is free software under the **GNU General Public License v3.0**. Forks must remain open source. See [`LICENSE`](LICENSE).
