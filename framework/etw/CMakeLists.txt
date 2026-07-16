cmake_minimum_required(VERSION 3.20)
project(phyriad_etw LANGUAGES CXX)

# ── phyriad_etw — pillar etw (STATIC) ──────────────────────────────────────────
# ETW (Event Tracing for Windows) session lifecycle manager.
#
# Public headers:
#   SessionManager.hpp — SessionManager + ProviderSpec + EventCallback +
#                        providers:: GUID constants
#
# Compiled sources:
#   src/SessionManager.cpp — Windows implementation only
#                            (compiles to empty TU on Linux)
#
# Direct dependencies:
#   - phyriad_schema (PUBLIC) — phyriad::Error, phyriad::ErrorCode
#   - phyriad_hal    (PUBLIC) — SessionManager.hpp uses hal::MemoryOrder
#
# Platform system libraries (Windows only):
#   - advapi32 — StartTrace, ControlTrace, EnableTraceEx2
#   - tdh      — ProcessTrace, OpenTrace, CloseTrace (Trace Data Helper)
#
# Non-Windows: header exposes no-op stubs. The library still links and
# compiles cleanly. start() returns ErrorCode::Unavailable.
#


# ── Standalone dependency resolution ─────────────────────────────────────────
if(NOT TARGET phyriad_schema)
    add_subdirectory(
        ${CMAKE_CURRENT_SOURCE_DIR}/../schema
        ${CMAKE_CURRENT_BINARY_DIR}/phyriad_schema
    )
endif()
if(NOT TARGET phyriad_hal)
    add_subdirectory(
        ${CMAKE_CURRENT_SOURCE_DIR}/../hal
        ${CMAKE_CURRENT_BINARY_DIR}/phyriad_hal
    )
endif()

# ── Library ───────────────────────────────────────────────────────────────────
add_library(phyriad_etw STATIC
    src/SessionManager.cpp
)

target_include_directories(phyriad_etw PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(phyriad_etw PUBLIC cxx_std_23)

target_link_libraries(phyriad_etw PUBLIC
    phyriad_schema
    phyriad_hal
)

# Windows-only ETW system libraries.
if(WIN32)
    target_link_libraries(phyriad_etw PUBLIC advapi32 tdh)
endif()

if(MSVC)
    target_compile_options(phyriad_etw PRIVATE /W4)
else()
    target_compile_options(phyriad_etw PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ── Tests ─────────────────────────────────────────────────────────────────────
if(BUILD_TESTING OR PHYRIAD_BUILD_TESTS)
    include(CTest)

    add_executable(phyriad_etw_test
        tests/etw_test.cpp
    )
    target_link_libraries(phyriad_etw_test PRIVATE phyriad_etw)
    target_compile_features(phyriad_etw_test PRIVATE cxx_std_23)
    if(MSVC)
        target_compile_options(phyriad_etw_test PRIVATE /W4)
    else()
        target_compile_options(phyriad_etw_test PRIVATE -Wall -Wextra)
    endif()

    add_test(
        NAME    phyriad_etw_test
        COMMAND phyriad_etw_test
        LABELS  etw
    )
endif()
# Made with my soul - Swately <3
