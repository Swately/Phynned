# catalog/cpp/render/present/CMakeLists.txt
# phyriad_render_present — external presentation-composition primitive (FR-RENDER-1).
#
# The presenter that owns the panel: a capture-proof, input-invisible,
# flip/composition-grade alpha overlay over a running game (PresentSurface).
# Mirrors catalog/cpp/stream/ in shape — standalone-buildable:
#   cmake -S catalog/cpp/render/present -B build-present -G Ninja
# resolves phyriad_schema in-place when it is not already a target in a parent build.
#
# Windows-only capability (DComp / composition swapchain / WDA are Win32). The
# non-Windows build compiles the same library to a no-op stub TU whose methods
# return ErrorCode::Unavailable (the etw posture, D-19).
#
cmake_minimum_required(VERSION 3.20)
project(phyriad_render_present LANGUAGES CXX)

# ── Standalone dependency resolution ──────────────────────────────────────────
# phyriad_schema provides phyriad::Error / phyriad::ErrorCode (the only Phyriad
# dependency). Resolved in-place when building this sub-pillar in isolation.
if(NOT TARGET phyriad_schema)
    add_subdirectory(
        ${CMAKE_CURRENT_SOURCE_DIR}/../../schema
        ${CMAKE_CURRENT_BINARY_DIR}/phyriad_schema
    )
endif()

# ── Library ───────────────────────────────────────────────────────────────────
add_library(phyriad_render_present STATIC
    src/PresentSurface.cpp
)

target_include_directories(phyriad_render_present PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(phyriad_render_present PUBLIC cxx_std_23)

target_link_libraries(phyriad_render_present PUBLIC
    phyriad_schema
)

# Windows composition/present system libraries. PUBLIC so downstream binaries that
# link this pillar inherit the dependency automatically (the stream ws2_32 pattern).
if(WIN32)
    target_link_libraries(phyriad_render_present PUBLIC d3d11 dxgi dcomp)
    # Win10+ so WS_EX_NOREDIRECTIONBITMAP + WDA_EXCLUDEFROMCAPTURE are declared.
    target_compile_definitions(phyriad_render_present PRIVATE WINVER=0x0A00 _WIN32_WINNT=0x0A00)
endif()

if(MSVC)
    target_compile_options(phyriad_render_present PRIVATE /W4 /permissive-)
else()
    target_compile_options(phyriad_render_present PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ── Tests ─────────────────────────────────────────────────────────────────────
if(BUILD_TESTING OR PHYRIAD_BUILD_TESTS)
    include(CTest)

    # CI-style: no screen needed. POD layout + non-Windows-stub + descriptor
    # defaults + (on Windows) create/teardown of a real DcompCt surface.
    add_executable(present_surface_test tests/present_surface_test.cpp)
    target_link_libraries(present_surface_test PRIVATE phyriad_render_present)
    target_compile_features(present_surface_test PRIVATE cxx_std_23)
    if(MSVC)
        target_compile_options(present_surface_test PRIVATE /W4 /permissive-)
    else()
        target_compile_options(present_surface_test PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME present_surface_test COMMAND present_surface_test LABELS present)
    if(COMMAND phyriad_test_dll_env)
        phyriad_test_dll_env(present_surface_test)
    endif()

    # Acceptance smoke consumer (Windows-only): creates a DcompCt+WDA surface, runs
    # PresentMon against THIS process, reports the PresentMode VERBATIM. NOT wired
    # into ctest by default (it needs a live screen + F:\PresetMon\PresentMon.exe) —
    # built as a manual harness (the §1.8 acceptance run). See present.md.
    if(WIN32)
        add_executable(present_acceptance_smoke tests/present_acceptance_smoke.cpp)
        target_link_libraries(present_acceptance_smoke PRIVATE phyriad_render_present)
        target_compile_features(present_acceptance_smoke PRIVATE cxx_std_23)
        if(MSVC)
            target_compile_options(present_acceptance_smoke PRIVATE /W4)
        else()
            target_compile_options(present_acceptance_smoke PRIVATE -Wall -Wextra)
        endif()
    endif()
endif()

# Made with my soul - Swately <3
