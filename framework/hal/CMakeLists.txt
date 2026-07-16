cmake_minimum_required(VERSION 3.20)
project(phyriad_hal LANGUAGES CXX)

# ── phyriad_hal — Hardware Abstraction Layer (INTERFACE — header-only) ───────────
# No produce .a/.lib — solo expone include paths y compile features.
# Es la raíz del árbol de dependencias de Phyriad: ningún pilar puede depender
# de nada más bajo que phyriad_hal.
#
# Pilares que dependen de phyriad_hal (directa o transitivamente via phyriad_behavior):
#   phyriad_behavior, phyriad_transport, phyriad_topology, phyriad_runtime, ...
#
# Uso desde otro pilar:
#   target_link_libraries(phyriad_<pilar> PUBLIC phyriad_hal)
#   # Después: #include <phyriad/hal/Arch.hpp>, <phyriad/hal/Timestamp.hpp>, etc.

add_library(phyriad_hal INTERFACE)

target_include_directories(phyriad_hal INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(phyriad_hal INTERFACE cxx_std_23)

# ── Tests ─────────────────────────────────────────────────────────────────────
if(BUILD_TESTING OR PHYRIAD_BUILD_TESTS)
    include(CTest)
    enable_testing()

    add_executable(hal_types_test
        tests/hal_types_test.cpp
    )
    target_link_libraries(hal_types_test PRIVATE phyriad_hal)
    target_compile_features(hal_types_test PRIVATE cxx_std_23)
    add_test(
        NAME    hal_types_test
        COMMAND hal_types_test
        LABELS  hal
    )

    # ── FR-16: WakeEvent test ─────────────────────────────────────────────────
    add_executable(wake_event_test
        tests/wake_event_test.cpp
    )
    target_link_libraries(wake_event_test PRIVATE phyriad_hal)
    target_compile_features(wake_event_test PRIVATE cxx_std_23)
    if(MSVC)
        target_compile_options(wake_event_test PRIVATE /W4)
        target_link_libraries(wake_event_test PRIVATE kernel32)
    else()
        target_compile_options(wake_event_test PRIVATE -Wall -Wextra -pthread)
        target_link_libraries(wake_event_test PRIVATE pthread)
    endif()
    add_test(
        NAME    wake_event_test
        COMMAND wake_event_test
        LABELS  hal
    )

    # ── Phase O2.1: aligned_alloc_hint + NUMA + hugepages ─────────────────────
    add_executable(allocator_test tests/allocator_test.cpp)
    target_link_libraries(allocator_test PRIVATE phyriad_hal)
    target_compile_features(allocator_test PRIVATE cxx_std_23)
    if(WIN32)
        # advapi32 for OpenProcessToken / LookupPrivilegeValue (large pages).
        target_link_libraries(allocator_test PRIVATE advapi32 kernel32)
    endif()
    add_test(
        NAME    allocator_test
        COMMAND allocator_test
        LABELS  hal
    )

    # ── Phase O6.3: cpu_wait_until + WAITPKG / PAUSE fallback ─────────────────
    add_executable(cpu_wait_test tests/cpu_wait_test.cpp)
    target_link_libraries(cpu_wait_test PRIVATE phyriad_hal)
    target_compile_features(cpu_wait_test PRIVATE cxx_std_23)
    add_test(
        NAME    cpu_wait_test
        COMMAND cpu_wait_test
        LABELS  hal
    )
endif()
# Made with my soul - Swately <3
