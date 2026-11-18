# Changelog — NetShutUp Legacy

All notable changes to the **Legacy** line (Windows 7/8/8.1) are listed here.
The project follows [Semantic Versioning](https://semver.org/); versions before 1.0 may change freely.

For the Modern line (Windows 10/11), see the `main` branch changelog.

---

## [Unreleased]

### Changed
- C++ standard bumped from C++17 to **C++20** (MSVC 2019+). Win8+-only STL
  primitives (`std::atomic_wait`, `std::latch`, `std::barrier`,
  `std::counting_semaphore`) remain banned project-wide; noted in ADR-001.

### Added
- `docs/design/ADR-001-legacy-language-selection.md` — language and runtime decision recorded (C++20 / Win32 toolchain with Windows 7 STL restrictions).
- `docs/compatibility-matrix.md` updated to reflect C++/Win32 selection.
- `legacy/` project skeleton: CMake build system (C++20, static CRT `/MT`), application manifest, OS version detection (`RtlGetVersion`), placeholder entry point, test harness.
- `README.md`, `CONTRIBUTING.md`, `SECURITY.md`, `CHANGELOG.md` — legacy-adapted project documentation updated for C++20 toolchain and active development progress.
- `legacy/src/registry.h/cpp` — RAII registry wrapper; read, write, delete, enumerate (sandbox-safe).
- `legacy/src/catalog.h/cpp` — Settings catalog: 6 verified Win7/8/8.1 entries across Privacy and Performance profiles.
- `legacy/src/scan.h/cpp` — Read-only `scan` command; reads all global-scope catalog entries, reports current vs. default.
- `legacy/src/backup.h/cpp` — Backup and restore engine: UTF-8 `.nsub` plain text serialization, atomic verification before apply, restoration of values and deleted ("not set") states.
- `legacy/src/preview.h/cpp` — Read-only `preview <profile>` command: diff calculation against catalog targets with formatted table and change summary.
- `legacy/src/elevation.h/cpp` — UAC administrator privilege detection and self-elevation via `ShellExecuteExW` (`runas`).
- `legacy/src/apply.h/cpp` — Transactional profile apply and revert engine: backup-before-write guarantee, automatic rollback on write failures, `--dry-run` simulation mode, and one-command revert.
- `legacy/src/menu.h/cpp` — Interactive Win32 console menu: arrow-key navigation, colored highlights, live status bar, zero-ANSI escape sequences for 100% native Windows 7/8/8.1 `cmd.exe` compatibility.
- `legacy/src/adapters.h/cpp` — Physical network adapter enumeration using `GetAdaptersAddresses`: filters out virtual interfaces, tunnels, loopbacks, and VPNs to target only genuine physical hardware.
- `legacy/tests/test_registry.cpp` — Registry sandbox unit tests (7 cases, all under `HKCU\Software\NetShutUp-Test`).
- `legacy/tests/test_backup.cpp` — Backup engine sandbox unit tests (save/load roundtrip, corruption handling, value restore, deletion restore, path generation).
- `legacy/tests/test_preview.cpp` — Preview engine unit tests (profile flags, diff classification, pending adapter detection).
- `legacy/tests/test_apply.cpp` — Apply and revert sandbox unit tests (dry-run isolation, apply-and-revert roundtrip, automatic rollback on write failure).
- `legacy/tests/test_adapters.cpp` — Physical adapter enumeration and subkey GUID formatting unit tests.
