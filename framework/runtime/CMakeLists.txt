# catalog/cpp/runtime/CMakeLists.txt
# phyriad::runtime — GraphRuntime STATIC pillar.
#
# Provides:
#   phyriad::runtime  — GraphRuntime, PerformanceProfile, make_auto_profile(),
#                   apply_profile_hints(), run_graph helpers
#
# Dependencies (resolved standalone via add_subdirectory fallback):
#   phyriad_graph    — BuiltGraph, NodeRegistry, WireRegistry, NodeHandle, WireHandle
#   phyriad_topology — HardwareTopology (for make_auto_profile)
#   phyriad_scheduler — Scheduler::compute_placement() in GraphRuntime.cpp
#   phyriad_hal      — hal::ctrl_*, hal::stat_* in Heartbeat.hpp / GraphRuntime.cpp
#   phyriad_profile  — PerformanceProfile (after Problem 3 extraction)
#   phyriad_schema   — schema::Error/ErrorCode in GraphRuntime.hpp / run_graph.hpp
#

cmake_minimum_required(VERSION 3.20)
project(phyriad_runtime LANGUAGES CXX)

# ── Standalone dependency resolution ─────────────────────────────────────────
if(NOT TARGET phyriad_graph)
    add_subdirectory(../graph "${CMAKE_CURRENT_BINARY_DIR}/graph")
endif()

# phyriad_topology is already pulled in transitively via phyriad_graph →
# phyriad_scheduler → phyriad_topology, but guard it explicitly so standalone
# builds work even if the chain is not yet established.
if(NOT TARGET phyriad_topology)
    add_subdirectory(../topology "${CMAKE_CURRENT_BINARY_DIR}/topology")
endif()

# phyriad_scheduler is needed directly by GraphRuntime.cpp for the
# Scheduler::compute_placement() call. Transitively reachable via phyriad_graph
# but we link it explicitly so the dependency is clear in the build graph.
if(NOT TARGET phyriad_scheduler)
    add_subdirectory(../scheduler "${CMAKE_CURRENT_BINARY_DIR}/scheduler")
endif()

# phyriad_hal is the cache-line + memory-order layer used by Heartbeat.hpp and
# GraphRuntime.cpp (hal::ctrl_*, hal::stat_*). Transitively reachable but
# explicit link makes the dependency unambiguous.
if(NOT TARGET phyriad_hal)
    add_subdirectory(../hal "${CMAKE_CURRENT_BINARY_DIR}/hal")
endif()

# phyriad_profile — owns PerformanceProfile (extracted from this pillar by
# Problem 3 of PILLAR_COMPOSABILITY_AUDIT). The legacy
# `<phyriad/runtime/PerformanceProfile.hpp>` header in this pillar is a thin
# shim that re-exports the profile namespace; consumers of phyriad_runtime
# inherit phyriad_profile transitively through this PUBLIC link.
if(NOT TARGET phyriad_profile)
    add_subdirectory(../profile "${CMAKE_CURRENT_BINARY_DIR}/profile")
endif()

# phyriad_schema is used directly by GraphRuntime.hpp + run_graph.hpp
# (schema::Error / ErrorCode return-channel types). Transitively reachable via
# phyriad_graph but listed explicitly so the dependency is unambiguous.
if(NOT TARGET phyriad_schema)
    add_subdirectory(../schema "${CMAKE_CURRENT_BINARY_DIR}/schema")
endif()

# ── Library (STATIC) ──────────────────────────────────────────────────────────
add_library(phyriad_runtime STATIC
    src/GraphRuntime.cpp
)

target_include_directories(phyriad_runtime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(phyriad_runtime PUBLIC cxx_std_23)

target_link_libraries(phyriad_runtime PUBLIC
    phyriad_graph
    phyriad_scheduler
    phyriad_topology
    phyriad_profile
    phyriad_hal
    phyriad_schema
)

# ── Platform-specific link requirements ──────────────────────────────────────
if(WIN32)
    target_link_libraries(phyriad_runtime PUBLIC kernel32)
endif()

if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(phyriad_runtime PUBLIC Threads::Threads rt)
endif()

# ── Tests ─────────────────────────────────────────────────────────────────────
if(BUILD_TESTING OR PHYRIAD_BUILD_TESTS)
    include(CTest)
    enable_testing()
    add_executable(runtime_test
        tests/runtime_test.cpp
    )
    target_link_libraries(runtime_test PRIVATE phyriad_runtime)
    target_compile_features(runtime_test PRIVATE cxx_std_23)

    if(WIN32)
        target_link_libraries(runtime_test PRIVATE kernel32)
    endif()

    add_test(NAME runtime_test COMMAND runtime_test)

    # ── Phase 4: Heartbeat restored-feature test ───────────────────────────
    add_executable(heartbeat_test tests/heartbeat_test.cpp)
    target_link_libraries(heartbeat_test PRIVATE phyriad_runtime)
    target_compile_features(heartbeat_test PRIVATE cxx_std_23)
    if(WIN32)
        target_link_libraries(heartbeat_test PRIVATE kernel32)
    endif()
    add_test(NAME heartbeat_test COMMAND heartbeat_test)

    # PerformanceProfile preset / auto-tuning test MOVED to its canonical pillar
    # home: catalog/cpp/profile/tests/performance_profile_test.cpp (W.4). The runtime
    # re-export shim (<phyriad/runtime/PerformanceProfile.hpp>) is compile-tested by
    # this pillar's own use of phyriad::runtime::PerformanceProfile.
endif()
# Made with my soul - Swately <3
