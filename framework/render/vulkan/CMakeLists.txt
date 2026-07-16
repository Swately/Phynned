# catalog/cpp/render/vulkan/CMakeLists.txt
# phyriad_render_vulkan — backend de renderizado Vulkan 1.2+ con ImGui.
#
# Dependencias:
#   Vulkan::Vulkan  — SDK de Vulkan (LunarG). Si no está disponible, el
#                     target NO se crea y el parent puede detectarlo con
#                     if(TARGET phyriad_render_vulkan).
#   glfw            — ventana y contexto. Buscado via FetchContent o find_package.
#   imgui_lib       — Dear ImGui con backends GLFW + Vulkan.
#   phyriad_render    — IRenderBackend, RenderStats, FrameArena.
#   phyriad_hal       — hal::rdtsc(), calibrate_tsc_freq().
#   phyriad_schema    — phyriad::Error, phyriad::ErrorCode.
#
# Opciones:
#   PHYRIAD_BUILD_FSR2  — FSR2Pass + FSR3Pass (requiere AMD FidelityFX SDK
#                       en third_party/ffx/). Cuando el SDK no está presente,
#                       los stubs compilan con PHYRIAD_HAS_FFX_SDK=0.
#
cmake_minimum_required(VERSION 3.20)

# ── Encontrar SDK Vulkan ──────────────────────────────────────────────────────
find_package(Vulkan QUIET)
if(NOT Vulkan_FOUND)
    message(STATUS "phyriad_render_vulkan: Vulkan SDK not found — skipping build")
    return()
endif()

message(STATUS "phyriad_render_vulkan: Vulkan ${Vulkan_VERSION} found at ${Vulkan_INCLUDE_DIRS}")

# ── glslc — GLSL → SPIR-V compiler (from the same SDK) ───────────────────────
# Required for compiling FrameInterpolatorNode's blend compute shader (and
# future v1.1 compute passes). Without it the shader-to-header step fails
# at build time with a clear message.
find_program(PHYRIAD_GLSLC glslc
    HINTS
        "${Vulkan_INCLUDE_DIRS}/../Bin"     # Windows SDK layout
        "${Vulkan_INCLUDE_DIRS}/../bin"     # POSIX SDK layout
        "$ENV{VULKAN_SDK}/Bin"
        "$ENV{VULKAN_SDK}/bin"
    DOC "Path to glslc (GLSL → SPIR-V) from the Vulkan SDK")
if(NOT PHYRIAD_GLSLC)
    message(FATAL_ERROR
        "phyriad_render_vulkan: glslc not found. Install the Vulkan SDK and "
        "ensure VULKAN_SDK env var points at it.")
endif()
message(STATUS "phyriad_render_vulkan: glslc = ${PHYRIAD_GLSLC}")

# ── compile_spv_to_header() ────────────────────────────────────────────────
# Compile a GLSL compute shader into an embedded uint32_t array header
# the C++ code can pass directly to vkCreateShaderModule. The two-step
# pipeline (.comp → .spv → .hpp) makes the generated header reproducible
# and avoids any runtime file I/O.
#
# Args:
#   _shader_src     full path to the GLSL source
#   _output_header  full path of the .hpp to generate
#   _var_name       name of the constexpr std::array in the header
function(phyriad_compile_spv_to_header _shader_src _output_header _var_name)
    get_filename_component(_stem  "${_shader_src}" NAME_WE)
    set(_spv "${CMAKE_CURRENT_BINARY_DIR}/shaders/${_stem}.spv")

    add_custom_command(
        OUTPUT "${_spv}"
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "${CMAKE_CURRENT_BINARY_DIR}/shaders"
        COMMAND "${PHYRIAD_GLSLC}"
                --target-env=vulkan1.2
                -O                          # optimise
                "${_shader_src}"
                -o "${_spv}"
        DEPENDS "${_shader_src}"
        COMMENT "glslc ${_stem}.comp -> ${_stem}.spv"
        VERBATIM)

    add_custom_command(
        OUTPUT "${_output_header}"
        COMMAND ${CMAKE_COMMAND}
                -D "INPUT=${_spv}"
                -D "OUTPUT=${_output_header}"
                -D "VARNAME=${_var_name}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/spv_to_header.cmake"
        DEPENDS "${_spv}"
                "${CMAKE_CURRENT_SOURCE_DIR}/cmake/spv_to_header.cmake"
        COMMENT "spv -> header: ${_stem}_spv.hpp"
        VERBATIM)
