// framework/graph/include/phyriad/graph/Graph.hpp
// Graph<G> concept and shared graph base types.
//
// Graph<G> is satisfied by:
//   StaticGraph<Nodes...>  — compile-time typed graph; schema_hash is consteval.
//   DynamicGraph           — type-erased runtime view backed by SHM descriptor.
//
// The concept captures the three invariants every graph must expose:
//   schema_hash()  — fingerprint of graph topology + HAL version
//   node_count()   — number of nodes in the graph
//   wire_count()   — number of directed wires (edges)
//
// WireId and NodeId are used in WireDescriptor and NodeDescriptor (SchemaDescriptor.hpp).
//
// §3.E, §1.4 of PHASE_H_IMPLEMENTATION_PATTERNS.md

#pragma once
#include <phyriad/schema/SchemaHash.hpp>
#include <phyriad/schema/Error.hpp>
#include <concepts>
#include <cstdint>

namespace phyriad::graph {

// ── WireId ────────────────────────────────────────────────────────────────
using WireId = uint32_t;

// NodeId is the same as phyriad::NodeId (uint32_t), re-exported for convenience.
using NodeId = phyriad::NodeId;

// ── Graph<G> concept ──────────────────────────────────────────────────────
// Minimal interface required of every graph representation.
template <typename G>
concept Graph =
    requires(G const& g) {
        // Compile-time fingerprint of the graph's type topology.
        // Static: can be consteval for StaticGraph, runtime-read for DynamicGraph.
        { G::schema_hash() } noexcept -> std::same_as<schema::Hash128>;

        // Node and wire counts — used when writing/reading SHM descriptors.
        { g.node_count() } noexcept -> std::convertible_to<uint32_t>;
        { g.wire_count() } noexcept -> std::convertible_to<uint32_t>;
    };

} // namespace phyriad::graph
// Made with my soul - Swately <3
