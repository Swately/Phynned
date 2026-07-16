# catalog/cpp/render/composite/CMakeLists.txt
# phyriad_render_composite — CompositeBackend: Vulkan scene + OpenGL UI overlay.
#
# Architecture: VK renders the 3-D scene to an offscreen image exported via
#   VK_KHR_external_memory (WIN32 / fd).
#   GL imports that image (GL_EXT_external_objects) and composites ImGui over it.
#
# When PHYRIAD_COMPOSITE_INTEROP_AVAILABLE is not defined (default) the composite
#   path compiles as Vulkan-only or OpenGL-only without the GL↔VK texture share.
#
# Dependencies:
#   phyriad_render_opengl3 — always required (UI/ImGui layer)
#   phyriad_render_vulkan  — required when PHYRIAD_BUILD_VULKAN=ON
#   phyriad_render         — IRenderBackend, RenderStats, FrameArena
#   phyriad_schema         — phyriad::Error, phyriad::ErrorCode
#
cmake_minimum_required(VERSION 3.20)
project(phyriad_render_composite LANGUAGES CXX)

# ── Guard: opengl3 backend must exist ────────────────────────────────────────
if(NOT TARGET phyriad_render_opengl3)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../opengl3/CMakeLists.txt)
        add_subdirectory(../opengl3 ${CMAKE_CURRENT_BINARY_DIR}/opengl3)
    else()
        message(WARNING
            "phyriad_render_composite: phyriad_render_opengl3 not found — skipping")
        return()
    endif()
endif()

if(NOT TARGET phyriad_render)
    add_subdirectory(../../../render ${CMAKE_CURRENT_BINARY_DIR}/render_base)
endif()
if(NOT TARGET phyriad_schema)
    add_subdirectory(../../../schema ${CMAKE_CURRENT_BINARY_DIR}/schema)
endif()

# ── External-memory interop detection ────────────────────────────────────────
# Set PHYRIAD_COMPOSITE_INTEROP_AVAILABLE=1 if both the Vulkan external-memory
# extension headers and OpenGL external-objects extension headers are present.
set(_interop_available 0)
if(PHYRIAD_BUILD_VULKAN AND TARGET phyriad_render_vulkan)
    if(WIN32)
        # VK_KHR_external_memory_win32: shipped with the Vulkan SDK >= 1.1
        # GL_EXT_external_objects_win32: in the OpenGL extension registry headers
        if(EXISTS "${Vulkan_INCLUDE_DIRS}/vulkan/vulkan_win32.h")
            set(_interop_available 1)
            message(STATUS
                "phyriad_render_composite: Windows external-memory interop available")
        endif()
    elseif(UNIX AND NOT APPLE)
        # VK_KHR_external_memory_fd
        if(EXISTS "${Vulkan_INCLUDE_DIRS}/vulkan/vulkan.h")
            set(_interop_available 1)
            message(STATUS
                "phyriad_render_composite: POSIX external-memory interop available")
        endif()
    endif()
endif()

# ── Library ───────────────────────────────────────────────────────────────────
add_library(phyriad_render_composite STATIC
    src/CompositeBackend.cpp)

target_compile_features(phyriad_render_composite PUBLIC cxx_std_23)

target_include_directories(phyriad_render_composite
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_definitions(phyriad_render_composite
    PRIVATE PHYRIAD_COMPOSITE_INTEROP_AVAILABLE=${_interop_available})

target_link_libraries(phyriad_render_composite
    PUBLIC
        phyriad_render          # IRenderBackend, RenderStats, FrameArena
        phyriad_schema          # phyriad::Error
        phyriad_render_opengl3  # OpenGL3Backend (UI layer)
)

if(PHYRIAD_BUILD_VULKAN AND TARGET phyriad_render_vulkan)
    target_link_libraries(phyriad_render_composite PUBLIC phyriad_render_vulkan)
endif()

if(MSVC)
    target_compile_options(phyriad_render_composite PRIVATE
        /W4 /wd4100 /wd4458)
else()
    target_compile_options(phyriad_render_composite PRIVATE -Wall -Wextra)
endif()

# ── Tests ─────────────────────────────────────────────────────────────────────
if(PHYRIAD_BUILD_TESTS OR BUILD_TESTING)
    include(CTest)

    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/composite_test.cpp)
        add_executable(composite_test tests/composite_test.cpp)
        target_compile_features(composite_test PRIVATE cxx_std_23)
        target_link_libraries(composite_test PRIVATE phyriad_render_composite)
        target_compile_definitions(composite_test PRIVATE
            PHYRIAD_COMPOSITE_INTEROP_AVAILABLE=${_interop_available})
        if(MSVC)
            target_compile_options(composite_test PRIVATE /W4 /wd4100)
        else()
            target_compile_options(composite_test PRIVATE -Wall -Wextra)
        endif()
        if(CMAKE_TESTING_ENABLED)
            add_test(NAME composite_test COMMAND composite_test)
        endif()
    endif()

    # ── vk_gl_interop_test — end-to-end VK↔GL shared memory verification ──
    # Stage 3+5 of the CompositeBackend interop work. Allocates a VK
    # ExternalImage, clears it via vkCmdClearColorImage, imports the
    # exported handle into an OpenGL ExternalTexture, reads pixels back
    # via glGetTextureImage, asserts they equal what Vulkan wrote.
    # Requires both phyriad_render_vulkan and phyriad_render_opengl3.
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/vk_gl_interop_test.cpp AND
       PHYRIAD_BUILD_VULKAN AND TARGET phyriad_render_vulkan AND
       TARGET phyriad_render_opengl3)
        add_executable(vk_gl_interop_test tests/vk_gl_interop_test.cpp)
        target_compile_features(vk_gl_interop_test PRIVATE cxx_std_23)
        target_link_libraries(vk_gl_interop_test PRIVATE
            phyriad_render_vulkan
            phyriad_render_opengl3)
        if(MSVC)
            target_compile_options(vk_gl_interop_test PRIVATE /W4 /wd4100)
        else()
            target_compile_options(vk_gl_interop_test PRIVATE -Wall -Wextra)
        endif()
        if(CMAKE_TESTING_ENABLED)
            add_test(NAME vk_gl_interop_test COMMAND vk_gl_interop_test)
            set_tests_properties(vk_gl_interop_test PROPERTIES TIMEOUT 30)
        endif()
    endif()
endif()
# Made with my soul - Swately <3
