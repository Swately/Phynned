// framework/graph/include/phyriad/graph/StaticGraph.hpp
// Compile-time typed graph — all node types known as template parameters.
//
// StaticGraph<Nodes...> owns its nodes in a std::tuple<Nodes...>.
// The graph topology (node types + their port types) is fingerprinted at
// compile time via consteval schema_hash().
//
// schema_hash() mixes:
//   - schema_hash<N>() for each node type N (covers sizeof, alignof, type name)
//   - kPhyriadHalVersion (detects HAL layout changes)
//   - kDestructivePad  (detects cache-line constant changes)
// Any change to a node type, port type, or HAL constants produces a different
// hash → DynamicGraph::attach() rejects stale SHM regions immediately.
//
// Wire connections are established by GraphBuilder<StaticGraph<...>> at
// graph-build time. GraphBuilder calls the private connect() methods on
// Outlet<T> and Inlet<T> (friendships declared in Port.hpp).
//
// Usage:
//   StaticGraph<CanonicalSource, CanonicalTransform> graph{};
//   GraphBuilder builder{graph};
//   Latest<SampleTick> wire{};
//   builder.wire<0, 1>(wire);  // source outlet → transform inlet
//   auto desc = builder.build();
//
// §3.E, §1.4 of PHASE_H_IMPLEMENTATION_PATTERNS.md

#pragma once
#include "Graph.hpp"
#include <phyriad/node/Node.hpp>
#include <phyriad/schema/SchemaDescriptor.hpp>
#include <phyriad/hal/Cacheline.hpp>
#include <cstdint>
#include <tuple>
#include <type_traits>

namespace phyriad::graph {

// Forward-declare GraphBuilder to grant friendship.
template <typename> class GraphBuilder;

// ── StaticGraph<Nodes...> ─────────────────────────────────────────────────
template <typename... Nodes>
    requires (node::Node<Nodes> && ...) && (sizeof...(Nodes) > 0)
class StaticGraph {
public:
    static constexpr uint32_t kNodeCount = static_cast<uint32_t>(sizeof...(Nodes));

    StaticGraph() = default;

    // Non-copyable, non-movable — nodes embed ports (Inlet<T> is non-movable).
    StaticGraph(StaticGraph const&)            = delete;
    StaticGraph& operator=(StaticGraph const&) = delete;
    StaticGraph(StaticGraph&&)                 = delete;
    StaticGraph& operator=(StaticGraph&&)      = delete;

    // ── Compile-time schema fingerprint ──────────────────────────────────
    // Mixes schema_hash<N>() of every node type, then HAL version + pad size.
    // Baked into the binary — any node type change → different hash.
    [[nodiscard]] static consteval schema::Hash128 schema_hash() noexcept {
        schema::XXH3State s{};
        (s.update(schema::schema_hash<Nodes>()), ...);  // fold over each node type
        s.update(schema::kPhyriadHalVersion);
        s.update(static_cast<uint64_t>(hal::kDestructivePad));
        return s.digest_128();
    }

    [[nodiscard]] constexpr uint32_t node_count() const noexcept {
        return kNodeCount;
    }

    [[nodiscard]] uint32_t wire_count() const noexcept {
        return wire_count_;
    }

    // ── Node access by index (for GraphBuilder) ───────────────────────────
    template <std::size_t I>
        requires (I < sizeof...(Nodes))
    [[nodiscard]] auto& get() noexcept {
        return std::get<I>(nodes_);
    }

    template <std::size_t I>
        requires (I < sizeof...(Nodes))
    [[nodiscard]] auto const& get() const noexcept {
        return std::get<I>(nodes_);
    }

    // ── Descriptor factory ────────────────────────────────────────────────
    // Builds the GraphSchemaDescriptor from the compile-time hash + counts.
    // Called by GraphBuilder::build() after wiring is complete.
    [[nodiscard]] schema::GraphSchemaDescriptor build_descriptor() const noexcept {
        return schema::make_schema_descriptor(schema_hash(), kNodeCount, wire_count_);
    }

private:
    std::tuple<Nodes...> nodes_{};
    uint32_t             wire_count_{0};

    template <typename> friend class GraphBuilder;
};

} // namespace phyriad::graph
// Made with my soul - Swately <3