endfunction()

# ── Compile the FrameInterpolator blend shader ───────────────────────────────
set(_blend_spv_header
    "${CMAKE_CURRENT_BINARY_DIR}/shaders/frame_blend_linear_spv.hpp")
phyriad_compile_spv_to_header(
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/frame_blend_linear.comp"
    "${_blend_spv_header}"
    "kFrameBlendLinearSpv")

# ── Compile the OpticalFlow shaders (block-match + warp) ─────────────────────
set(_of_match_spv_header
    "${CMAKE_CURRENT_BINARY_DIR}/shaders/optical_flow_block_match_spv.hpp")
phyriad_compile_spv_to_header(
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/optical_flow_block_match.comp"
    "${_of_match_spv_header}"
    "kOpticalFlowBlockMatchSpv")

set(_of_warp_spv_header
    "${CMAKE_CURRENT_BINARY_DIR}/shaders/optical_flow_warp_spv.hpp")
phyriad_compile_spv_to_header(
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/optical_flow_warp.comp"
    "${_of_warp_spv_header}"
    "kOpticalFlowWarpSpv")

# hierarchical/pyramid block-match (STAGE-15: promoted from bench/stage14_hierarchical) —
# downsample + coarse-to-fine match. Drives OpticalFlowPipeline's large-displacement tracking.
set(_of_pyrdown_spv_header
    "${CMAKE_CURRENT_BINARY_DIR}/shaders/optical_flow_pyr_down_spv.hpp")
phyriad_compile_spv_to_header(
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/optical_flow_pyr_down.comp"
    "${_of_pyrdown_spv_header}"
    "kOpticalFlowPyrDownSpv")

set(_of_hiermatch_spv_header
    "${CMAKE_CURRENT_BINARY_DIR}/shaders/optical_flow_hier_match_spv.hpp")
phyriad_compile_spv_to_header(
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/optical_flow_hier_match.comp"
    "${_of_hiermatch_spv_header}"
    "kOpticalFlowHierMatchSpv")

# CANON #12 app-local FORK (render_assistant): the FG-LOCAL matcher variant with the runner-up (STAGE-77
# second-best) tracking compiled OUT — selected by OpticalFlowPipeline only when fg_variant is armed AND
# emit_second_best is OFF (the default FG flow path). Same descriptor/push layout as the canonical above
# (so the pipeline layout is reused); the canonical stays byte-identical for the gme/ambig consumers.
set(_of_hiermatch_fg_spv_header
    "${CMAKE_CURRENT_BINARY_DIR}/shaders/optical_flow_hier_match_fg_spv.hpp")
phyriad_compile_spv_to_header(
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/optical_flow_hier_match_fg.comp"
    "${_of_hiermatch_fg_spv_header}"
    "kOpticalFlowHierMatchFgSpv")

# ARC-A LEVER-1: per-tile affine-fit post-pass (OpticalFlowPipeline.cpp #includes it when armed).
set(_of_affinefit_spv_header
    "${CMAKE_CURRENT_BINARY_DIR}/shaders/optical_flow_affine_fit_spv.hpp")
phyriad_compile_spv_to_header(
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/optical_flow_affine_fit.comp"
    "${_of_affinefit_spv_header}"
    "kOpticalFlowAffineFitSpv")

# ── Pillar guards — asegurar que los pilares base están cargados ──────────────
if(NOT TARGET phyriad_schema)
    add_subdirectory(../../schema  ${CMAKE_CURRENT_BINARY_DIR}/schema)
endif()
if(NOT TARGET phyriad_hal)
    add_subdirectory(../../hal     ${CMAKE_CURRENT_BINARY_DIR}/hal)
endif()
if(NOT TARGET phyriad_render)
    add_subdirectory(../../render  ${CMAKE_CURRENT_BINARY_DIR}/render_base)
endif()

