# catalog/cpp/render/CMakeLists.txt
# phyriad_render — header-only render abstraction layer.
#
# Provides: IRenderBackend, FrameArena, RenderStats, RenderBackendKind,
#           MockRenderBackend.  All are pure C++23 headers; no source files.
#
# The concrete OpenGL3 backend lives in the opengl3/ subdirectory and is
# added when the necessary GLFW / ImGui targets are already defined (either
# by the top-level build or by a standalone FetchContent below).
#
cmake_minimum_required(VERSION 3.20)
project(phyriad_render LANGUAGES CXX)

# ── Pillar dependencies (standalone guard) ────────────────────────────────────
if(NOT TARGET phyriad_schema)
    add_subdirectory(../schema ${CMAKE_CURRENT_BINARY_DIR}/schema)
endif()
if(NOT TARGET phyriad_hal)
    add_subdirectory(../hal ${CMAKE_CURRENT_BINARY_DIR}/hal)
endif()

# ── phyriad_render — INTERFACE (header-only) ────────────────────────────────────
add_library(phyriad_render INTERFACE)
target_include_directories(phyriad_render INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(phyriad_render INTERFACE cxx_std_23)
target_link_libraries(phyriad_render INTERFACE phyriad_schema phyriad_hal)

# ── OpenGL3 backend ───────────────────────────────────────────────────────────
# Added when GLFW + ImGui targets exist (provided by top-level or FetchContent).
# For standalone builds of this pillar, fetch them here if missing.
if(NOT TARGET glfw OR NOT TARGET imgui_lib)
    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR
       OR CMAKE_SOURCE_DIR STREQUAL "${CMAKE_CURRENT_SOURCE_DIR}/..")
        # Standalone or render-as-root: fetch dependencies ourselves.
        include(FetchContent)

        if(NOT TARGET glfw)
            FetchContent_Declare(glfw
                GIT_REPOSITORY https://github.com/glfw/glfw.git
                GIT_TAG        3.4)
            set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
            set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
            set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
            set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
            FetchContent_MakeAvailable(glfw)
        endif()

        if(NOT TARGET imgui_lib)
            FetchContent_Declare(imgui
                GIT_REPOSITORY https://github.com/ocornut/imgui.git
                GIT_TAG        v1.91.5-docking)
            FetchContent_MakeAvailable(imgui)

            add_library(imgui_lib STATIC
                ${imgui_SOURCE_DIR}/imgui.cpp
                ${imgui_SOURCE_DIR}/imgui_draw.cpp
                ${imgui_SOURCE_DIR}/imgui_tables.cpp
                ${imgui_SOURCE_DIR}/imgui_widgets.cpp
                ${imgui_SOURCE_DIR}/imgui_demo.cpp
                ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
                ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
            )
            target_include_directories(imgui_lib PUBLIC
                ${imgui_SOURCE_DIR}
                ${imgui_SOURCE_DIR}/backends)
            target_link_libraries(imgui_lib PUBLIC glfw)
            if(WIN32)
                target_link_libraries(imgui_lib PUBLIC opengl32)
            endif()
        endif()
    endif()
endif()

if(TARGET glfw AND TARGET imgui_lib)
    add_subdirectory(opengl3)
endif()

# ── Vulkan backend ────────────────────────────────────────────────────────────
# Added only when PHYRIAD_BUILD_VULKAN=ON and the SDK is present.
# The subdirectory itself calls find_package(Vulkan QUIET) and returns early
# if the SDK is not found, so we guard at both levels.
if(PHYRIAD_BUILD_VULKAN AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/vulkan/CMakeLists.txt)
    add_subdirectory(vulkan)
endif()

# ── Composite backend ─────────────────────────────────────────────────────────
# CompositeBackend composites the Vulkan scene with the OpenGL3 UI layer.
# Requires phyriad_render_opengl3 (always) + phyriad_render_vulkan (when available).
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/composite/CMakeLists.txt
   AND TARGET phyriad_render_opengl3)
    add_subdirectory(composite)
endif()

# ── Tests ─────────────────────────────────────────────────────────────────────
if((BUILD_TESTING OR PHYRIAD_BUILD_TESTS) AND
    EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests AND
    NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/.empty)
    file(GLOB render_tests CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
    if(render_tests)
        add_executable(render_test ${render_tests})
        target_link_libraries(render_test PRIVATE phyriad_render)
        target_compile_features(render_test PRIVATE cxx_std_23)
        include(CTest)
        add_test(NAME render_test COMMAND render_test)
    endif()
endif()
# Made with my soul - Swately <3
