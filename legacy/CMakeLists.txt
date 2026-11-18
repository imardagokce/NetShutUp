cmake_minimum_required(VERSION 3.16)
project(netshutup-legacy VERSION 0.1.0 LANGUAGES CXX)

# ── C++ standard ────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Windows target: Windows 7 SP1 (0x0601) ──────────────────────────────────
# Must be set before any Windows headers are pulled in.
add_compile_definitions(
    _WIN32_WINNT=0x0601
    WINVER=0x0601
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# ── MSVC-specific flags ──────────────────────────────────────────────────────
if(MSVC)
    # Static CRT: no VCRUNTIME dependency on the target machine
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    add_compile_options(
        /W4         # High warning level
        /WX         # Warnings as errors
        /permissive- # Strict conformance
        /utf-8      # Source and execution charset
    )
endif()

# ── Sources ──────────────────────────────────────────────────────────────────
set(SOURCES
    src/main.cpp
    src/version.cpp
    src/registry.cpp
    src/catalog.cpp
    src/scan.cpp
    src/backup.cpp
    src/preview.cpp
    src/elevation.cpp
    src/apply.cpp
)

# Console subsystem: CLI + interactive menu (no WIN32 flag).
add_executable(netshutup-legacy ${SOURCES})

target_include_directories(netshutup-legacy PRIVATE src)

# Link ntdll for RtlGetVersion (loaded at runtime via GetProcAddress,
# but we still declare the import so the linker resolves the module handle)
target_link_libraries(netshutup-legacy PRIVATE ntdll)

# ── Application manifest ─────────────────────────────────────────────────────
# Sets the subsystem and declares Windows 7 compatibility.
# Replace with a full .manifest file when UAC elevation is wired up.
if(MSVC)
    target_sources(netshutup-legacy PRIVATE src/netshutup.manifest)
endif()

# ── Tests ────────────────────────────────────────────────────────────────────
option(BUILD_TESTS "Build the test suite" ON)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ── Install ───────────────────────────────────────────────────────────────────
install(TARGETS netshutup-legacy RUNTIME DESTINATION .)
