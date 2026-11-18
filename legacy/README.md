# NetShutUp Legacy — Build Guide

This directory contains the C++17/Win32 application for Windows 7 SP1, 8 and 8.1.

## Requirements

| Tool | Version | Notes |
|---|---|---|
| CMake | ≥ 3.16 | |
| MSVC | VS 2017 or later | With "Desktop development with C++" workload |
| Windows SDK | 7.1 or later | 7.1 SDK recommended for Win7 SP1 target |

MinGW-w64 (GCC) may work but is not the primary target — MSVC is used for CI.

## Build

```bat
:: From the repo root, on the legacy branch:
cd legacy
cmake -B build\debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build\debug

:: Release (static CRT, no VCRUNTIME dependency)
cmake -B build\release -DCMAKE_BUILD_TYPE=Release
cmake --build build\release --config Release
```

The output binary is `build\release\netshutup-legacy.exe`. It has no runtime
dependencies and runs on Windows 7 SP1 and later.

## Run tests

```bat
cd legacy
cmake -B build\test -DBUILD_TESTS=ON
cmake --build build\test
ctest --test-dir build\test
```

> **Important:** Tests must never write to real system registry keys.
> All registry interaction uses `HKEY_CURRENT_USER\Software\NetShutUp-Test`.

## Project layout

```text
legacy/
  CMakeLists.txt          top-level build definition
  src/
    main.cpp              entry point and CLI dispatcher (placeholder)
    version.h / .cpp      OS version detection via RtlGetVersion
    netshutup.manifest    Windows application manifest (UAC + supportedOS)
  profiles/               built-in profile definitions (embedded in binary)
  tests/
    CMakeLists.txt        test build definition
    test_placeholder.cpp  build-pipeline smoke test
```

New modules will be added here as development progresses.
See [`docs/design/`](../docs/design/) for architecture decisions.
