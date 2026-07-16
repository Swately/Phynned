// framework/ui/include/phyriad/ui/UIThreadNode.hpp
// GLFW-driven source node — phyriad::ui input pipeline entry point.
//
// UIThreadNode receives a GLFWwindow* (it does NOT create the window).
// It runs on ThreadRole::UI_MAIN and is driven by the main loop via tick().
//
// Per tick():
//   1. glfwPollEvents() — fires all pending GLFW callbacks synchronously.
//   2. Drain ev_buf_[0..ev_count_-1]  → input_outlet_.publish() for each event.
//   3. If pending_ws_                  → window_outlet_.publish(pending_ws_val_).
//   4. If glfwWindowShouldClose(w)     → return Error{WindowClosed}.
//   5. Else                            → return {}.
//
// All GLFW callbacks are static member functions. They recover the node via
// glfwGetWindowUserPointer(window), then delegate to push_event() /
// push_window_state(). These run synchronously inside glfwPollEvents()
// on the UI thread — no atomics are needed for the event buffer.
//
// Buffer overflow (> kBufCap events per tick) silently discards the excess.
// Typical polling at 60 Hz leaves the buffer empty most ticks.
//
// Outlet layout:
//   outlet()              / outlet_at_erased(0) → Outlet<InputEvent>&
//   window_outlet()       / outlet_at_erased(1) → Outlet<WindowState>&
//   outlet_count()                              → 2
//
// Non-copyable, non-movable: Outlet<> members pin their address, and the
// GLFW user pointer points to *this.
//
#pragma once
#include <phyriad/ui/types/InputEvent.hpp>
#include <phyriad/ui/types/WindowState.hpp>
#include <phyriad/ui/glfw_callbacks.hpp>
#include <phyriad/node/Port.hpp>
#include <phyriad/schema/Error.hpp>
#include <phyriad/hal/Timestamp.hpp>
#include <GLFW/glfw3.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>

namespace phyriad::ui {

class UIThreadNode {
public:
    // ── Type aliases (Source-compatible) ──────────────────────────────────────
    using output_type       = InputEvent;
    using window_state_type = WindowState;

    // ── Construction / Destruction ────────────────────────────────────────────
    explicit UIThreadNode(GLFWwindow* window) noexcept
        : window_(window), ev_count_(0u), pending_ws_(false)
    {
        if (window_) {
            glfwSetWindowUserPointer(window_, this);
            register_callbacks();
        }
    }

    ~UIThreadNode() noexcept {
        if (window_) {
            unregister_callbacks();
            glfwSetWindowUserPointer(window_, nullptr);
        }
    }

    // Non-copyable, non-movable — Outlet<> members pin their address;
    // GLFW user pointer points to *this.
    UIThreadNode(UIThreadNode const&)            = delete;
    UIThreadNode& operator=(UIThreadNode const&) = delete;
    UIThreadNode(UIThreadNode&&)                 = delete;
    UIThreadNode& operator=(UIThreadNode&&)      = delete;

    // ── Multi-outlet accessors ────────────────────────────────────────────────
    [[nodiscard]] node::Outlet<InputEvent>&  outlet()        noexcept { return input_outlet_;  }
    [[nodiscard]] node::Outlet<WindowState>& window_outlet() noexcept { return window_outlet_; }
    [[nodiscard]] std::size_t outlet_count() const noexcept { return 2u; }

    // Type-erased outlet accessor for NodeHandle wiring (returns void*).
    [[nodiscard]] void* outlet_at_erased(std::size_t i) noexcept {
        if (i == 0u) return &input_outlet_;
        if (i == 1u) return &window_outlet_;
        return nullptr;
    }

    // ── Lifecycle hooks ───────────────────────────────────────────────────────
    [[nodiscard]] auto on_start() noexcept -> std::expected<void, phyriad::Error> {
        // Do NOT call register_callbacks() here.
        // The constructor installs the raw GLFW callbacks, and then
        // Application::run() calls ImGui_ImplGlfw_InitForOpenGL(..., true)
        // which wraps them in ImGui chaining callbacks. Re-registering here
        // would overwrite those chaining wrappers and break ImGui input.
        return {};
    }

    [[nodiscard]] auto on_stop() noexcept -> std::expected<void, phyriad::Error> {
        if (window_) unregister_callbacks();
        return {};
    }

    // ── tick — one frame of GLFW polling ─────────────────────────────────────
    [[nodiscard]] auto tick() noexcept -> std::expected<void, phyriad::Error> {
        ev_count_   = 0u;
        pending_ws_ = false;

        if (first_tick_ && window_) {
            first_tick_ = false;
            int fb_w = 0, fb_h = 0;
            glfwGetFramebufferSize(window_, &fb_w, &fb_h);
            pending_ws_val_ = glfw_callbacks::pack_framebuffer_size(
                window_, fb_w, fb_h);
            pending_ws_ = true;
        }

        glfwPollEvents();   // fires callbacks → push_event() / push_window_state()

        // Publish buffered input events.
        for (uint32_t i = 0u; i < ev_count_; ++i) {
            (void)input_outlet_.publish(ev_buf_[i]);
        }
        ev_count_ = 0u;

        // Publish pending window state change.
        if (pending_ws_) {
            (void)window_outlet_.publish(pending_ws_val_);
            pending_ws_ = false;
        }

        // Window close → signal shutdown to the main loop.
        if (window_ && glfwWindowShouldClose(window_)) {
            return std::unexpected(phyriad::Error{
                .code           = ErrorCode::WindowClosed,
                .source_node_id = 0u,
                .timestamp_ns   = static_cast<uint64_t>(hal::rdtsc())});
        }

        return {};
    }

