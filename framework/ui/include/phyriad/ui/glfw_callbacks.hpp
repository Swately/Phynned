// framework/ui/include/phyriad/ui/glfw_callbacks.hpp
// GLFW event → phyriad::ui message packing functions.
//
// Each function converts a raw GLFW callback argument set into an InputEvent
// or WindowState struct. All functions are noexcept and allocation-free.
// timestamp_tsc is always stamped via hal::rdtsc() at call time.
//
// These are free functions (not methods) so they can be used both from
// UIThreadNode callbacks and from unit tests that inject synthetic events.
//
#pragma once
#include <phyriad/ui/types/InputEvent.hpp>
#include <phyriad/ui/types/WindowState.hpp>
#include <phyriad/hal/Timestamp.hpp>
#include <GLFW/glfw3.h>
#include <cstring>

namespace phyriad::ui::glfw_callbacks {

// ── Mouse button ──────────────────────────────────────────────────────────────
[[nodiscard]] inline InputEvent
pack_mouse_button(GLFWwindow* /*window*/,
                  int button, int action, int mods) noexcept
{
    InputEvent ev{};
    ev.kind          = InputEvent::Kind::MouseButton;
    ev.action        = static_cast<uint8_t>(action);
    ev.button        = static_cast<uint8_t>(button);
    ev.modifiers     = static_cast<uint32_t>(mods);
    ev.timestamp_tsc = hal::rdtsc();
    return ev;
}

// ── Cursor position ───────────────────────────────────────────────────────────
[[nodiscard]] inline InputEvent
pack_cursor_pos(GLFWwindow* /*window*/, double x, double y) noexcept
{
    InputEvent ev{};
    ev.kind          = InputEvent::Kind::MouseMove;
    ev.x             = static_cast<int32_t>(x);
    ev.y             = static_cast<int32_t>(y);
    ev.timestamp_tsc = hal::rdtsc();
    // Sub-pixel precision packed into payload[0..7] (two doubles).
    double coords[2] = {x, y};
    std::memcpy(ev.payload, coords, sizeof(coords));
    return ev;
}

// ── Key event ─────────────────────────────────────────────────────────────────
[[nodiscard]] inline InputEvent
pack_key(GLFWwindow* /*window*/,
         int key, int scancode, int action, int mods) noexcept
{
    InputEvent ev{};
    ev.kind          = InputEvent::Kind::Key;
    ev.action        = static_cast<uint8_t>(action);
    ev.modifiers     = static_cast<uint32_t>(mods);
    ev.key_or_char   = static_cast<uint32_t>(key);
    ev.scan_code     = static_cast<uint32_t>(scancode);
    ev.timestamp_tsc = hal::rdtsc();
    return ev;
}

// ── Character (Unicode codepoint) ─────────────────────────────────────────────
[[nodiscard]] inline InputEvent
pack_char(GLFWwindow* /*window*/, unsigned int codepoint) noexcept
{
    InputEvent ev{};
    ev.kind          = InputEvent::Kind::Char;
    ev.key_or_char   = codepoint;
    ev.timestamp_tsc = hal::rdtsc();
    return ev;
}

// ── Scroll ────────────────────────────────────────────────────────────────────
[[nodiscard]] inline InputEvent
pack_scroll(GLFWwindow* /*window*/, double dx, double dy) noexcept
{
    InputEvent ev{};
    ev.kind          = InputEvent::Kind::Scroll;
    ev.timestamp_tsc = hal::rdtsc();
    // Pack scroll deltas into payload as two doubles (dx, dy).
    double deltas[2] = {dx, dy};
    std::memcpy(ev.payload, deltas, sizeof(deltas));
    return ev;
}

// ── Mouse enter / leave ───────────────────────────────────────────────────────
[[nodiscard]] inline InputEvent
pack_cursor_enter(GLFWwindow* /*window*/, int entered) noexcept
{
    InputEvent ev{};
    ev.kind          = entered ? InputEvent::Kind::MouseEnter
                               : InputEvent::Kind::MouseLeave;
    ev.timestamp_tsc = hal::rdtsc();
    return ev;
}

// ── Framebuffer size → WindowState ───────────────────────────────────────────
// Returns a WindowState snapshot reflecting the new framebuffer dimensions.
// `window_w/window_h` are the logical window size (OS coords).
[[nodiscard]] inline WindowState
pack_framebuffer_size(GLFWwindow* window, int fb_w, int fb_h) noexcept
{
    WindowState ws{};

    int win_w = fb_w, win_h = fb_h;
    if (window) glfwGetWindowSize(window, &win_w, &win_h);

    // If fb is 0 (modal resize on Windows), use logical size.
    if (fb_w == 0 || fb_h == 0) {
        fb_w = win_w;
        fb_h = win_h;
    }

    ws.fb_width  = static_cast<uint32_t>(fb_w);
    ws.fb_height = static_cast<uint32_t>(fb_h);
    ws.width     = static_cast<uint32_t>(win_w);
    ws.height    = static_cast<uint32_t>(win_h);
    ws.dpi_scale = (win_w > 0)
        ? static_cast<float>(fb_w) / static_cast<float>(win_w)
        : 1.0f;
    ws.change_tsc = hal::rdtsc();
    return ws;
}

// ── Window close → InputEvent ─────────────────────────────────────────────────
[[nodiscard]] inline InputEvent
pack_window_close(GLFWwindow* /*window*/) noexcept
{
    InputEvent ev{};
    ev.kind          = InputEvent::Kind::WindowClose;
    ev.timestamp_tsc = hal::rdtsc();
    return ev;
}

// ── Window focus ──────────────────────────────────────────────────────────────
[[nodiscard]] inline InputEvent
pack_window_focus(GLFWwindow* /*window*/, int focused) noexcept
{
    InputEvent ev{};
    ev.kind          = InputEvent::Kind::WindowFocus;
    ev.action        = static_cast<uint8_t>(focused);
    ev.timestamp_tsc = hal::rdtsc();
    return ev;
}

// ── File drop ─────────────────────────────────────────────────────────────────
// Packs count into payload[0..3] as uint32_t; first path pointer not stored
// (paths live in GLFW memory and may not be safe to cache across frames).
[[nodiscard]] inline InputEvent
pack_file_drop(GLFWwindow* /*window*/, int count) noexcept
{
    InputEvent ev{};
    ev.kind          = InputEvent::Kind::FileDrop;
    ev.timestamp_tsc = hal::rdtsc();
    const uint32_t n = static_cast<uint32_t>(count);
    std::memcpy(ev.payload, &n, sizeof(n));
    return ev;
}

} // namespace phyriad::ui::glfw_callbacks
// Made with my soul - Swately <3
