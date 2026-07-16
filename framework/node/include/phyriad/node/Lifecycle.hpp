// framework/node/include/phyriad/node/Lifecycle.hpp
// Concept-based duck-typed lifecycle hooks and DataflowFn enforcement.
//
// Lifecycle hooks are OPTIONAL. A node implements only what it needs.
// The graph runtime calls try_start(), try_stop(), try_pause(), try_resume(),
// try_tick() — each dispatches to the real method if present, else no-ops.
//
// Optional hook signatures (must be noexcept):
//   on_start()  -> std::expected<void, phyriad::Error>   // called once at startup
//   on_stop()   -> std::expected<void, phyriad::Error>   // called once at shutdown
//   on_pause()  -> std::expected<void, phyriad::Error>   // called when node is suspended
//   on_resume() -> std::expected<void, phyriad::Error>   // called when node is resumed
//   on_tick()   -> std::expected<void, phyriad::Error>   // called each scheduler cycle
//
// Checkpointable concept: opt-in for nodes with checkpointable state.
// Implementation lives in Block G (Orchestration); concept defined here so
// Categories and Node can reference it without pulling in orchestration.
//
// DataflowFn concept: enforces that hot-path callables never throw.
//

#pragma once
#include <phyriad/schema/Error.hpp>
#include <phyriad/schema/PodMessage.hpp>
#include <concepts>
#include <expected>
#include <type_traits>

namespace phyriad::node {

// ── Optional lifecycle hook concepts ─────────────────────────────────────

template <typename N>
concept HasOnStart = requires(N& node) {
    { node.on_start() } noexcept
        -> std::same_as<std::expected<void, phyriad::Error>>;
};

template <typename N>
concept HasOnStop = requires(N& node) {
    { node.on_stop() } noexcept
        -> std::same_as<std::expected<void, phyriad::Error>>;
};

template <typename N>
concept HasOnPause = requires(N& node) {
    { node.on_pause() } noexcept
        -> std::same_as<std::expected<void, phyriad::Error>>;
};

template <typename N>
concept HasOnResume = requires(N& node) {
    { node.on_resume() } noexcept
        -> std::same_as<std::expected<void, phyriad::Error>>;
};

template <typename N>
concept HasOnTick = requires(N& node) {
    { node.on_tick() } noexcept
        -> std::same_as<std::expected<void, phyriad::Error>>;
};

// ── try_* free functions — dispatch to hook if present, else no-op ────────

template <typename N>
[[nodiscard]] auto try_start(N& node) noexcept
    -> std::expected<void, phyriad::Error>
{
    if constexpr (HasOnStart<N>) {
        return node.on_start();
    } else {
        return {};
    }
}

template <typename N>
[[nodiscard]] auto try_stop(N& node) noexcept
    -> std::expected<void, phyriad::Error>
{
    if constexpr (HasOnStop<N>) {
        return node.on_stop();
    } else {
        return {};
    }
}

template <typename N>
[[nodiscard]] auto try_pause(N& node) noexcept
    -> std::expected<void, phyriad::Error>
{
    if constexpr (HasOnPause<N>) {
        return node.on_pause();
    } else {
        return {};
    }
}

template <typename N>
[[nodiscard]] auto try_resume(N& node) noexcept
    -> std::expected<void, phyriad::Error>
{
    if constexpr (HasOnResume<N>) {
        return node.on_resume();
    } else {
        return {};
    }
}

template <typename N>
[[nodiscard]] auto try_tick(N& node) noexcept
    -> std::expected<void, phyriad::Error>
{
    if constexpr (HasOnTick<N>) {
        return node.on_tick();
    } else {
        return {};
    }
}

// ── Checkpointable concept (impl in Block G) ─────────────────────────────
// A node is Checkpointable if it exposes:
//   state_type      — trivially-copyable snapshot of all mutable state
//   state()         — const snapshot accessor
//   restore_state() — deterministic state restore (used after checkpoint load)
template <typename N>
concept Checkpointable = requires(N& node, typename N::state_type const& snap) {
    typename N::state_type;
    requires std::is_trivially_copyable_v<typename N::state_type>;
    { node.state()           } noexcept -> std::same_as<typename N::state_type const&>;
    { node.restore_state(snap) } noexcept;
};

// ── DataflowFn concept ────────────────────────────────────────────────────
// Any callable on the hot path must be nothrow and return std::expected.
// Enforced here at the concept level; checked in Block G's Supervisor::wire().
template <typename F>
concept DataflowFn =
    std::is_nothrow_invocable_v<F> &&
    requires { requires !std::is_void_v<std::invoke_result_t<F>>; };

} // namespace phyriad::node
// Made with my soul - Swately <3
