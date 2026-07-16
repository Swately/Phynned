# Ayama/CMakeLists.txt
# Ayama — non-invasive runtime optimizer for asymmetric & hybrid CPUs.
#
# Self-contained: the Phyriad pillars Ayama consumes are VENDORED under
# framework/ (snapshot of catalog/cpp, 2026-07-16 — see framework/VENDORED.md).
# They are Ayama's own base now, modified at will without touching any other
# project's substrate (the PhyriadFG model).
#
# Layout:
#   ./           — this directory (the project root)
#   framework/   — the vendored pillar closure (17 units)
#
# Options:
#   AYAMA_BUILD_TESTS   — build unit tests                          (default ON)
#   AYAMA_BUILD_TOOLS   — build ayama-agent, ayama-cli, ayama-bench (default ON)
#   AYAMA_BUILD_UI      — build ayama-ui standalone window          (default ON)
#
cmake_minimum_required(VERSION 3.20)
project(Ayama LANGUAGES CXX VERSION 0.1.0)  # x-release-please-version

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Generates the root CTestTestfile.cmake so ctest DISCOVERS the submodule tests.
# Without this, ctest reported "No tests were found!!!" with exit 0 and the
# testbench printed a green having run nothing (caught 2026-07-16).
include(CTest)

# ── Options ──────────────────────────────────────────────────────────────────
option(AYAMA_BUILD_TESTS  "Build Ayama unit tests"             ON)
option(AYAMA_BUILD_TOOLS  "Build Ayama executables"            ON)
option(AYAMA_BUILD_UI     "Build ayama-ui standalone window"   ON)

# ── Fallback definition of phyriad_static_mingw_runtime ──────────────────────
# Ayama is always the CMake root since the 2026-07-16 separation, so this
# helper is always defined here (the guard is kept harmless).
if(NOT COMMAND phyriad_static_mingw_runtime)
    if(WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        function(phyriad_static_mingw_runtime _target)
            target_link_options(${_target} PRIVATE
                -static
                -static-libgcc
                -static-libstdc++
            )
        endfunction()
    else()
        function(phyriad_static_mingw_runtime _target)
        endfunction()
    endif()
endif()

# ── Phyriad framework path resolution ──────────────────────────────────────────
# The pillars live VENDORED in framework/ — Ayama's own copy, no reference
# outside the project tree. Known failure mode of this macro (lived 2026-07-15):
# an undefined target silently degrades to a raw -l flag, so configure passes
# while includes are absent — the gate is a full BUILD, never configure alone.
set(PHYRIAD_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/framework")

macro(ensure_phyriad_target _name _sub)
    if(NOT TARGET ${_name})
        add_subdirectory(${PHYRIAD_ROOT}/${_sub}
                         ${CMAKE_CURRENT_BINARY_DIR}/_phyriad_build/${_sub})
    endif()
endmacro()

ensure_phyriad_target(phyriad_hal           hal)
ensure_phyriad_target(phyriad_schema        schema)
ensure_phyriad_target(phyriad_transport     transport)
ensure_phyriad_target(phyriad_topology      topology)
ensure_phyriad_target(phyriad_scheduler     scheduler)
ensure_phyriad_target(phyriad_node          node)
ensure_phyriad_target(phyriad_graph         graph)
ensure_phyriad_target(phyriad_profile       profile)
ensure_phyriad_target(phyriad_runtime       runtime)
ensure_phyriad_target(phyriad_tuning        tuning)
# Trimmed at separation (2026-07-16): orchestration, behavior, correlation —
# zero #include and zero target_link_libraries sites in Ayama's sources
# (verified by grep + a green link without them). Re-vendor from catalog/cpp
# if a future feature needs one.
# phyriad_daemon dependency removed earlier (affinity/priority via
# phyriad_topology FR-3 / FR-9 free functions).
# Added 2026-07-15 (standalone testbench found them missing — undefined targets
# silently degrade to raw -l flags, so configure passed but includes were absent):
ensure_phyriad_target(phyriad_process       process)
ensure_phyriad_target(phyriad_etw           etw)
ensure_phyriad_target(phyriad_stigmergy     stigmergy)
ensure_phyriad_target(phyriad_ipc           ipc)
# ── GLFW + ImGui for ayama-ui ────────────────────────────────────────────────
# ayama-ui's guard requires the imgui_lib + glfw targets. They were fetched by
# the container's root CMakeLists (retired 2026-07-15) — since then ayama-ui was
# silently SKIPPED in every standalone build (its guard message(STATUS)-returns).
# Restored at separation (2026-07-16): fetch them here, BEFORE framework/render
# is added, so render's opengl3 backend and phyriad_ui pick them up. Recipe
# mirrors framework/render/CMakeLists.txt's standalone branch (same pinned tags).
# First configure downloads both from GitHub (network required once).
if(AYAMA_BUILD_UI)
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

# UI pillars — only needed for ayama-ui; silently skip if not present.
if(NOT TARGET phyriad_render)
    if(EXISTS ${PHYRIAD_ROOT}/render/CMakeLists.txt)
        add_subdirectory(${PHYRIAD_ROOT}/render
                         ${CMAKE_CURRENT_BINARY_DIR}/_phyriad_build/render)
    endif()
endif()
if(NOT TARGET phyriad_ui)
    if(EXISTS ${PHYRIAD_ROOT}/ui/CMakeLists.txt)
        add_subdirectory(${PHYRIAD_ROOT}/ui
                         ${CMAKE_CURRENT_BINARY_DIR}/_phyriad_build/ui)
    endif()
endif()

# ── Ayama submodules ──────────────────────────────────────────────────────────
# Topological order (leaf-first):
#   observer → learn → policy → action → bench → ipc → config → core
# learn is placed before policy because AutoPolicySelector.hpp (part of the
# policy library) includes <ayama/learn/PerGameMemory.hpp> for the per-game
# memory shortcut. core is last — it PRIVATELY depends on all others.
add_subdirectory(observer)
add_subdirectory(learn)
add_subdirectory(policy)
add_subdirectory(action)
add_subdirectory(bench)
add_subdirectory(ipc)
add_subdirectory(config)
add_subdirectory(core)

# ── Distribution layout ────────────────────────────────────────────────────────
# Goal: when the user opens the distribution folder, the first thing they
# see is the entry point — ayama-ui.exe — without a wall of supporting
# binaries cluttering the view. Everything else lives in a `runtime/`
# subfolder which experienced users can still navigate to for the CLI
# tools and service installer.
#
# Layout:
#   <build>/ayama-dist/
#     ayama-ui.exe                        ← entry point (top level)
#     runtime/
#       ayama-agent.exe                   ← spawned by UI
#       ayama-cli.exe                     ← power-user CLI
#       ayama-bench.exe                   ← benchmark CLI
#       ayama-service-register.exe        ← Windows service installer
#       PresentMon.exe                    ← frame capture (downloaded)
#
# The UI's AgentLauncher and bench panel both search `runtime/` first
# before falling back to legacy sibling layouts — so a downstream
# packager who wants to flatten everything back to a single dir can do
# so without breaking discovery.
#
# Override either CMake variable at configure time to relocate the dist
# tree (e.g. into the top-level Phyriad build directory) or to flatten
# the runtime subfolder back to the dist root.
set(AYAMA_RUNTIME_OUTPUT_DIRECTORY
    "${CMAKE_BINARY_DIR}/ayama-dist"
    CACHE PATH "Top-level directory where ayama-ui.exe lands")
set(AYAMA_INTERNAL_RUNTIME_DIRECTORY
    "${AYAMA_RUNTIME_OUTPUT_DIRECTORY}/runtime"
    CACHE PATH "Subdirectory for supporting binaries (agent, cli, bench, etc.)")

# ── Tools ─────────────────────────────────────────────────────────────────────
if(AYAMA_BUILD_TOOLS)
    add_subdirectory(tools/ayama-agent)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tools/ayama-cli/CMakeLists.txt)
        add_subdirectory(tools/ayama-cli)
    endif()
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tools/ayama-bench/CMakeLists.txt)
        add_subdirectory(tools/ayama-bench)
    endif()
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tools/installer/CMakeLists.txt)
        add_subdirectory(tools/installer)
    endif()