# ── AMD FidelityFX SDK detection ─────────────────────────────────────────────
set(_ffx_root "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffx")
set(_has_ffx_sdk 0)
if(PHYRIAD_BUILD_FSR2)
    if(EXISTS "${_ffx_root}/include/FfxFsr2.h")
        message(STATUS "phyriad_render_vulkan: FidelityFX SDK found at ${_ffx_root}")
        set(_has_ffx_sdk 1)
    else()
        message(STATUS
            "phyriad_render_vulkan: PHYRIAD_BUILD_FSR2=ON but FidelityFX SDK not found "
            "at ${_ffx_root}/include/FfxFsr2.h — compiling FSR2/3 stubs")
    endif()
endif()

# ── Librería ──────────────────────────────────────────────────────────────────
set(_vk_sources
    src/VulkanContext.cpp
    src/Swapchain.cpp
    src/VulkanBackend.cpp
    src/FramePool.cpp          # lock-free frame image pool
    src/GpuTelemetry.cpp       # per-frame GPU timestamp queries
    src/BlendPipeline.cpp      # v1.0 — FrameInterpolator LinearBlend compute
    src/ExternalImage.cpp      # v1.0 — VK↔GL interop image (CompositeBackend)
    src/OpticalFlowPipeline.cpp  # v1.0 — block-match + warp for OpticalFlow
    src/OffscreenScene.cpp)    # v1.0 — render pass targeting external image

if(PHYRIAD_BUILD_FSR2)
    list(APPEND _vk_sources
        src/FSR2Pass.cpp
        src/FSR3Pass.cpp)
endif()

add_library(phyriad_render_vulkan STATIC ${_vk_sources})

# Add the generated SPV-header as a build dependency so the compile rule
# fires before anything that #includes the corresponding *_spv.hpp compiles.
target_sources(phyriad_render_vulkan PRIVATE
    "${_blend_spv_header}"
    "${_of_match_spv_header}"
    "${_of_warp_spv_header}"
    "${_of_pyrdown_spv_header}"
    "${_of_hiermatch_spv_header}"
    "${_of_hiermatch_fg_spv_header}"
    "${_of_affinefit_spv_header}")

target_compile_features(phyriad_render_vulkan PRIVATE cxx_std_23)

# Propagate the build flag so consumers know Vulkan is compiled in.
# Application.hpp uses #ifdef PHYRIAD_BUILD_VULKAN to gate VulkanBackend includes.
target_compile_definitions(phyriad_render_vulkan PUBLIC PHYRIAD_BUILD_VULKAN)

# FSR2/3 SDK presence flag — used in FSR2Pass.cpp / FSR3Pass.cpp via #if.
if(PHYRIAD_BUILD_FSR2)
    target_compile_definitions(phyriad_render_vulkan
        PRIVATE PHYRIAD_HAS_FFX_SDK=${_has_ffx_sdk})
endif()

target_include_directories(phyriad_render_vulkan
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        # Generated SPV headers live in the build tree.
        ${CMAKE_CURRENT_BINARY_DIR}/shaders
)

# FidelityFX SDK include path — only when SDK is present.
if(_has_ffx_sdk)
    target_include_directories(phyriad_render_vulkan PRIVATE
        "${_ffx_root}/include")
    # Link prebuilt SDK lib if present (optional — some SDK versions are
    # header-only or require linking the backend implementation separately).
    if(EXISTS "${_ffx_root}/lib/x64/ffxfsr2_x64.lib")
        target_link_libraries(phyriad_render_vulkan PRIVATE
            "${_ffx_root}/lib/x64/ffxfsr2_x64.lib")
    elseif(EXISTS "${_ffx_root}/lib/ffxfsr2.a")
        target_link_libraries(phyriad_render_vulkan PRIVATE
            "${_ffx_root}/lib/ffxfsr2.a")
    endif()
endif()

target_link_libraries(phyriad_render_vulkan
    PUBLIC
        phyriad_render    # IRenderBackend, RenderStats, FrameArena
        phyriad_hal       # hal::rdtsc(), Timestamp
        phyriad_schema    # phyriad::Error
        Vulkan::Vulkan  # VkInstance, vkCreate*, etc.
)

# GLFW — necesario para glfwVulkanSupported / glfwCreateWindowSurface.
if(TARGET glfw)
    target_link_libraries(phyriad_render_vulkan PUBLIC glfw)
