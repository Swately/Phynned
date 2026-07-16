cmake_minimum_required(VERSION 3.20)
project(phyriad_topology LANGUAGES CXX)

# ── phyriad_topology — pillar topology (STATIC) ─────────────────────────────────
# Hardware topology detection: cores, NUMA, cache, ISA features, GPU,
# V-Cache detection, Intel P/E-core classification, PCIe NUMA affinity.
#
# Headers (PUBLIC):
#   CacheInfo.hpp           — L1/L2/L3 cache descriptor (CPUID)
#   CpuFeatures.hpp         — X86Features bitmask (SSE4, AVX2, AVX-512, …)
#   VCacheDetector.hpp      — AMD 3D V-Cache logical ID list
#   HybridCoreDetector.hpp  — Intel P/E-core CoreTypeEntry list
#   PCIeAffinityProbe.hpp   — PCIe device NUMA affinity map
#   HardwareTopology.hpp    — aggregate HardwareTopology + hw:: API
#
# Compiled sources:
#   src/CacheInfo.cpp           — CPUID L1/L2/L3 probing
#   src/VCacheDetector.cpp      — AMD 3D V-Cache detection
#   src/HybridCoreDetector.cpp  — Intel hybrid P/E-core detection
#   src/PCIeAffinityProbe.cpp   — PCIe NUMA affinity (sysfs / Registry)
#   src/HardwareTopology.cpp    — probe(), enumerate_cores(), hw:: functions
#
# Direct dependencies:
#   - phyriad_hal (INTERFACE) — Arch.hpp, Cacheline.hpp, MemoryOrder.hpp
#
# NOTE: make_auto_profile() and apply_profile_hints() are NOT here —
# they depend on PerformanceProfile and live in catalog/cpp/runtime/.

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

# ── Sources ──────────────────────────────────────────────────────────────────
set(TOPOLOGY_SOURCES
    src/CacheInfo.cpp
    src/VCacheDetector.cpp
    src/HybridCoreDetector.cpp
    src/PCIeAffinityProbe.cpp
    src/HardwareTopology.cpp
)

add_library(phyriad_topology STATIC ${TOPOLOGY_SOURCES})

target_include_directories(phyriad_topology PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(phyriad_topology PUBLIC cxx_std_23)

target_link_libraries(phyriad_topology PUBLIC
    phyriad_hal
    phyriad_schema
)

# ── Platform-specific system libraries ───────────────────────────────────────
if(WIN32)
    # kernel32: GetLogicalProcessorInformationEx, GetNumaHighestNodeNumber,
    #           GetNumaAvailableMemoryNodeEx, SetThreadAffinityMask, etc.
    # avrt:     THREAD_PROTECTION S2 — MMCSS (AvSetMmThreadCharacteristics /
    #           AvSetMmThreadPriority / AvRevertMmThreadCharacteristics).
    target_link_libraries(phyriad_topology PUBLIC kernel32 avrt)
endif()

if(UNIX AND NOT APPLE)
    # pthread: pthread_setaffinity_np, pthread_setschedparam
    find_package(Threads REQUIRED)
    target_link_libraries(phyriad_topology PUBLIC Threads::Threads)
endif()

# ── Tests ─────────────────────────────────────────────────────────────────────
if(BUILD_TESTING OR PHYRIAD_BUILD_TESTS)
    include(CTest)
    find_package(Threads REQUIRED)

    add_executable(topology_test
        tests/topology_test.cpp
    )
    target_link_libraries(topology_test PRIVATE
        phyriad_topology
        Threads::Threads
    )
    target_compile_features(topology_test PRIVATE cxx_std_23)

    add_test(
        NAME    topology_test
        COMMAND topology_test
        LABELS  topology
    )

    # GFR-Ayama-2 + GFR-Ayama-4 — thread affinity and ideal-processor APIs.
    add_executable(thread_affinity_test
        tests/thread_affinity_test.cpp
    )
    target_link_libraries(thread_affinity_test PRIVATE
        phyriad_topology
        Threads::Threads
    )
    target_compile_features(thread_affinity_test PRIVATE cxx_std_23)

    add_test(
        NAME    thread_affinity_test
        COMMAND thread_affinity_test
        LABELS  topology
    )
endif()
# Made with my soul - Swately <3
