cmake_minimum_required(VERSION 3.20)
project(phyriad_ipc LANGUAGES CXX)

# ── phyriad_ipc — pillar ipc (INTERFACE / header-only template library) ─────────
# IPC primitives: typed shared-memory regions + SPSC lock-free ring buffer.
#
# Public headers:
#   ShmRegion.hpp — ShmHeaderConcept + ShmRegion<H,P> template         (FR-5)
#   Ring.hpp      — Ring<T, Capacity> SPSC lock-free ring buffer        (FR-12)
#
# Because all types are fully-templated, all implementation lives in the
# headers. There is no compiled source file — the library target is INTERFACE.
#
# Direct dependencies:
#   - phyriad_hal    (INTERFACE) — phyriad::hal::kDestructivePad, kCachelineSize
#   - phyriad_schema (INTERFACE) — phyriad::Error, phyriad::ErrorCode
#
# Platform system libraries (only linked by tests / consumers):
#   Windows: kernel32 (CreateFileMappingW, MapViewOfFile, …)
#   Linux:   -lrt on some distros for shm_open/shm_unlink.
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

# ── INTERFACE library (header-only templates) ─────────────────────────────────
add_library(phyriad_ipc INTERFACE)

target_include_directories(phyriad_ipc INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(phyriad_ipc INTERFACE cxx_std_23)

target_link_libraries(phyriad_ipc INTERFACE
    phyriad_hal
    phyriad_schema
)

# On Linux, shm_open/shm_unlink may require -lrt on older glibc distributions.
if(UNIX AND NOT APPLE)
    find_library(RT_LIBRARY rt)
    if(RT_LIBRARY)
        target_link_libraries(phyriad_ipc INTERFACE ${RT_LIBRARY})
    endif()
endif()

# ── Tests ─────────────────────────────────────────────────────────────────────
if(BUILD_TESTING OR PHYRIAD_BUILD_TESTS)
    include(CTest)

    add_executable(shm_region_test
        tests/shm_region_test.cpp
    )
    target_link_libraries(shm_region_test PRIVATE phyriad_ipc)
    target_compile_features(shm_region_test PRIVATE cxx_std_23)
    if(MSVC)
        target_compile_options(shm_region_test PRIVATE /W4)
    else()
        target_compile_options(shm_region_test PRIVATE -Wall -Wextra)
    endif()
    add_test(
        NAME    shm_region_test
        COMMAND shm_region_test
        LABELS  ipc
    )

    # ── FR-12: Ring<T> test ───────────────────────────────────────────────────
    add_executable(ring_test
        tests/ring_test.cpp
    )
    target_link_libraries(ring_test PRIVATE phyriad_ipc)
    target_compile_features(ring_test PRIVATE cxx_std_23)
    if(MSVC)
        target_compile_options(ring_test PRIVATE /W4)
    else()
        target_compile_options(ring_test PRIVATE -Wall -Wextra -pthread)
        target_link_libraries(ring_test PRIVATE pthread)
    endif()
    add_test(
        NAME    ring_test
        COMMAND ring_test
        LABELS  ipc
    )
endif()
# Made with my soul - Swately <3
