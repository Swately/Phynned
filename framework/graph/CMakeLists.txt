# catalog/cpp/graph/CMakeLists.txt
# phyriad::graph + phyriad::api — header-only INTERFACE pillar.
#
# Provides:
#   phyriad::graph   — Graph<G> concept, StaticGraph, DynamicGraph, GraphBuilder,
#                  GraphValidator
#   phyriad::api     — DslGraphBuilder, BuiltGraph, NodeHandle, WireHandle,
#                  NodeRegistry, WireRegistry, placement:: factory helpers
#
# Dependencies (resolved standalone via add_subdirectory fallback):
#   phyriad_hal        — kDestructivePad, kPhyriadHalVersion
#   phyriad_schema     — Hash128, SchemaDescriptor, PodMessage, Error
#   phyriad_transport  — Ring<T,Cap>
#   phyriad_node       — Node<N>, Source<N>, Runnable, Lifecycle, Port<T>
#   phyriad_scheduler  — PlacementHint, PlacementPolicy, Scheduler
#

cmake_minimum_required(VERSION 3.20)
project(phyriad_graph LANGUAGES CXX)

# ── Standalone dependency resolution ─────────────────────────────────────────
if(NOT TARGET phyriad_hal)
    add_subdirectory(../hal "${CMAKE_CURRENT_BINARY_DIR}/hal")
endif()
if(NOT TARGET phyriad_schema)
    add_subdirectory(../schema "${CMAKE_CURRENT_BINARY_DIR}/schema")
endif()
if(NOT TARGET phyriad_transport)
    add_subdirectory(../transport "${CMAKE_CURRENT_BINARY_DIR}/transport")
endif()
if(NOT TARGET phyriad_node)
    add_subdirectory(../node "${CMAKE_CURRENT_BINARY_DIR}/node")
endif()
if(NOT TARGET phyriad_scheduler)
    add_subdirectory(../scheduler "${CMAKE_CURRENT_BINARY_DIR}/scheduler")
endif()

# ── Library (INTERFACE — header-only) ─────────────────────────────────────────
add_library(phyriad_graph INTERFACE)

target_include_directories(phyriad_graph INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(phyriad_graph INTERFACE cxx_std_23)

target_link_libraries(phyriad_graph INTERFACE
    phyriad_scheduler
    phyriad_node
    phyriad_transport
    phyriad_schema
    phyriad_hal
)

# ── Platform-specific link requirements ──────────────────────────────────────
if(WIN32)
    target_link_libraries(phyriad_graph INTERFACE kernel32)
endif()

if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(phyriad_graph INTERFACE Threads::Threads rt)
endif()

# ── Tests ─────────────────────────────────────────────────────────────────────
if(BUILD_TESTING OR PHYRIAD_BUILD_TESTS)
    include(CTest)
    enable_testing()
    add_executable(graph_test
        tests/graph_test.cpp
    )
    target_link_libraries(graph_test PRIVATE phyriad_graph)
    target_compile_features(graph_test PRIVATE cxx_std_23)

    if(WIN32)
        target_link_libraries(graph_test PRIVATE kernel32)
    endif()

    add_test(NAME graph_test COMMAND graph_test)
endif()
# Made with my soul - Swately <3
