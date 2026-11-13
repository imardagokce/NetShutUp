# Compatibility Matrix (Proposed)

This document establishes the initial support scope for NetShutUp. All entries are **proposed targets, not verified compatibility claims**. A Windows version should only be advertised as supported after testing the built application on that version.

## Product lines

The project will have two independent applications with separate source trees and build pipelines. They will not share application code or runtime dependencies.

| Product line | Proposed implementation | Proposed OS targets | Status |
|---|---|---|---|
| Legacy | Native C++ / Win32 | Windows 7 SP1, Windows 8, Windows 8.1 | Planned; toolchain and API compatibility need validation |
| Modern | C# / .NET | Windows 10, Windows 11 | Planned; minimum supported .NET version and Windows 10 build range need definition |

Suggested repository layout:

```text
legacy/  # standalone Legacy application
modern/  # standalone Modern application
 docs/  # shared product and compatibility documentation only
```

The `docs/` directory may contain shared requirements and test plans, but the applications must remain independently buildable and must not depend on shared runtime code.

## OS support scope

| Windows version | Product line | Initial status | Notes |
|---|---|---|---|
| Windows 7 SP1 | Legacy | Proposed | Test separately on x86 and x64 if both are selected for support. No claim that the OS is secure or currently maintained. |
| Windows 8 | Legacy | Proposed | Include explicitly; do not treat it as interchangeable with 8.1. |
| Windows 8.1 | Legacy | Proposed | Test separately from Windows 8. |
| Windows 10 | Modern | Proposed | Define the exact editions/builds and .NET runtime before publishing a support claim. Windows 10 servicing status varies; compatibility does not imply ongoing Microsoft security support. |
| Windows 11 | Modern | Proposed | Define supported editions, minimum build, and CPU architectures before publishing a support claim. |

Architecture support (x86, x64, ARM64) is not decided yet. Do not promise an architecture until the build and test matrix covers it.

## Initial safety requirements

- The first implementation milestone is read-only system diagnostics; it must not change network settings.
- Before any setting is changed, show the proposed changes and preserve the original values for application-level rollback.
- A Windows restore point is an additional recovery option, not a substitute for recording the settings changed by the application and not a backup of personal files.
- If restore-point creation is requested, verify that it succeeded before proceeding. Any bypass must be an explicit, informed user choice.
- Describe Legacy hardening as a limited attempt to reduce exposure, never as protection against or prevention of attacks, and clearly state that it cannot replace OS security updates.

## Decisions and validation still needed

1. Decide whether Legacy supports both x86 and x64, or x64 only.
2. Select and test a compiler/Windows SDK toolchain capable of producing the intended Legacy binaries for Windows 7 SP1, 8, and 8.1.
3. Set the exact Windows 10 editions/builds and .NET runtime for Modern.
4. Decide Modern architecture support, including whether ARM64 is in scope.
5. Create clean test environments for every claimed OS/build/architecture combination.

Until these decisions are made and tests pass, the table above is a planning document rather than a compatibility guarantee.
