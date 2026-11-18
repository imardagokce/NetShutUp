cmake_minimum_required(VERSION 3.16)

# ── Test suite for NetShutUp Legacy ─────────────────────────────────────────
# Tests must NEVER write to real system registry keys.
# All registry interaction uses the sandbox key:
#   HKEY_CURRENT_USER\Software\NetShutUp-Test

# Shared sources needed by multiple tests (no main.cpp).
set(LEGACY_LIB_SOURCES
    ${PROJECT_SOURCE_DIR}/src/registry.cpp
    ${PROJECT_SOURCE_DIR}/src/catalog.cpp
    ${PROJECT_SOURCE_DIR}/src/version.cpp
    ${PROJECT_SOURCE_DIR}/src/scan.cpp
    ${PROJECT_SOURCE_DIR}/src/backup.cpp
    ${PROJECT_SOURCE_DIR}/src/preview.cpp
    ${PROJECT_SOURCE_DIR}/src/elevation.cpp
    ${PROJECT_SOURCE_DIR}/src/apply.cpp
    ${PROJECT_SOURCE_DIR}/src/adapters.cpp
)

# Common settings applied to every test target.
function(configure_test target)
    target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_compile_definitions(${target} PRIVATE
        _WIN32_WINNT=0x0601 WINVER=0x0601
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    if(MSVC)
        set_target_properties(${target} PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    endif()
endfunction()

# ── test_placeholder ─────────────────────────────────────────────────────────
add_executable(test_placeholder test_placeholder.cpp)
configure_test(test_placeholder)
add_test(NAME placeholder COMMAND test_placeholder)

# ── test_registry ─────────────────────────────────────────────────────────────
add_executable(test_registry
    test_registry.cpp
    ${LEGACY_LIB_SOURCES}
)
configure_test(test_registry)
add_test(NAME registry COMMAND test_registry)

# ── test_backup ───────────────────────────────────────────────────────────────
add_executable(test_backup
    test_backup.cpp
    ${LEGACY_LIB_SOURCES}
)
configure_test(test_backup)
add_test(NAME backup COMMAND test_backup)

# ── test_preview ──────────────────────────────────────────────────────────────
add_executable(test_preview
    test_preview.cpp
    ${LEGACY_LIB_SOURCES}
)
configure_test(test_preview)
add_test(NAME preview COMMAND test_preview)

# ── test_apply ────────────────────────────────────────────────────────────────
add_executable(test_apply
    test_apply.cpp
    ${LEGACY_LIB_SOURCES}
)
configure_test(test_apply)
add_test(NAME apply COMMAND test_apply)

# ── test_adapters ─────────────────────────────────────────────────────────────
add_executable(test_adapters
    test_adapters.cpp
    ${LEGACY_LIB_SOURCES}
)
configure_test(test_adapters)
target_link_libraries(test_adapters PRIVATE iphlpapi)
add_test(NAME adapters COMMAND test_adapters)
