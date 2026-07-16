# catalog/cpp/stigmergy/CMakeLists.txt
# phyriad_stigmergy — INTERFACE library (header-only) exposing the
# stigmergic-coordination primitives as a public API of Phyriad.
#
# Provides three primitives + one base class:
#   phyriad::stigmergy::Field<T>          — shared observable seqlock cell
#   phyriad::stigmergy::Pheromone<T, N>   — N-deep agent-indexed deposits
#   phyriad::stigmergy::Classifier<S, A>  — emergent decision policy ABC
#   phyriad::stigmergy::Worker<F, P, L>   — autonomous agent loop (optional)
#
# Internally wraps `transport::Latest<T>` and `std::atomic<T>` to be a
# zero-overhead abstraction. The performance contract is documented in
# (Stigmergy pillar — see docs/pillars/STIGMERGY.md)
# and verified by `bench_stigmergy_primitives`.
#
# Dependencies (transitively reachable; listed explicitly for clarity):
#   phyriad_hal       — kDestructivePad, memory-order wrappers
#   phyriad_transport — Latest<T> (wrapped by Field<T>)
#   phyriad_schema    — PodMessage<T> concept (constraint on Field<T> value)
#   phyriad_topology  — hw::pin_current_thread (used by Worker<L>::start)
#
# Stigmergy as first-class pillar.
cmake_minimum_required(VERSION 3.20)
project(phyriad_stigmergy LANGUAGES CXX)

# ── Standalone dependency resolution ─────────────────────────────────────────
# When this pillar is built standalone (e.g. as part of a sub-tree build),
# pull its dependencies if not already part of the parent build.
if(NOT TARGET phyriad_hal)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../hal
                     ${CMAKE_CURRENT_BINARY_DIR}/phyriad_hal)
endif()
if(NOT TARGET phyriad_schema)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../schema
                     ${CMAKE_CURRENT_BINARY_DIR}/phyriad_schema)
endif()
if(NOT TARGET phyriad_transport)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../transport
                     ${CMAKE_CURRENT_BINARY_DIR}/phyriad_transport)
endif()
# Problem 6 of PILLAR_COMPOSABILITY_AUDIT: `Worker::start` now accepts an
# injected pin function; consumers wanting pinning link `phyriad_topology`
# themselves. Stigmergy stays truly standalone.

# ── Library (INTERFACE — header-only) ────────────────────────────────────────
add_library(phyriad_stigmergy INTERFACE)

target_include_directories(phyriad_stigmergy INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(phyriad_stigmergy INTERFACE cxx_std_23)

target_link_libraries(phyriad_stigmergy INTERFACE
    phyriad_hal
    phyriad_schema
    phyriad_transport
)

# ── Tests ─────────────────────────────────────────────────────────────────────
if(BUILD_TESTING OR PHYRIAD_BUILD_TESTS)
    include(CTest)
    enable_testing()

    foreach(_test field pheromone classifier integration)
        add_executable(stigmergy_${_test}_test
            tests/stigmergy_${_test}_test.cpp
        )
        target_link_libraries(stigmergy_${_test}_test PRIVATE phyriad_stigmergy)
        target_compile_features(stigmergy_${_test}_test PRIVATE cxx_std_23)
        if(WIN32)
            target_link_libraries(stigmergy_${_test}_test PRIVATE kernel32)
        endif()
        add_test(
            NAME    stigmergy_${_test}_test
            COMMAND stigmergy_${_test}_test
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        )
    endforeach()
endif()
# Made with my soul - Swately <3