endif()

# All supporting binaries land in the internal runtime/ subfolder so the
# user-facing top-level stays clean (only ayama-ui.exe at the root).
# Done in one shot AFTER all add_subdirectory() calls so we don't need
# to modify each tool's CMakeLists.txt individually.
foreach(_ayama_exe ayama-agent ayama-cli ayama-bench ayama-service-register)
    if(TARGET ${_ayama_exe})
        set_target_properties(${_ayama_exe} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${AYAMA_INTERNAL_RUNTIME_DIRECTORY}")
    endif()
endforeach()

# ── ayama-ui — standalone optimizer window ─────────────────────────────────
if(AYAMA_BUILD_UI)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tools/ayama-ui/CMakeLists.txt)
        add_subdirectory(tools/ayama-ui)
        if(TARGET ayama-ui)
            set_target_properties(ayama-ui PROPERTIES
                RUNTIME_OUTPUT_DIRECTORY "${AYAMA_RUNTIME_OUTPUT_DIRECTORY}")
        endif()
    endif()
endif()

# ── Helper scripts (Defender exclusion, install helpers) ─────────────────────
# Copy scripts/ to <dist>/scripts/ so end users get them next to the binaries.
# Currently:
#   add-defender-exclusion.ps1 — whitelists Ayama with Windows Defender
#                                 (avoids false-positive blocks on unsigned exes).
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/scripts/add-defender-exclusion.ps1")
    set(_ayama_dist_scripts "${AYAMA_RUNTIME_OUTPUT_DIRECTORY}/scripts")
    file(MAKE_DIRECTORY "${_ayama_dist_scripts}")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/add-defender-exclusion.ps1"
        "${_ayama_dist_scripts}/add-defender-exclusion.ps1"
        COPYONLY
    )
endif()

# ── MinGW runtime — static linking for distribution ──────────────────────────
# Ayama is distributed to end users who don't have MinGW installed. Without
# this, ayama-ui.exe and the runtime/*.exe binaries fail to start with
# "libstdc++-6.dll not found" / "libgcc_s_seh-1.dll not found" /
# "libwinpthread-1.dll not found". phyriad_static_mingw_runtime() bundles
# all three into the .exe (no-op on MSVC / Linux / Clang).
# Applied AFTER all targets are defined, in one shot. See root CMakeLists.txt
# for the function definition.
foreach(_ayama_exe ayama-ui ayama-agent ayama-cli ayama-bench ayama-service-register)
    if(TARGET ${_ayama_exe})
        phyriad_static_mingw_runtime(${_ayama_exe})
    endif()
endforeach()
# Made with my soul - Swately <3
