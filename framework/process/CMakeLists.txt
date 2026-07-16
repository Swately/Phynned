cmake_minimum_required(VERSION 3.20)
project(phyriad_process LANGUAGES CXX)

# ── phyriad_process — pillar process (STATIC) ──────────────────────────────────
# Lightweight, zero-heap-allocation process enumeration.
#
# Public headers:
#   ProcessEnumerator.hpp — ProcessEntry POD + enumerate_processes()
#
# Compiled sources:
#   src/ProcessEnumerator.cpp — Win32 (EnumProcesses) and Linux (/proc) paths
#
# Direct dependencies:
#   - phyriad_hal    (INTERFACE) — platform constants
#   - phyriad_schema (PUBLIC)    — Error type
#
# Platform system libraries:
#   Windows: psapi.lib  (EnumProcesses, QueryFullProcessImageNameA,
#                        GetProcessTimes)
#   Linux:   none (opendir/readdir/open are libc)
#


# ── Standalone dependency resolution ─────────────────────────────────────────
if(NOT TARGET phyriad_hal)
    add_subdirectory(
        ${CMAKE_CURRENT_SOURCE_DIR}/../hal
        ${CMAKE_CURRENT_BINARY_DIR}/phyriad_hal
    )
endif()

if(NOT TARGET phyriad_schema)
    add_subdirectory(
        ${CMAKE_CURRENT_SOURCE_DIR}/../schema
        ${CMAKE_CURRENT_BINARY_DIR}/phyriad_schema
    )
endif()

# ── Library ───────────────────────────────────────────────────────────────────
add_library(phyriad_process STATIC
    src/ProcessEnumerator.cpp
    src/CurrentProcess.cpp
    src/ProcessMetricsSnapshot.cpp
)

target_include_directories(phyriad_process PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(phyriad_process PUBLIC cxx_std_23)

target_link_libraries(phyriad_process PUBLIC
    phyriad_hal
    phyriad_schema
)

# ── Platform-specific system libraries ───────────────────────────────────────
if(WIN32)
    # psapi: EnumProcesses, QueryFullProcessImageNameA, GetModuleBaseNameA.
    # ntdll: NOT linked explicitly — NtQuerySystemInformation is loaded via
    #        GetProcAddress("ntdll.dll", ...) at runtime.
    target_link_libraries(phyriad_process PUBLIC psapi)
endif()

if(UNIX AND NOT APPLE)
    # /proc-based capture uses dirent (libc, no extra link needed).
    # readlink for CurrentProcess needs no extra link on modern glibc.
endif()

if(MSVC)
    target_compile_options(phyriad_process PRIVATE /W4)
else()
    target_compile_options(phyriad_process PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ── Tests ─────────────────────────────────────────────────────────────────────
if(BUILD_TESTING OR PHYRIAD_BUILD_TESTS)
    include(CTest)

    add_executable(process_test
        tests/process_test.cpp
    )
    target_link_libraries(process_test PRIVATE phyriad_process)
    target_compile_features(process_test PRIVATE cxx_std_23)
    if(NOT MSVC)
        target_compile_options(process_test PRIVATE -Wall -Wextra)
    endif()
    add_test(
        NAME    process_test
        COMMAND process_test
        LABELS  process
    )

    # ── FR-18: CurrentProcess test ────────────────────────────────────────────
    add_executable(current_process_test
        tests/current_process_test.cpp
    )
    target_link_libraries(current_process_test PRIVATE phyriad_process)
    target_compile_features(current_process_test PRIVATE cxx_std_23)
    if(MSVC)
        target_compile_options(current_process_test PRIVATE /W4)
    else()
        target_compile_options(current_process_test PRIVATE -Wall -Wextra)
    endif()
    add_test(
        NAME    current_process_test
        COMMAND current_process_test
        LABELS  process
    )

    # ── FR-11: ProcessMetricsSnapshot test ────────────────────────────────────
    add_executable(process_metrics_snapshot_test
        tests/process_metrics_snapshot_test.cpp
    )
    target_link_libraries(process_metrics_snapshot_test PRIVATE phyriad_process)
    target_compile_features(process_metrics_snapshot_test PRIVATE cxx_std_23)
    if(MSVC)
        target_compile_options(process_metrics_snapshot_test PRIVATE /W4)
    else()
        target_compile_options(process_metrics_snapshot_test PRIVATE -Wall -Wextra)
    endif()
    add_test(
        NAME    process_metrics_snapshot_test
        COMMAND process_metrics_snapshot_test
        LABELS  process
    )

    # ── GFR-Ayama-3: thread enumeration extension to ProcessMetricsSnapshot ─
    add_executable(enumerate_threads_test
        tests/enumerate_threads_test.cpp
    )
    target_link_libraries(enumerate_threads_test PRIVATE phyriad_process)
    target_compile_features(enumerate_threads_test PRIVATE cxx_std_23)
    if(NOT MSVC)
        target_compile_options(enumerate_threads_test PRIVATE -Wall -Wextra)
    endif()
    add_test(
        NAME    enumerate_threads_test
        COMMAND enumerate_threads_test
        LABELS  process
    )
endif()
# Made with my soul - Swately <3
