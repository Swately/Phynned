// framework/ui/include/phyriad/ui/types/WindowState.hpp
// phyriad::ui window state snapshot message — 32 bytes, alignof=8.
//
// Published by UIThreadNode when any window dimension or focus state changes.
// Consumed by AppLogicNode and RenderNode to adapt viewport and projection.
//
#pragma once
#include <phyriad/schema/PodMessage.hpp>
#include <cstdint>

namespace phyriad::ui {

struct WindowState {
    static constexpr uint32_t kFocused    = 1u << 0;
    static constexpr uint32_t kMinimized  = 1u << 1;
    static constexpr uint32_t kFullscreen = 1u << 2;
    static constexpr uint32_t kHovered    = 1u << 3;

    uint32_t width      {0};
    uint32_t height     {0};
    uint32_t fb_width   {0};
    uint32_t fb_height  {0};
    float    dpi_scale  {1.0f};
    uint32_t flags      {0};
    uint64_t change_tsc {0};
};

static_assert(sizeof(WindowState)  == 32u);
static_assert(alignof(WindowState) == 8u);
static_assert(schema::PodMessage<WindowState>);

} // namespace phyriad::ui
// Made with my soul - Swately <3