else()
    find_package(glfw3 QUIET)
    if(glfw3_FOUND)
        target_link_libraries(phyriad_render_vulkan PUBLIC glfw3::glfw3)
    else()
        message(WARNING
            "phyriad_render_vulkan: GLFW not found — "
            "glfwCreateWindowSurface will not be available")
    endif()
endif()

# imgui_lib — backends GLFW + Vulkan deben estar compilados en imgui_lib.
# La configuración de imgui_lib (con VULKAN backend) se hace en el top-level
# CMakeLists o en build_ui/. Este target los espera disponibles.
if(TARGET imgui_lib)
    target_link_libraries(phyriad_render_vulkan PUBLIC imgui_lib)
else()
    message(WARNING
        "phyriad_render_vulkan: imgui_lib target not found — "
        "ImGui Vulkan backend will not be linked")
endif()

# ── Include paths de ImGui backends ──────────────────────────────────────────
# imgui_impl_vulkan.h y imgui_impl_glfw.h deben estar en el path de búsqueda.
# Si imgui_lib no proporciona los paths via INTERFACE, los añadimos aquí.
# El directorio real depende del setup de FetchContent en el top-level CMake.
if(TARGET imgui_lib)
    get_target_property(IMGUI_INC imgui_lib INTERFACE_INCLUDE_DIRECTORIES)
    if(IMGUI_INC)
        target_include_directories(phyriad_render_vulkan PRIVATE ${IMGUI_INC})
    endif()
endif()

# ── Warnings ──────────────────────────────────────────────────────────────────
if(MSVC)
    target_compile_options(phyriad_render_vulkan PRIVATE
        /W4
        /wd4100   # unreferenced formal parameter
        /wd4458   # declaration hides class member
    )
else()
    target_compile_options(phyriad_render_vulkan PRIVATE
        -Wall -Wextra
    )
endif()

