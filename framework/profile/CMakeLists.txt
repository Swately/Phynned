# catalog/cpp/profile/CMakeLists.txt
# phyriad_profile — performance-profile + autotuning primitive.
#
# Header-only INTERFACE pillar. Owns the cross-pillar configuration object
# `PerformanceProfile` plus `ProfileKind`, `RingWaitMode`, `SlotCopyMode`,
# per-node tick budgets and per-wire ring profiles. Extracted from `runtime`
# by Problem 3 of PILLAR_COMPOSABILITY_AUDIT so that `pool` (and any other
# pillar) can consume it without dragging runtime/graph/scheduler/node.
#
# Deps:
#   phyriad_hal       — Cacheline / Arch (transitive via topology)
#   phyriad_topology  — HardwareTopology argument to make_auto_profile
#   phyriad_scheduler — PlacementHint argument to apply_profile_hints
#
# The legacy include path `<phyriad/runtime/PerformanceProfile.hpp>` survives
# as a thin shim that re-exports these symbols into the `phyriad::runtime`
# namespace (see catalog/cpp/runtime/include/phyriad/runtime/PerformanceProfile.hpp).
cmake_minimum_required(VERSION 3.20)
project(phyriad_profile LANGUAGES CXX)

# ── Standalone dependency resolution ─────────────────────────────────────────
if(NOT TARGET phyriad_hal)
    add_subdirectory(../hal      ${CMAKE_CURRENT_BINARY_DIR}/hal)
endif()
if(NOT TARGET phyriad_topology)
    add_subdirectory(../topology ${CMAKE_CURRENT_BINARY_DIR}/topology)
endif()
if(NOT TARGET phyriad_scheduler)
    add_subdirectory(../scheduler ${CMAKE_CURRENT_BINARY_DIR}/scheduler)
endif()

# ── Library (INTERFACE — header-only) ────────────────────────────────────────
add_library(phyriad_profile INTERFACE)

target_include_directories(phyriad_profile INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(phyriad_profile INTERFACE cxx_std_23)

target_link_libraries(phyriad_profile INTERFACE
    phyriad_hal
    phyriad_topology
    phyriad_scheduler
)

# ── Tests ─────────────────────────────────────────────────────────────────────
# The pillar's canonical test (moved from runtime/tests/ — W.4: a pillar owns its
# own test; the runtime re-export shim is compile-tested by runtime's own usage).
if(BUILD_TESTING OR PHYRIAD_BUILD_TESTS)
    include(CTest)

    add_executable(performance_profile_test tests/performance_profile_test.cpp)
    target_link_libraries(performance_profile_test PRIVATE phyriad_profile)
    target_compile_features(performance_profile_test PRIVATE cxx_std_23)
    if(WIN32)
        target_link_libraries(performance_profile_test PRIVATE kernel32)
    endif()
    add_test(
        NAME    performance_profile_test
        COMMAND performance_profile_test
        LABELS  profile
    )
endif()
# Made with my soul - Swately <3
