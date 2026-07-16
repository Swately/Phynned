cmake_minimum_required(VERSION 3.20)
project(phyriad_node LANGUAGES CXX)

# ── phyriad_node — pillar node (INTERFACE — header-only) ───────────────────────
# Node concepts, lifecycle hooks, port types, category concepts,
# Runnable<N>, deducing-this mixins, and coroutine frame allocator.
#
# All headers — no compiled sources.
#
# Headers (INTERFACE):
#   Lifecycle.hpp           — lifecycle hook concepts + try_* dispatch
#   Port.hpp                — Outlet<T>, Inlet<T> type-erased ports
#   Categories.hpp          — Source, Sink, Transform, Splitter, Merger, Actor
#   Node.hpp                — Node<N> aggregator concept + category helpers
#   Runnable.hpp            — Runnable<N>, WrappableNode<N>
#   Mixins.hpp              — Sender<T>, Receiver<T> (deducing-this / CRTP)
#   CoroutineAdapter.hpp    — CoroFrameAllocator, AwaitableTask<T>
#   canonical/              — CanonicalSource, CanonicalTransform (compile-time proofs)
#   NodeAll.hpp             — umbrella include
#
# Direct dependencies:
#   - phyriad_hal      (INTERFACE) — Cacheline.hpp (kDestructivePad for CoroFrameAllocator)
#   - phyriad_schema   (INTERFACE) — Error.hpp, PodMessage.hpp (SampleTick in canonical)
#   - phyriad_transport (PUBLIC)   — Ring.hpp (RingHandle + Ring<T,Cap> in Port.hpp)

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

if(NOT TARGET phyriad_transport)
    add_subdirectory(
        ${CMAKE_CURRENT_SOURCE_DIR}/../transport
        ${CMAKE_CURRENT_BINARY_DIR}/phyriad_transport
    )
endif()

# ── INTERFACE target (header-only) ───────────────────────────────────────────
add_library(phyriad_node INTERFACE)

target_include_directories(phyriad_node INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(phyriad_node INTERFACE cxx_std_23)

target_link_libraries(phyriad_node INTERFACE
    phyriad_hal
    phyriad_schema
    phyriad_transport
)

# ── Tests ─────────────────────────────────────────────────────────────────────
if(BUILD_TESTING OR PHYRIAD_BUILD_TESTS)
    include(CTest)
    find_package(Threads REQUIRED)

    add_executable(node_types_test
        tests/node_types_test.cpp
    )
    target_link_libraries(node_types_test PRIVATE
        phyriad_node
        Threads::Threads
    )
    target_compile_features(node_types_test PRIVATE cxx_std_23)

    add_test(
        NAME    node_types_test
        COMMAND node_types_test
        LABELS  node
    )
endif()
# Made with my soul - Swately <3
