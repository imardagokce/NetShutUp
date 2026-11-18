cmake_minimum_required(VERSION 3.16)

# ── Test suite for NetShutUp Legacy ─────────────────────────────────────────
# Tests must NEVER write to real system registry keys.
# All registry interaction uses the sandbox key:
#   HKEY_CURRENT_USER\Software\NetShutUp-Test
#
# This CMakeLists.txt will grow as modules are added.

# Placeholder test to verify the build pipeline works end-to-end.
add_executable(test_placeholder test_placeholder.cpp)
target_include_directories(test_placeholder PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(test_placeholder PRIVATE
    _WIN32_WINNT=0x0601 WINVER=0x0601
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

if(MSVC)
    set_target_properties(test_placeholder PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_test(NAME placeholder COMMAND test_placeholder)
