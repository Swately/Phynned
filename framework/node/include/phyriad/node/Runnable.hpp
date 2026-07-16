// framework/node/include/phyriad/node/Runnable.hpp
// Runnable<N> — concept for nodes that the GraphRuntime can drive directly.
//
// A Runnable node:
//   1. Satisfies Node<N>  — belongs to at least one dataflow category.
//   2. Is default_constructible — GraphRuntime allocates and constructs it.
//   3. Has tick() noexcept → expected<void, Error> — the hot-path entry point.
//
// tick() semantics by category:
//   Source    — generates and publishes one message via outlet.
//   Transform — reads one message from inlet, processes, publishes to outlet.
//               Returns RingEmpty if no message is available (non-fatal).
//   Sink      — reads one message from inlet and consumes it.
//               Returns RingEmpty if no message is available (non-fatal).
//   Actor     — general tick; defines its own semantics.
//
// CanonicalSource and CanonicalTransform add tick() wrappers — see
// pillars/node/include/phyriad/node/canonical/ for concrete examples.
//

#pragma once
#include "Node.hpp"
#include <phyriad/schema/Error.hpp>
#include <concepts>
#include <expected>
#include <type_traits>

namespace phyriad {

// ── Runnable<N> ───────────────────────────────────────────────────────────────
template <typename N>
concept Runnable =
    node::Node<N> &&
    std::is_default_constructible_v<N> &&
    requires(N& n) {
        { n.tick() } noexcept -> std::same_as<std::expected<void, phyriad::Error>>;
    };

// ── WrappableNode<N> ─────────────────────────────────────────────────────────
// Like Runnable<N> but WITHOUT the default-constructibility requirement.
// Used by NodeHandle::wrap() for externally-owned, non-default-constructible
// nodes (e.g. UIThreadNode, RenderNode<States...>).
template <typename N>
concept WrappableNode =
    node::Node<N> &&
    requires(N& n) {
        { n.tick() } noexcept -> std::same_as<std::expected<void, phyriad::Error>>;
    };

} // namespace phyriad
// Made with my soul - Swately <3
