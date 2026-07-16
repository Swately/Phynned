# catalog/cpp/ui/CMakeLists.txt
# phyriad_ui        — header-only phyriad::ui application layer.
# phyriad_ui_compiled — compiled sources: FrameInterpolatorNode, PresentNode.
#
# Provides: UIThreadNode, RenderNode<States...>, Application,
#           ApplicationConfig, glfw_callbacks, WindowState, InputEvent,
#           FrameInterpolatorNode, PresentNode.
#
cmake_minimum_required(VERSION 3.20)
project(phyriad_ui LANGUAGES CXX)

# ── Pillar dependencies (standalone guard) ────────────────────────────────────
if(NOT TARGET phyriad_schema)
    add_subdirectory(../schema ${CMAKE_CURRENT_BINARY_DIR}/schema)
endif()
if(NOT TARGET phyriad_hal)
    add_subdirectory(../hal ${CMAKE_CURRENT_BINARY_DIR}/hal)
endif()
if(NOT TARGET phyriad_node)
    add_subdirectory(../node ${CMAKE_CURRENT_BINARY_DIR}/node)
endif()
if(NOT TARGET phyriad_render)
    add_subdirectory(../render ${CMAKE_CURRENT_BINARY_DIR}/render)
endif()
if(NOT TARGET phyriad_graph)
    add_subdirectory(../graph ${CMAKE_CURRENT_BINARY_DIR}/graph)
endif()
if(NOT TARGET phyriad_runtime)
    add_subdirectory(../runtime ${CMAKE_CURRENT_BINARY_DIR}/runtime)
endif()
if(NOT TARGET phyriad_topology)
    add_subdirectory(../topology ${CMAKE_CURRENT_BINARY_DIR}/topology)
endif()
# phyriad_pool dep removed by Problem 2 of PILLAR_COMPOSABILITY_AUDIT: the
# PoolMetricsBridge + PoolMetricsMsg pair lives on pool's side now
# (catalog/cpp/pool/include/phyriad/pool/bridges/). Apps that need it link
# phyriad_pool themselves and pull in those headers directly. The ui pillar
# stays free of pool transitively.

# ── phyriad_ui — INTERFACE (header-only) ───────────────────────────────────────
add_library(phyriad_ui INTERFACE)
target_include_directories(phyriad_ui INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(phyriad_ui INTERFACE cxx_std_23)

target_link_libraries(phyriad_ui INTERFACE
    phyriad_schema
    phyriad_hal
    phyriad_node
    phyriad_render
    phyriad_graph
    phyriad_runtime
    phyriad_topology)

# phyriad_render_opengl3 is linked when available (OpenGL3 backend).
if(TARGET phyriad_render_opengl3)
    target_link_libraries(phyriad_ui INTERFACE phyriad_render_opengl3)
endif()

# phyriad_render_vulkan is linked when PHYRIAD_BUILD_VULKAN=ON and the Vulkan SDK
# was found (the target may not exist if the SDK is absent even with the flag).
# Linking it also propagates the PHYRIAD_BUILD_VULKAN compile definition, which
# gates the #include <phyriad/render/vulkan/VulkanBackend.hpp> in Application.hpp.
if(PHYRIAD_BUILD_VULKAN AND TARGET phyriad_render_vulkan)
    target_link_libraries(phyriad_ui INTERFACE phyriad_render_vulkan)
endif()

# GLFW headers are needed by UIThreadNode.hpp / glfw_callbacks.hpp.
if(TARGET glfw)
    target_link_libraries(phyriad_ui INTERFACE glfw)
endif()

# ImGui headers are needed by the public Application.hpp (#include <imgui.h>).
# phyriad_render_opengl3 links imgui_lib PRIVATE, so its include path is NOT
# propagated to phyriad_ui consumers — we must export it here ourselves, or
# anything that includes <phyriad/ui/Application.hpp> fails with
# "imgui.h: No such file or directory".
if(TARGET imgui_lib)
    target_link_libraries(phyriad_ui INTERFACE imgui_lib)
endif()

# Windows-only native dependencies for Application::run()'s GPU-driver
# error dialog (MessageBoxW + ShellExecuteW). MinGW gcc auto-links these
# from <windows.h> / <shellapi.h> include-pragmas, but listing them
# explicitly is the portable form that also satisfies MSVC.
if(WIN32)
    target_link_libraries(phyriad_ui INTERFACE user32 shell32)
endif()

# ── phyriad_ui_compiled — compiled node implementations ───────────────────────
# FrameInterpolatorNode and PresentNode require compiled translation units.
# We keep phyriad_ui as INTERFACE (header-only) and expose a companion STATIC
# target that consumers can opt into when they need the compiled nodes.
set(_ui_compiled_sources "")
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/FrameInterpolatorNode.cpp)
    list(APPEND _ui_compiled_sources src/FrameInterpolatorNode.cpp)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/PresentNode.cpp)
    list(APPEND _ui_compiled_sources src/PresentNode.cpp)
