// framework/ui/include/phyriad/ui/RenderNode.hpp
// Variadic-template render node — consumes an implicit WindowState inlet plus
// N user-defined typed state inlets, drives ImGui, publishes RenderStats.
//
// Template parameters:
//   States... — zero or more PodMessage types representing application state.
//               Each type adds one Inlet<States_i> and one cached_states_i slot.
//               Typical usage: RenderNode<AppState> or RenderNode<WorldState, HudState>.
//
// Inlet layout:
//   inlet<0>()   → Inlet<WindowState>&  (implicit — always present)
//   inlet<1>()   → Inlet<States_0>&     (first user state)
//   inlet<2>()   → Inlet<States_1>&     ...
//   inlet_count() = 1 + sizeof...(States)
//
// The WindowState inlet drives resize/HiDPI: when a new WindowState arrives
// with different fb_width, fb_height, or dpi_scale, RenderNode calls
// backend_->resize(fb_width, fb_height, dpi_scale).
// If no WindowState transport is connected, the inlet returns RingEmpty each
// tick which is silently ignored.
//
// Per tick():
//   1. Poll WindowState inlet: if changed, call backend_->resize().
//   2. Poll all user inlets: update cached state (ignore RingEmpty/InvalidHandle).
//      Propagate any other fatal error immediately (before new_frame).
//   3. arena_->reset_frame() — reclaim bump-allocator memory.
//   4. backend_->new_frame() — begin ImGui frame.
//   5. draw_(cached_states_...) — user widget callback (no WindowState arg).
//   6. backend_->end_frame() — collect RenderStats.
//   7. ++frame_id_ → stats.frame_id.
//   8. backend_->present() — swap buffers / vsync block.
//   9. outlet_.publish(stats).
//  10. Return {}.
//
// RenderNode is non-copyable and non-movable (Inlet/Outlet members pin address).
//
#pragma once
#include <phyriad/render/IRenderBackend.hpp>
#include <phyriad/render/FrameArena.hpp>
#include <phyriad/render/RenderStats.hpp>
#include <phyriad/ui/types/WindowState.hpp>
#include <phyriad/node/Port.hpp>
#include <phyriad/schema/Error.hpp>
#include <phyriad/schema/PodMessage.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <tuple>
#include <type_traits>
#include <utility>

namespace phyriad::ui {

template <schema::PodMessage... States>
class RenderNode {
public:
    // ── Type aliases ──────────────────────────────────────────────────────────
    using output_type   = render::RenderStats;
    using widgets_fn_t  = void(*)(const States&...) noexcept;

    // ── Construction ──────────────────────────────────────────────────────────
    RenderNode(render::IRenderBackend& backend,
               render::FrameArena&    arena,
               widgets_fn_t           draw) noexcept
        : backend_(&backend)
        , arena_(&arena)
        , draw_(draw)
    {}

    // Non-copyable, non-movable — Inlet/Outlet members pin their addresses.
    RenderNode(RenderNode const&)            = delete;
    RenderNode& operator=(RenderNode const&) = delete;
    RenderNode(RenderNode&&)                 = delete;
    RenderNode& operator=(RenderNode&&)      = delete;

    // ── Outlet ────────────────────────────────────────────────────────────────
    [[nodiscard]] node::Outlet<render::RenderStats>& outlet() noexcept { return outlet_; }

    // ── Inlets ────────────────────────────────────────────────────────────────
    // Total inlets = 1 (WindowState) + sizeof...(States).
    [[nodiscard]] std::size_t inlet_count() const noexcept {
        return 1u + sizeof...(States);
    }

    // Compile-time typed inlet accessor.
    //   inlet<0>() → Inlet<WindowState>& (implicit HiDPI/resize inlet)
    //   inlet<I>() for I >= 1 → Inlet<States_I-1>&
    template <std::size_t I>
    [[nodiscard]] auto& inlet() noexcept {
        if constexpr (I == 0u) {
            return window_inlet_;
        } else {
            return std::get<I - 1u>(inlets_);
        }
    }

    // Runtime type-erased inlet accessor for NodeHandle wiring.
    //   idx == 0 → &window_inlet_
    //   idx >= 1 → &Inlet<States_idx-1>
    [[nodiscard]] void* inlet_at_erased(std::size_t idx) noexcept {
        if (idx == 0u) return &window_inlet_;
        return inlet_at_impl(idx - 1u, std::index_sequence_for<States...>{});
    }

    // ── Lifecycle ─────────────────────────────────────────────────────────────
    [[nodiscard]] auto on_start() noexcept -> std::expected<void, phyriad::Error> {
        frame_id_  = 0u;
        cached_ws_ = WindowState{};   // clear cached window state on restart
        return {};
    }

