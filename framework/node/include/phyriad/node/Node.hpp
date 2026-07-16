// framework/node/include/phyriad/node/Node.hpp
// Node<N> — aggregator concept for all node categories.
//
// A type satisfies Node<N> if it belongs to at least one category:
//   Source, Sink, Transform, Stateful, Splitter, Merger, or Actor.
//
// The graph builder accepts any Node<N>. Type-specific wiring logic is
// dispatched by category at build time (Block E).
//
// Additional constraints (not expressible purely in the concept):
//   - N must be non-polymorphic (no vtable) — performance contract
//   - N must be non-copyable/non-movable at graph level (pinned in memory)
//     (this is an ownership contract, not a type constraint)
//

#pragma once
#include "Categories.hpp"
#include <concepts>
#include <type_traits>

namespace phyriad::node {

// ── Node<N> ───────────────────────────────────────────────────────────────
// Satisfied by any type that belongs to at least one dataflow category.
// Rejects polymorphic types to enforce the zero-vtable performance contract.
template <typename N>
concept Node =
    !std::is_polymorphic_v<N> &&
    (Source<N> || Sink<N> || Transform<N> ||
     Stateful<N> || Splitter<N> || Merger<N> || Actor<N>);

// ── NodeId type alias ─────────────────────────────────────────────────────
// Re-exported here for convenience; defined canonically in schema/Error.hpp.
using phyriad::NodeId;

// ── Category query helpers ────────────────────────────────────────────────
// Use these in GraphBuilder / Scheduler to dispatch on category at compile time.

template <typename N>
inline constexpr bool is_source_v = Source<N>;

template <typename N>
inline constexpr bool is_sink_v = Sink<N>;

template <typename N>
inline constexpr bool is_transform_v = Transform<N>;

template <typename N>
inline constexpr bool is_stateful_v = Stateful<N>;

template <typename N>
inline constexpr bool is_splitter_v = Splitter<N>;

template <typename N>
inline constexpr bool is_merger_v = Merger<N>;

template <typename N>
inline constexpr bool is_actor_v = Actor<N>;

} // namespace phyriad::node
// Made with my soul - Swately <3