endif()

if(_ui_compiled_sources)
    add_library(phyriad_ui_compiled STATIC ${_ui_compiled_sources})
    target_compile_features(phyriad_ui_compiled PUBLIC cxx_std_23)

    # Inherit all phyriad_ui includes and transitive deps.
    target_link_libraries(phyriad_ui_compiled PUBLIC phyriad_ui)

    # Also link phyriad_render_vulkan when Vulkan is available — PresentNode and
    # FrameInterpolatorNode use FramePool, Swapchain, VkFence, etc.
    if(PHYRIAD_BUILD_VULKAN AND TARGET phyriad_render_vulkan)
        target_link_libraries(phyriad_ui_compiled PUBLIC phyriad_render_vulkan)
    endif()

    if(MSVC)
        target_compile_options(phyriad_ui_compiled PRIVATE
            /W4 /wd4100 /wd4458)
    else()
        target_compile_options(phyriad_ui_compiled PRIVATE -Wall -Wextra)
    endif()
endif()

# ── Tests ─────────────────────────────────────────────────────────────────────
if(BUILD_TESTING OR PHYRIAD_BUILD_TESTS)
    include(CTest)

    # ── Compile-time header validation (POD layout, enum values, defaults) ─────
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/ui_compile_test.cpp)
        add_executable(ui_compile_test
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/ui_compile_test.cpp)
        target_link_libraries(ui_compile_test PRIVATE phyriad_ui)
        target_compile_features(ui_compile_test PRIVATE cxx_std_23)
        if(CMAKE_TESTING_ENABLED)
            add_test(NAME ui_compile_test COMMAND ui_compile_test)
        endif()
    endif()

    # ── Dynamic backend selection logic ───────────────────────────────────────
    # This test includes OpenGL3Backend.hpp directly, so it requires the
    # opengl3 render target to exist (gated off when STANDALONE).
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/render_backend_test.cpp
       AND TARGET phyriad_render_opengl3)
        add_executable(render_backend_test
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/render_backend_test.cpp)
        target_link_libraries(render_backend_test PRIVATE phyriad_ui)
        target_compile_features(render_backend_test PRIVATE cxx_std_23)
        if(MSVC)
            target_compile_options(render_backend_test PRIVATE /W4 /wd4100)
        else()
            target_compile_options(render_backend_test PRIVATE -Wall -Wextra)
        endif()
        if(CMAKE_TESTING_ENABLED)
            add_test(NAME render_backend_test COMMAND render_backend_test)
        endif()
    endif()

    # ── render_node_test — RenderNode invariants ──────────────────────────
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/render_node_test.cpp)
        add_executable(render_node_test
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/render_node_test.cpp)
        target_link_libraries(render_node_test PRIVATE phyriad_ui)
        target_compile_features(render_node_test PRIVATE cxx_std_23)
        if(MSVC)
            target_compile_options(render_node_test PRIVATE /W4 /wd4100)
        else()
            target_compile_options(render_node_test PRIVATE -Wall -Wextra)
        endif()
        if(CMAKE_TESTING_ENABLED)
            add_test(NAME render_node_test COMMAND render_node_test)
        endif()
    endif()

    # ── frame_interp_test — FrameInterpolatorNode/PresentNode/FramePool ──────
    # Requires phyriad_ui_compiled (provides the compiled node implementations).
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/frame_interp_test.cpp
       AND TARGET phyriad_ui_compiled)
        add_executable(frame_interp_test
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/frame_interp_test.cpp)
        target_compile_features(frame_interp_test PRIVATE cxx_std_23)
        # phyriad_ui_compiled pulls in phyriad_ui + phyriad_render_vulkan transitively.
        target_link_libraries(frame_interp_test PRIVATE phyriad_ui_compiled)
        if(MSVC)
            target_compile_options(frame_interp_test PRIVATE /W4 /wd4100)
        else()
            target_compile_options(frame_interp_test PRIVATE -Wall -Wextra)
        endif()
        if(CMAKE_TESTING_ENABLED)
            add_test(NAME frame_interp_test COMMAND frame_interp_test)
        endif()
    endif()
endif()
# Made with my soul - Swately <3
