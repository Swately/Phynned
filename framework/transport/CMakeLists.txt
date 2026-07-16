cmake_minimum_required(VERSION 3.20)
project(phyriad_transport LANGUAGES CXX)

# ── phyriad_transport — pillar transport (STATIC) ───────────────────────────────
# Lock-free in-process channels: Ring<T,Cap> SWMR, Latest<T> seqlock,
# SIMD slot copy dispatch, and ring wait policies.
#
# Headers (INTERFACE, all header-only except SlotCopy):
#   Transport.hpp       — Transport<Impl,Msg> concept, LatencyClass
#   Ring.hpp            — Ring<T,Cap> Disruptor-style SWMR, RingHandle
#   Latest.hpp          — Latest<T> seqlock SWMR single-slot
#   SlotCopy.hpp        — SlotCopyFn, SlotCopyMode, pick_slot_copy()
#   RingWaitPolicy.hpp  — RingWaitBusySpin/Pause/Yield/Backoff/Sleeping
#   TransportAll.hpp    — umbrella include
#
# Compiled sources:
#   src/SlotCopy.cpp    — SIMD copy implementations + CPUID dispatch
#
# Dependencias directas:
#   - phyriad_schema (INTERFACE) — Error.hpp, PodMessage.hpp, SchemaHash.hpp
#   - phyriad_hal    (INTERFACE) — Cacheline.hpp, MemoryOrder.hpp, Prefetch.hpp, Arch.hpp

# ── Resolución de dependencias en build standalone ────────────────────────────
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
set(TRANSPORT_SOURCES
    src/SlotCopy.cpp
)

add_library(phyriad_transport STATIC ${TRANSPORT_SOURCES})

target_include_directories(phyriad_transport PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(phyriad_transport PUBLIC cxx_std_23)

target_link_libraries(phyriad_transport PUBLIC
    phyriad_schema
    phyriad_hal
)

# ── AVX2/AVX-512 compile flags for SlotCopy.cpp ──────────────────────────────
# We apply these to the whole target; only SlotCopy.cpp uses AVX intrinsics.
# Using target_compile_options with PRIVATE so they don't leak to dependents.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # Check what the compiler supports
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag(-mavx2   COMPILER_HAS_AVX2)
    check_cxx_compiler_flag(-mavx512f COMPILER_HAS_AVX512F)

    if(COMPILER_HAS_AVX2)
        target_compile_options(phyriad_transport PRIVATE -mavx2)
    endif()
    # AVX-512 — enabled when the toolchain supports it.
    #   NOTE: SlotCopy.cpp gates the AVX-512 body with `#if defined(__AVX512F__)`
    #   (a file-level preprocessor guard), NOT a per-function
    #   `__attribute__((target("avx512f")))`. So this `-mavx512f` flag raises the
    #   ISA for the WHOLE translation unit — the compiler may legally emit
    #   AVX-512 in any function here (verified 2026-05-20, V3-P14: slot_copy_avx512
    #   does emit real `vmovdqu64 %zmm`). That is acceptable because the build
    #   targets machines whose CPUID advertises AVX-512; pick_slot_copy() gates
    #   *which* copy variant is selected at runtime via CPUID, but it does NOT
    #   make a -mavx512f-built TU safe on an AVX2-only CPU. If a single binary
    #   must run on both, compile this TU at -mavx2 and add a per-function target
    #   attribute to slot_copy_avx512.
    # Matches legacy `-march=native` perf (Zen4 7950X3D, Ice Lake+, SPR have
    # no AVX-512 frequency penalty; bench_ring_spsc legacy = 462 M msg/s @ 128).
    if(COMPILER_HAS_AVX512F)
        target_compile_options(phyriad_transport PRIVATE -mavx512f)
    endif()
elseif(MSVC)
    target_compile_options(phyriad_transport PRIVATE /arch:AVX2)
    # NOTE: /arch:AVX512 cannot be enabled safely target-wide on MSVC — it
    # raises the *baseline* ISA for the whole TU, so MSVC would emit AVX-512
    # in scalar/AVX2 paths too (no per-function target attribute equivalent),
    # crashing with SIGILL on CPUs without AVX-512 at runtime. AVX-512 fast
    # path is therefore GCC/Clang-only until MSVC gains __declspec(target).
endif()

# ── Tests ─────────────────────────────────────────────────────────────────────
if(BUILD_TESTING OR PHYRIAD_BUILD_TESTS)
    include(CTest)
    find_package(Threads REQUIRED)

    add_executable(transport_types_test
        tests/transport_types_test.cpp
    )
    target_link_libraries(transport_types_test PRIVATE
        phyriad_transport
        Threads::Threads
    )
    target_compile_features(transport_types_test PRIVATE cxx_std_23)

    add_test(
        NAME    transport_types_test
        COMMAND transport_types_test
        LABELS  transport
    )

    # ── Phase 4: RingChannel restored-feature test ──────────────────────────
    add_executable(ring_channel_test tests/ring_channel_test.cpp)
    target_link_libraries(ring_channel_test PRIVATE phyriad_transport Threads::Threads)
    target_compile_features(ring_channel_test PRIVATE cxx_std_23)
    add_test(
        NAME    ring_channel_test
        COMMAND ring_channel_test
        LABELS  transport
    )
endif()
# Made with my soul - Swately <3
