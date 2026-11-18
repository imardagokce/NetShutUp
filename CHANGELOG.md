# Changelog — NetShutUp Legacy

All notable changes to the **Legacy** line (Windows 7/8/8.1) are listed here.
The project follows [Semantic Versioning](https://semver.org/); versions before 1.0 may change freely.

For the Modern line (Windows 10/11), see the `main` branch changelog.

---

## [Unreleased]

### Added
- `docs/design/ADR-001-legacy-language-selection.md` — language and runtime decision recorded (C++17 / Win32).
- `docs/compatibility-matrix.md` updated to reflect C++/Win32 selection.
- `legacy/` project skeleton: CMake build system, application manifest, OS version detection (`RtlGetVersion`), placeholder entry point, test harness.
- `README.md`, `CONTRIBUTING.md`, `SECURITY.md`, `CHANGELOG.md` — legacy-adapted project documentation.
- `legacy/src/registry.h/cpp` — RAII registry wrapper; read, write, delete, enumerate (sandbox-safe).
- `legacy/src/catalog.h/cpp` — Settings catalog: 6 verified Win7/8/8.1 entries across Privacy and Performance profiles.
- `legacy/src/scan.h/cpp` — Read-only `scan` command; reads all global-scope catalog entries, reports current vs. default.
- `legacy/tests/test_registry.cpp` — Registry sandbox unit tests (7 cases, all under `HKCU\Software\NetShutUp-Test`).
