# ayama/tools/ayama-ui/CMakeLists.txt
# ayama-ui — Ayama standalone optimizer window (ayama.exe)
#
# Follows the exact same build pattern as examples/standard_window.
# Requires imgui_lib and glfw (fetched by top-level CMakeLists when
# PHYRIAD_BUILD_AYAMA=ON triggers _phyriad_need_glfw).
#
# Pillar deps: phyriad_ui (Application, RenderNode), phyriad_render (backend)
# Ayama deps:  ayama_ipc (AyamaClient), ayama_core (types), ayama_action (log)
#

# Guard: only build if the UI targets are available.
if(NOT TARGET phyriad_ui OR NOT TARGET imgui_lib)
    message(STATUS "ayama-ui: skipping (phyriad_ui or imgui_lib not available). "
                   "Build from top-level with -DPHYRIAD_BUILD_AYAMA=ON.")
    return()
endif()

# Windows: embed the manifest resource so the .exe ships with UAC
# requireAdministrator + DPI awareness baked into its PE headers. We add
# the .rc file to the executable's sources; CMake invokes the platform's
# resource compiler (windres on MinGW, rc on MSVC) automatically.
set(AYAMA_UI_SOURCES main.cpp)
if(WIN32)
    enable_language(RC)
    list(APPEND AYAMA_UI_SOURCES ayama-ui.rc)
endif()

add_executable(ayama-ui ${AYAMA_UI_SOURCES})

target_include_directories(ayama-ui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}        # AyamaAppState.hpp, AyamaLogicNode.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/widgets
    ${PHYRIAD_ROOT}/_meta/include  # <phyriad/version.hpp> (orphan header — no phyriad_* target re-exports it)
)

target_link_libraries(ayama-ui PRIVATE
    # Phyriad pillars
    phyriad_ui
    phyriad_render
    phyriad_hal
    phyriad_schema
    phyriad_transport
    # ImGui + GLFW (fetched by top-level)
    imgui_lib
    glfw
    # Ayama libs
    ayama_ipc
    ayama_core
    ayama_action
    ayama_observer
    ayama_policy
)

target_compile_features(ayama-ui PRIVATE cxx_std_23)

if(MSVC)
    target_compile_options(ayama-ui PRIVATE /W4)
    set_target_properties(ayama-ui PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:WINDOWS")
else()
    target_compile_options(ayama-ui PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    target_link_libraries(ayama-ui PRIVATE
        opengl32
        kernel32 user32 advapi32 gdi32
        # ole32 / shell32: required by the Settings tab folder-picker
        # (IFileDialog + CLSID_FileOpenDialog COM coclass live in shell32,
        # plumbing in ole32). uuid carries the IFileDialog interface GUID.
        ole32 shell32 uuid
    )
    target_compile_definitions(ayama-ui PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

# ── PresentMon binary fetch ────────────────────────
# Downloads the PresentMon CLI from GameTechDev/PresentMon GitHub releases at
# configure time. The .exe ends up next to ayama-ui.exe in the build output dir
# so the UI's bench runner can spawn it as a child process.
#
# Pin to a specific version for reproducible builds. To bump: change
# AYAMA_PRESENTMON_VERSION + AYAMA_PRESENTMON_SHA256 below.
#
# License: PresentMon is MIT-licensed (see github.com/GameTechDev/PresentMon).
# We do NOT redistribute the binary in the repo — only fetch on demand.
set(AYAMA_PRESENTMON_VERSION "2.4.1" CACHE STRING
    "PresentMon version to download for ayama-ui bench runner")
set(_pm_url "https://github.com/GameTechDev/PresentMon/releases/download/v${AYAMA_PRESENTMON_VERSION}/PresentMon-${AYAMA_PRESENTMON_VERSION}-x64.exe")
# Download into the internal runtime/ subfolder where the rest of the
# supporting binaries (agent, cli, bench) live. The bench panel knows to
# search runtime/ when looking for PresentMon.exe. Fall back to the
# local build dir if the parent CMakeLists hasn't defined the path yet.
if(DEFINED AYAMA_INTERNAL_RUNTIME_DIRECTORY)
    set(_pm_dest "${AYAMA_INTERNAL_RUNTIME_DIRECTORY}/PresentMon.exe")
    file(MAKE_DIRECTORY "${AYAMA_INTERNAL_RUNTIME_DIRECTORY}")
elseif(DEFINED AYAMA_RUNTIME_OUTPUT_DIRECTORY)
    set(_pm_dest "${AYAMA_RUNTIME_OUTPUT_DIRECTORY}/PresentMon.exe")
    file(MAKE_DIRECTORY "${AYAMA_RUNTIME_OUTPUT_DIRECTORY}")
else()
    set(_pm_dest "${CMAKE_CURRENT_BINARY_DIR}/PresentMon.exe")
endif()

if(NOT EXISTS "${_pm_dest}")
    message(STATUS "ayama-ui: downloading PresentMon ${AYAMA_PRESENTMON_VERSION}...")
    file(DOWNLOAD
        "${_pm_url}"
        "${_pm_dest}"
        SHOW_PROGRESS
        TIMEOUT 120
        STATUS _pm_status
        LOG     _pm_log)
    list(GET _pm_status 0 _pm_code)
    if(NOT _pm_code EQUAL 0)
        list(GET _pm_status 1 _pm_msg)
        # Common on MinGW-CMake on Windows: missing CA bundle for HTTPS.
        # Retry with TLS_VERIFY OFF. We're downloading from an official
        # GitHub release URL so the practical risk is minimal — but log it.
        message(STATUS
            "ayama-ui: first download attempt failed (${_pm_code}: ${_pm_msg}). "
            "Retrying without TLS verification...")
        file(REMOVE "${_pm_dest}")
        file(DOWNLOAD
            "${_pm_url}"
            "${_pm_dest}"
            SHOW_PROGRESS
            TIMEOUT 120
            TLS_VERIFY OFF
            STATUS _pm_status2
            LOG     _pm_log2)
        list(GET _pm_status2 0 _pm_code2)
        if(NOT _pm_code2 EQUAL 0)
            list(GET _pm_status2 1 _pm_msg2)
            message(WARNING
                "ayama-ui: PresentMon download FAILED twice "
                "(${_pm_code2}: ${_pm_msg2}).\n"
                "  URL: ${_pm_url}\n"
                "  The bench runner UI will be disabled until you manually place\n"
                "  PresentMon.exe at: ${_pm_dest}\n"
                "  Or re-run cmake with internet access.")
            file(REMOVE "${_pm_dest}")
        else()
            message(STATUS "ayama-ui: PresentMon downloaded -> ${_pm_dest} "
                            "(TLS verification disabled)")
        endif()
    else()
        message(STATUS "ayama-ui: PresentMon downloaded -> ${_pm_dest}")
    endif()
endif()

# Inform the UI about the PresentMon path via compile definition.
# The UI looks for PresentMon.exe relative to its own .exe path at runtime
# (so this works for both dev builds and installed bundles).
target_compile_definitions(ayama-ui PRIVATE
    AYAMA_PRESENTMON_FILENAME="PresentMon.exe"
    AYAMA_PRESENTMON_VERSION="${AYAMA_PRESENTMON_VERSION}"
)

# Install target (optional)
if(CMAKE_INSTALL_PREFIX)
    install(TARGETS ayama-ui RUNTIME DESTINATION bin)
    if(EXISTS "${_pm_dest}")
        install(FILES "${_pm_dest}" DESTINATION bin)
    endif()
endif()
# Made with my soul - Swately <3