    // ── Called from GLFW callbacks (package-internal) ─────────────────────────
    void push_event(InputEvent const& ev) noexcept {
        if (ev_count_ < kBufCap) ev_buf_[ev_count_++] = ev;
        // Overflow: excess events are silently dropped (rare at 60 Hz).
    }

    void push_window_state(WindowState const& ws) noexcept {
        pending_ws_val_ = ws;
        pending_ws_     = true;
    }

private:
    static constexpr uint32_t kBufCap = 32u;

    // ── GLFW callback stubs ───────────────────────────────────────────────────
    [[nodiscard]] static UIThreadNode* self(GLFWwindow* w) noexcept {
        return static_cast<UIThreadNode*>(glfwGetWindowUserPointer(w));
    }

    static void cb_mouse_button(GLFWwindow* w,
                                int btn, int act, int mods) noexcept {
        if (auto* n = self(w))
            n->push_event(glfw_callbacks::pack_mouse_button(w, btn, act, mods));
    }
    static void cb_cursor_pos(GLFWwindow* w, double x, double y) noexcept {
        if (auto* n = self(w))
            n->push_event(glfw_callbacks::pack_cursor_pos(w, x, y));
    }
    static void cb_key(GLFWwindow* w,
                       int key, int sc, int act, int mods) noexcept {
        if (auto* n = self(w))
            n->push_event(glfw_callbacks::pack_key(w, key, sc, act, mods));
    }
    static void cb_char(GLFWwindow* w, unsigned int cp) noexcept {
        if (auto* n = self(w))
            n->push_event(glfw_callbacks::pack_char(w, cp));
    }
    static void cb_scroll(GLFWwindow* w, double dx, double dy) noexcept {
        if (auto* n = self(w))
            n->push_event(glfw_callbacks::pack_scroll(w, dx, dy));
    }
    static void cb_cursor_enter(GLFWwindow* w, int entered) noexcept {
        if (auto* n = self(w))
            n->push_event(glfw_callbacks::pack_cursor_enter(w, entered));
    }
    static void cb_framebuffer_size(GLFWwindow* w, int fw, int fh) noexcept {
        if (auto* n = self(w))
            n->push_window_state(glfw_callbacks::pack_framebuffer_size(w, fw, fh));
    }
    static void cb_window_close(GLFWwindow* w) noexcept {
        if (auto* n = self(w))
            n->push_event(glfw_callbacks::pack_window_close(w));
    }
    static void cb_window_focus(GLFWwindow* w, int focused) noexcept {
        if (auto* n = self(w))
            n->push_event(glfw_callbacks::pack_window_focus(w, focused));
    }
    static void cb_file_drop(GLFWwindow* w,
                             int count, const char** /*paths*/) noexcept {
        if (auto* n = self(w))
            n->push_event(glfw_callbacks::pack_file_drop(w, count));
    }
    static void cb_window_refresh(GLFWwindow* w) noexcept {
        // No data published — only forces glfwPollEvents() to return during
        // the modal resize loop on Windows so UIThreadNode continues ticking.
        (void)w;
    }

    // ── Callback registration / deregistration ────────────────────────────────
    void register_callbacks() noexcept {
        glfwSetMouseButtonCallback    (window_, cb_mouse_button);
        glfwSetCursorPosCallback      (window_, cb_cursor_pos);
        glfwSetKeyCallback            (window_, cb_key);
        glfwSetCharCallback           (window_, cb_char);
        glfwSetScrollCallback         (window_, cb_scroll);
        glfwSetCursorEnterCallback    (window_, cb_cursor_enter);
        glfwSetFramebufferSizeCallback(window_, cb_framebuffer_size);
        glfwSetWindowCloseCallback    (window_, cb_window_close);
        glfwSetWindowFocusCallback    (window_, cb_window_focus);
        glfwSetDropCallback           (window_, cb_file_drop);
        glfwSetWindowRefreshCallback  (window_, cb_window_refresh);
    }

    void unregister_callbacks() noexcept {
        glfwSetMouseButtonCallback    (window_, nullptr);
        glfwSetCursorPosCallback      (window_, nullptr);
        glfwSetKeyCallback            (window_, nullptr);
        glfwSetCharCallback           (window_, nullptr);
        glfwSetScrollCallback         (window_, nullptr);
        glfwSetCursorEnterCallback    (window_, nullptr);
        glfwSetFramebufferSizeCallback(window_, nullptr);
        glfwSetWindowCloseCallback    (window_, nullptr);
        glfwSetWindowFocusCallback    (window_, nullptr);
        glfwSetDropCallback           (window_, nullptr);
        glfwSetWindowRefreshCallback  (window_, nullptr);
    }

    // ── Data members ──────────────────────────────────────────────────────────
    GLFWwindow*                       window_;
    node::Outlet<InputEvent>          input_outlet_;
    node::Outlet<WindowState>         window_outlet_;
    std::array<InputEvent, kBufCap>   ev_buf_{};
    uint32_t                          ev_count_{0u};
    bool                              pending_ws_{false};
    bool                              first_tick_{true};
    WindowState                       pending_ws_val_{};
};

} // namespace phyriad::ui
// Made with my soul - Swately <3