    [[nodiscard]] auto on_stop() noexcept -> std::expected<void, phyriad::Error> {
        return {};
    }

    // ── tick — one render frame ───────────────────────────────────────────────
    [[nodiscard]] auto tick() noexcept -> std::expected<void, phyriad::Error> {
        // 1a. Poll WindowState inlet.
        {
            auto ws_r = window_inlet_.receive();
            if (ws_r.has_value()) {
                const auto& ws = *ws_r;
                // Call resize when dimensions or DPI scale change.
                if (ws.fb_width  != cached_ws_.fb_width  ||
                    ws.fb_height != cached_ws_.fb_height ||
                    ws.dpi_scale != cached_ws_.dpi_scale)
                {
                    backend_->resize(ws.fb_width, ws.fb_height, ws.dpi_scale);
                }
                cached_ws_ = ws;
            } else {
                const auto code = ws_r.error().code;
                if (code != ErrorCode::RingEmpty &&
                    code != ErrorCode::InvalidHandle) {
                    return std::unexpected(ws_r.error());  // fatal WindowState error
                }
                // RingEmpty or InvalidHandle — no window state this tick (normal).
            }
        }

        // 1b. Poll all user inlets — update cached states or ignore RingEmpty/InvalidHandle.
        if (auto err = poll_all(std::index_sequence_for<States...>{}); !err)
            return err;

        // 2. Reclaim bump allocator memory for this frame.
        arena_->reset_frame();

        // 3. Begin ImGui frame.
        backend_->new_frame();

        // 4. User widget callback (only user states, not WindowState).
        std::apply([this](const States&... s) noexcept { draw_(s...); },
                   cached_states_);

        // 5. Render + collect stats.
        render::RenderStats stats = backend_->end_frame();

        // 6. Stamp frame_id (overrides backend's internal counter with ours).
        stats.frame_id = ++frame_id_;

        // 7. Present (vsync block).
        backend_->present();

        // 8. Publish stats.
        (void)outlet_.publish(stats);

        return {};
    }

private:
    // ── Inlet polling helpers ─────────────────────────────────────────────────

    // poll_all: iterate user inlets via index_sequence, short-circuit on fatal error.
    template <std::size_t... Is>
    [[nodiscard]] std::expected<void, phyriad::Error>
    poll_all(std::index_sequence<Is...>) noexcept
    {
        if constexpr (sizeof...(Is) == 0u) {
            return {};  // zero user-state node — nothing to poll
        } else {
            std::expected<void, phyriad::Error> result{};

            auto poll_one = [&](auto ic) noexcept -> bool {
                constexpr std::size_t I = decltype(ic)::value;
                auto r = std::get<I>(inlets_).receive();
                if (r.has_value()) {
                    std::get<I>(cached_states_) = std::move(*r);
                    return true;  // continue fold
                }
                const auto code = r.error().code;
                if (code == ErrorCode::RingEmpty ||
                    code == ErrorCode::InvalidHandle) {
                    return true;  // non-fatal — keep cached state
                }
                result = std::unexpected(r.error());  // fatal
                return false;  // abort fold
            };

            // && fold: stops at first false (fatal error).
            (... && poll_one(std::integral_constant<std::size_t, Is>{}));
            return result;
        }
    }

    // inlet_at_impl: runtime-indexed void* to user Inlet<States_I>.
    // (index is already adjusted: 0 → user state 0, etc.)
    template <std::size_t... Is>
    [[nodiscard]] void*
    inlet_at_impl(std::size_t idx, std::index_sequence<Is...>) noexcept
    {
        if constexpr (sizeof...(Is) == 0u) {
            (void)idx;
            return nullptr;  // no user-state inlets
        } else {
            void* result = nullptr;
            // Short-circuit || fold: set result when idx matches, then stop.
            ((idx == Is && (result = &std::get<Is>(inlets_), true)) || ...);
            return result;
        }
    }

    // ── Data members ──────────────────────────────────────────────────────────
    render::IRenderBackend*                      backend_;
    render::FrameArena*                          arena_;
    widgets_fn_t                                 draw_;

    // Implicit WindowState inlet (index 0).
    node::Inlet<WindowState>                     window_inlet_;
    WindowState                                  cached_ws_{};

    // User-state inlets (indices 1..sizeof...(States)).
    std::tuple<node::Inlet<States>...>           inlets_;
    node::Outlet<render::RenderStats>            outlet_;
    std::tuple<States...>                        cached_states_{};
    uint64_t                                     frame_id_{0u};
};

} // namespace phyriad::ui
// Made with my soul - Swately <3
