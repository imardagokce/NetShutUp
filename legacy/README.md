# NetShutUp Legacy — Build Guide

This directory contains the C++20/Win32 application for Windows 7 SP1, 8 and 8.1.

## Requirements

| Tool | Version | Notes |
|---|---|---|
| CMake | ≥ 3.16 | |
| MSVC | VS 2019 16.11 or later | With "Desktop development with C++" workload; must support `/std:c++20` |
| Windows SDK | 10 or later | Code targets Windows 7 SP1 via `_WIN32_WINNT=0x0601` |

MinGW-w64 (GCC) may work but is not the primary target — MSVC is used for CI.

> **Forbidden STL features** (they pull in `WaitOnAddress`, Windows 8+, and crash
> on Windows 7 SP1): `std::atomic_wait`, `std::atomic_notify_*`, `std::latch`,
> `std::barrier`, `std::counting_semaphore`. See
> [`ADR-001`](../docs/design/ADR-001-legacy-language-selection.md).

## Build

```bat
:: From the repo root, on the legacy branch:
cd legacy
cmake -B build
cmake --build build --config Release
```

The output binary is `build\Release\netshutup-legacy.exe` (static CRT, no
VCRUNTIME dependency; runs on Windows 7 SP1 and later).

## Run tests

```bat
cd legacy
cmake -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug
```

> **Important:** Tests must never write to real system registry keys.
> All registry interaction uses `HKEY_CURRENT_USER\Software\NetShutUp-Test`.

## Commands

```text
netshutup-legacy                          Interactive console menu
netshutup-legacy scan                     Show current settings vs defaults
netshutup-legacy preview <profile>        Show what a profile would change
netshutup-legacy apply <profile> [--dry-run]
netshutup-legacy revert [backup]          Undo the last apply (or a given backup)
netshutup-legacy selftest                 Tester mode: apply/revert every profile, write a report
```

Profiles: `privacy`, `performance`, `recommended`. Add `--ignore-os` (after the
command) to run on Windows 10/11 for development. Tester guide:
[`docs/TESTING-legacy.md`](../docs/TESTING-legacy.md).

## Project layout

```text
legacy/
  CMakeLists.txt          top-level build definition
  src/
    main.cpp              entry point and CLI dispatcher
    menu.h / .cpp         interactive Win32 console menu (no ANSI)
    version.h / .cpp      OS version detection via RtlGetVersion
    registry.h / .cpp     RAII registry access
    catalog.h / .cpp      verified settings catalog and profile membership
    scan / preview        read-only state and diff
    backup / apply        verified backups, transactional apply and revert
    elevation             UAC detection and relaunch
    adapters              physical adapter enumeration
    selftest*.h / .cpp    tester mode (runner, masking, report, real wiring)
    netshutup.manifest    Windows application manifest (UAC + supportedOS)
  profiles/               built-in profile definitions
  tests/                  CTest suites (sandbox registry only)
```

See [`docs/design/`](../docs/design/) for architecture decisions.