# ── Tests ─────────────────────────────────────────────────────────────────────
if(PHYRIAD_BUILD_TESTS)
    # ── vulkan_init_test — VulkanBackend headless contract ────────────────────
    add_executable(vulkan_init_test
        tests/vulkan_init_test.cpp
    )
    target_compile_features(vulkan_init_test PRIVATE cxx_std_23)
    target_link_libraries(vulkan_init_test PRIVATE phyriad_render_vulkan)
    if(MSVC)
        target_compile_options(vulkan_init_test PRIVATE /W4 /wd4100)
    else()
        target_compile_options(vulkan_init_test PRIVATE -Wall -Wextra)
    endif()
    if(CMAKE_TESTING_ENABLED)
        add_test(NAME vulkan_init_test COMMAND vulkan_init_test)
    endif()

    # ── gpu_telemetry_test — GpuTelemetry/GpuMetrics headless ───────────────
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/gpu_telemetry_test.cpp)
        add_executable(gpu_telemetry_test tests/gpu_telemetry_test.cpp)
        target_compile_features(gpu_telemetry_test PRIVATE cxx_std_23)
        target_link_libraries(gpu_telemetry_test PRIVATE phyriad_render_vulkan)
        if(MSVC)
            target_compile_options(gpu_telemetry_test PRIVATE /W4 /wd4100)
        else()
            target_compile_options(gpu_telemetry_test PRIVATE -Wall -Wextra)
        endif()
        if(CMAKE_TESTING_ENABLED)
            add_test(NAME gpu_telemetry_test COMMAND gpu_telemetry_test)
        endif()
    endif()

    # ── frame_pool_test — FramePool headless contract ────────────────────────
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/frame_pool_test.cpp)
        add_executable(frame_pool_test tests/frame_pool_test.cpp)
        target_compile_features(frame_pool_test PRIVATE cxx_std_23)
        target_link_libraries(frame_pool_test PRIVATE phyriad_render_vulkan)
        if(MSVC)
            target_compile_options(frame_pool_test PRIVATE /W4 /wd4100)
        else()
            target_compile_options(frame_pool_test PRIVATE -Wall -Wextra)
        endif()
        if(CMAKE_TESTING_ENABLED)
            add_test(NAME frame_pool_test COMMAND frame_pool_test)
        endif()
    endif()

    # ── blend_pipeline_test — BlendPipeline headless math verification ────────
    # Uploads red + blue to two images, dispatches the blend shader,
    # reads back, asserts every pixel ≈ (127, 0, 127, 255). Validates
    # the LinearBlend implementation without requiring visual review.
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/blend_pipeline_test.cpp)
        add_executable(blend_pipeline_test tests/blend_pipeline_test.cpp)
        target_compile_features(blend_pipeline_test PRIVATE cxx_std_23)
        target_link_libraries(blend_pipeline_test PRIVATE phyriad_render_vulkan)
        if(MSVC)
            target_compile_options(blend_pipeline_test PRIVATE /W4 /wd4100)
        else()
            target_compile_options(blend_pipeline_test PRIVATE -Wall -Wextra)
        endif()
        if(CMAKE_TESTING_ENABLED)
            add_test(NAME blend_pipeline_test COMMAND blend_pipeline_test)
            set_tests_properties(blend_pipeline_test PROPERTIES TIMEOUT 30)
        endif()
    endif()

    # ── external_image_test — VK external-memory image + handle export ────────
    # Stage 2 of the CompositeBackend VK↔GL interop work: allocate an
    # image whose backing memory can be exported as an OS handle
    # (HANDLE on Windows, fd on POSIX), then verify the handle is
    # non-null and closeable. Stage 3 will add the GL import path on
    # top of the same handle export.
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/external_image_test.cpp)
        add_executable(external_image_test tests/external_image_test.cpp)
        target_compile_features(external_image_test PRIVATE cxx_std_23)
        target_link_libraries(external_image_test PRIVATE phyriad_render_vulkan)
        if(MSVC)
            target_compile_options(external_image_test PRIVATE /W4 /wd4100)
        else()
            target_compile_options(external_image_test PRIVATE -Wall -Wextra)
        endif()
        if(CMAKE_TESTING_ENABLED)
            add_test(NAME external_image_test COMMAND external_image_test)
            set_tests_properties(external_image_test PROPERTIES TIMEOUT 30)
        endif()
    endif()

    # ── optical_flow_test — OpticalFlowPipeline synthetic-motion check ────────
    # Builds two frames with a small blob translated by a known vector
    # (+5, +3), runs the block-match + warp pipelines, reads the motion-
    # vector image back to host memory, and asserts the tile containing
    # the blob reports the expected motion within ±0.5 pixel.
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/optical_flow_test.cpp)
        add_executable(optical_flow_test tests/optical_flow_test.cpp)
        target_compile_features(optical_flow_test PRIVATE cxx_std_23)
        target_link_libraries(optical_flow_test PRIVATE phyriad_render_vulkan)
        if(MSVC)
            target_compile_options(optical_flow_test PRIVATE /W4 /wd4100)
        else()
            target_compile_options(optical_flow_test PRIVATE -Wall -Wextra)
        endif()
        if(CMAKE_TESTING_ENABLED)
            add_test(NAME optical_flow_test COMMAND optical_flow_test)
            set_tests_properties(optical_flow_test PROPERTIES TIMEOUT 60)
        endif()
    endif()

    # ── fsr2_test — FSR2Pass/FSR3Pass API contract (headless) ─────────────────
    # Built whenever PHYRIAD_BUILD_FSR2=ON (SDK presence is not required —
    # stubs compile without it and the tests validate the error path).
    if(PHYRIAD_BUILD_FSR2)
        add_executable(fsr2_test tests/fsr2_test.cpp)
        target_compile_features(fsr2_test PRIVATE cxx_std_23)
        target_link_libraries(fsr2_test PRIVATE phyriad_render_vulkan)
        target_compile_definitions(fsr2_test PRIVATE
            PHYRIAD_HAS_FFX_SDK=${_has_ffx_sdk})
        if(MSVC)
            target_compile_options(fsr2_test PRIVATE /W4 /wd4100)
        else()
            target_compile_options(fsr2_test PRIVATE -Wall -Wextra)
        endif()
        if(CMAKE_TESTING_ENABLED)
            add_test(NAME fsr2_test COMMAND fsr2_test)
        endif()
    endif()
endif()
# Made with my soul - Swately <3
