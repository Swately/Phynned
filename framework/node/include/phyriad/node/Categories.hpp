// framework/node/include/phyriad/node/Categories.hpp
// Node category concepts: Source, Sink, Transform, Stateful,
//                         Splitter, Merger, Actor.
//
// Categories describe the dataflow role of a node. The graph builder uses them
// to validate connection legality at compile time (e.g., only Source → Sink).
//
// Source<N>    — produces messages of N::output_type; exposes outlet().
// Sink<N>      — consumes messages of N::input_type via on_message(); exposes inlet().
// Transform<N> — Source AND Sink with matching or convertible types.
// Stateful<N>  — any category PLUS opt-in checkpointing via state_type.
// Splitter<N>  — one inlet, multiple outlets (fan-out). outlet_count() advertises N.
// Merger<N>    — multiple inlets (fan-in), one outlet. inlet_count() advertises N.
// Actor<N>     — general purpose: has a tick() method; may have any port layout.
//
// A node may satisfy multiple categories (e.g., a Stateful Transform).
// Node<N> (Node.hpp) is the aggregator that accepts any of these.
//
// NOTE: Splitter and Merger concepts use outlet_count()/inlet_count() to
// advertise port cardinality. The actual port accessors (outlet(i), inlet(i))
// are accessed by the GraphBuilder via index; concepts here check only the
// count accessor to keep concept complexity manageable. Full wire validation
// happens in Block E (GraphValidator).
//

#pragma once
#include "Port.hpp"
#include "Lifecycle.hpp"
#include <phyriad/schema/PodMessage.hpp>
#include <concepts>
#include <cstddef>
#include <expected>
#include <type_traits>

namespace phyriad::node {

// ── Source<N> ─────────────────────────────────────────────────────────────
// A node that emits messages. Must expose:
//   output_type              — the message type produced
//   Outlet<output_type>& outlet() noexcept
template <typename N>
concept Source =
    requires {
        typename N::output_type;
        requires schema::PodMessage<typename N::output_type>;
    } &&
    requires(N& n) {
        { n.outlet() } noexcept
            -> std::same_as<Outlet<typename N::output_type>&>;
    };

// ── Sink<N> ───────────────────────────────────────────────────────────────
// A node that consumes messages. Must expose:
//   input_type               — the message type consumed
//   Inlet<input_type>& inlet() noexcept
//   on_message(input_type const&) noexcept -> expected<void, Error>
template <typename N>
concept Sink =
    requires {
        typename N::input_type;
        requires schema::PodMessage<typename N::input_type>;
    } &&
    requires(N& n, typename N::input_type const& msg) {
        { n.inlet() } noexcept
            -> std::same_as<Inlet<typename N::input_type>&>;
        { n.on_message(msg) } noexcept
            -> std::same_as<std::expected<void, phyriad::Error>>;
    };

// ── Transform<N> ──────────────────────────────────────────────────────────
// A node that is both a Source and a Sink.
// The input_type and output_type MAY differ (transformation is allowed).
template <typename N>
concept Transform = Source<N> && Sink<N>;

// ── Stateful<N> ───────────────────────────────────────────────────────────
// A node whose mutable state can be snapshotted and restored.
// Satisfies Checkpointable<N>; may be combined with any other category.
// Implementation of checkpoint mechanics lives in Block G (Orchestration).
template <typename N>
concept Stateful = Checkpointable<N>;

// ── Splitter<N> ───────────────────────────────────────────────────────────
// A node that fans out one input to multiple outputs.
// Must be a Sink and advertise the number of outlets via outlet_count().
// Individual outlets are accessed by GraphBuilder via outlet(std::size_t i).
template <typename N>
concept Splitter =
    Sink<N> &&
    requires(N& n) {
        { n.outlet_count() } noexcept -> std::convertible_to<std::size_t>;
    };

// ── Merger<N> ─────────────────────────────────────────────────────────────
// A node that fans in multiple inputs to one output.
// Must be a Source and advertise the number of inlets via inlet_count().
// Individual inlets are accessed by GraphBuilder via inlet(std::size_t i).
template <typename N>
concept Merger =
    Source<N> &&
    requires(N& n) {
        { n.inlet_count() } noexcept -> std::convertible_to<std::size_t>;
    };

// ── Actor<N> ──────────────────────────────────────────────────────────────
// A general-purpose node driven by the scheduler's tick loop.
// Does not prescribe port structure; owns its own dataflow internally.
// May also satisfy Source, Sink, etc. simultaneously.
template <typename N>
concept Actor =
    requires(N& n) {
        { n.tick() } noexcept
            -> std::same_as<std::expected<void, phyriad::Error>>;
    };

} // namespace phyriad::node
// Made with my soul - Swately <3
