# catalog/cpp/scheduler/CMakeLists.txt
# V-Cache/NUMA/PCIe-aware placement engine — STATIC pillar.
#
# Dependencies (resolved stand-alone via add_subdirectory fallback):
#   phyriad_hal       — HAL atomics / cache-line constants
#   phyriad_topology  — HardwareTopology, CoreInfo, PCIeAffinityEntry
#

cmake_minimum_required(VERSION 3.20)
project(phyriad_scheduler LANGUAGES CXX)

# ── Standalone dependency resolution ─────────────────────────────────────────
if(NOT TARGET phyriad_hal)
    add_subdirectory(../hal "${CMAKE_CURRENT_BINARY_DIR}/hal")
endif()
if(NOT TARGET phyriad_topology)
    add_subdirectory(../topology "${CMAKE_CURRENT_BINARY_DIR}/topology")
endif()

# ── Library ───────────────────────────────────────────────────────────────────
add_library(phyriad_scheduler STATIC
    src/Scheduler.cpp
)

target_include_directories(phyriad_scheduler PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(phyriad_scheduler PUBLIC cxx_std_23)

target_link_libraries(phyriad_scheduler PUBLIC
    phyriad_topology
    phyriad_hal
)

# ── Platform-specific link requirements ──────────────────────────────────────
if(WIN32)
    target_link_libraries(phyriad_scheduler PUBLIC kernel32)
endif()

if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(phyriad_scheduler PUBLIC Threads::Threads)
endif()

# ── Tests ─────────────────────────────────────────────────────────────────────
if(BUILD_TESTING OR PHYRIAD_BUILD_TESTS)
    include(CTest)
    enable_testing()
    add_executable(scheduler_test
        tests/scheduler_test.cpp
    )
    target_link_libraries(scheduler_test PRIVATE phyriad_scheduler)
    target_compile_features(scheduler_test PRIVATE cxx_std_23)

    if(WIN32)
        target_link_libraries(scheduler_test PRIVATE kernel32)
    endif()

    add_test(NAME scheduler_test COMMAND scheduler_test)

    # CCDAwarePlacement test
    add_executable(ccd_placement_test
        tests/ccd_placement_test.cpp
    )
    target_link_libraries(ccd_placement_test PRIVATE phyriad_scheduler)
    target_compile_features(ccd_placement_test PRIVATE cxx_std_23)

    if(WIN32)
        target_link_libraries(ccd_placement_test PRIVATE kernel32)
    endif()

    add_test(NAME ccd_placement_test COMMAND ccd_placement_test)
endif()
# Made with my soul - Swately <3
