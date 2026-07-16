// framework/ui/include/phyriad/ui/ApplicationConfig.hpp
// Configuration struct for phyriad::ui::Application.
//
// Passed to Application::run() to configure the window, render backend,
// frame arena size, and performance profile. All fields have safe defaults.
//
#pragma once
#include <phyriad/render/RenderBackendKind.hpp>
#include <phyriad/topology/HardwareTopology.hpp>
#include <cstdint>
#include <cstddef>

namespace phyriad {

// ProfileKind — convenience alias, mirrors the concept in PerformanceProfile.
// LATENCY  = pin threads, V-Cache preferred, busy-wait loops.
// BALANCED = scheduler-driven placement, no busy-wait.
// POWER    = reduced spin counts, allow E-cores.
enum class ProfileKind : uint8_t {
    LATENCY  = 0,
    BALANCED = 1,
    POWER    = 2,
};

} // namespace phyriad

namespace phyriad::ui {

struct ApplicationConfig {
    struct Window {
        uint32_t    width    {1280};
        uint32_t    height   { 720};
        const char* title    {"Phyriad"};
        bool        resizable{true};
    };

    Window                         window;
    std::size_t                    frame_arena_capacity{4u * 1024u * 1024u};
    phyriad::ProfileKind               profile{phyriad::ProfileKind::BALANCED};
    phyriad::render::RenderBackendKind render_backend{
        phyriad::render::RenderBackendKind::OpenGL3};
};

} // namespace phyriad::ui
// Made with my soul - Swately <3
