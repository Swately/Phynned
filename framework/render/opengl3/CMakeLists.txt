# catalog/cpp/render/opengl3/CMakeLists.txt
# phyriad_render_opengl3 — OpenGL 3.3 Core Profile render backend.
#
# Compiles OpenGL3Backend.cpp which includes:
#   <imgui.h>, <imgui_impl_glfw.h>, <imgui_impl_opengl3.h>, <GLFW/glfw3.h>
#
# Requires: glfw, imgui_lib — provided by the parent render CMakeLists.txt
# or by the top-level build when PHYRIAD_BUILD_EXAMPLES is ON.
#
cmake_minimum_required(VERSION 3.20)
project(phyriad_render_opengl3 LANGUAGES CXX)

# ── Pillar dependencies (standalone guard) ────────────────────────────────────
if(NOT TARGET phyriad_render)
    add_subdirectory(../.. ${CMAKE_CURRENT_BINARY_DIR}/render)
endif()

# ── phyriad_render_opengl3 ──────────────────────────────────────────────────────
add_library(phyriad_render_opengl3 STATIC
    src/OpenGL3Backend.cpp
    src/FSR1Pass.cpp
    src/ExternalTexture.cpp    # v1.0 — VK↔GL interop (CompositeBackend)
    src/Texture2D.cpp)         # consumer-owned RGBA8 texture for ImGui::Image

target_include_directories(phyriad_render_opengl3 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_compile_features(phyriad_render_opengl3 PUBLIC cxx_std_23)

target_link_libraries(phyriad_render_opengl3
    PUBLIC  phyriad_render
    PRIVATE imgui_lib)

if(WIN32)
    target_link_libraries(phyriad_render_opengl3 PRIVATE opengl32)
endif()

# ── FSR1Pass needs GLFW for glfwGetProcAddress ────────────────────────────────
# GLFW is already available via imgui_lib PUBLIC link, but we make it explicit.
if(TARGET glfw)
    target_link_libraries(phyriad_render_opengl3 PUBLIC glfw)
else()
    find_package(glfw3 QUIET)
    if(glfw3_FOUND)
        target_link_libraries(phyriad_render_opengl3 PUBLIC glfw3::glfw3)
    endif()
endif()

# ── Tests ─────────────────────────────────────────────────────────────────────
if(PHYRIAD_BUILD_TESTS)
    add_executable(fsr1_test tests/fsr1_test.cpp)
    target_compile_features(fsr1_test PRIVATE cxx_std_23)
    target_link_libraries(fsr1_test PRIVATE phyriad_render_opengl3)

    if(MSVC)
        target_compile_options(fsr1_test PRIVATE /W4 /wd4100)
    else()
        target_compile_options(fsr1_test PRIVATE -Wall -Wextra)
    endif()

    if(CMAKE_TESTING_ENABLED)
        add_test(NAME fsr1_test COMMAND fsr1_test)
    endif()
endif()
# Made with my soul - Swately <3
