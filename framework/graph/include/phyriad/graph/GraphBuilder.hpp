// framework/graph/include/phyriad/graph/GraphBuilder.hpp
// Fluent builder: wires Outlet<T> to Inlet<T> and records WireDescriptors.
//
// GraphBuilder<G> takes a graph by reference and provides wire() / wire_ring()
// to establish live connections between node ports. After all wires are added,
// build() emits the GraphSchemaDescriptor that callers write to SHM.
//
// GraphBuilder calls the PRIVATE connect() methods of Outlet<T> and Inlet<T>
// via the friend declaration established in Port.hpp.
//
// Usage (static graph):
//   StaticGraph<CanonicalSource, CanonicalTransform> graph{};
//   transport::Latest<SampleTick> wire{};
//   GraphBuilder builder{graph};
//   builder.wire<0, 1>(wire);        // source[0].outlet → transform[1].inlet
//   auto desc = builder.build();
//
// Usage (ring wire):
//   transport::Ring<SampleTick, 1024> ring{};
//   builder.wire_ring<0, 1>(ring);   // subscribes inlet automatically
//
// wire() is chainable (returns *this).
// build() can be called multiple times — returns the same descriptor each time.
//
// §3.D, §3.E of PHASE_H_IMPLEMENTATION_PATTERNS.md

#pragma once
#include "Graph.hpp"
#include <phyriad/node/Port.hpp>
#include <phyriad/transport/Ring.hpp>
#include <phyriad/schema/SchemaDescriptor.hpp>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace phyriad::graph {

template <typename G>
class GraphBuilder {
public:
    explicit GraphBuilder(G& graph) noexcept : graph_(graph) {}

    // ── wire ──────────────────────────────────────────────────────────────
    // Wire the outlet of node[SrcIdx] to the inlet of node[DstIdx] via
    // an already-owned transport Tr.
    //
    // Tr must satisfy:
    //   - tr.send(T const&) noexcept → Outlet<T>::connect() accepts it
    //   - tr.receive()      noexcept → Inlet<T>::connect() accepts it
    //   - Tr::type_hash static member → recorded in WireDescriptor
    //
    // Both the outlet and inlet are wired in a single call. If either is
    // already connected, the old connection is silently replaced.
    template <std::size_t SrcIdx, std::size_t DstIdx, typename Tr>
        requires requires {
            Tr::type_hash;  // all Phyriad transport types expose this
        }
    GraphBuilder& wire(Tr& transport) noexcept {
        // Access node ports — calls private get<I>() on StaticGraph.
        auto& out = graph_.template get<SrcIdx>().outlet();
        auto& in  = graph_.template get<DstIdx>().inlet();

        // Establish live connections (accesses private connect() via friend).
        out.connect(transport);
        in.connect(transport);

        // Record the wire for descriptor generation.
        schema::WireDescriptor wd{};
        wd.message_hash    = Tr::type_hash;
        wd.capacity        = 0;   // not a ring — no fixed capacity
        wd.slot_size_bytes = 0;
        wd.src_node_id     = static_cast<uint32_t>(SrcIdx);
        wd.dst_node_id     = static_cast<uint32_t>(DstIdx);
        wd.wire_id         = next_wire_id_++;
        wire_descs_.push_back(wd);
        ++graph_.wire_count_;
        return *this;
    }

    // ── wire_ring ─────────────────────────────────────────────────────────
    // Wire outlet[SrcIdx] → inlet[DstIdx] via Ring<T,Cap>.
    // Inlet<T>::connect(Ring<T,Cap>&) subscribes and embeds the RingHandle.
    // Outlet<T>::connect(Ring<T,Cap>&) uses Ring::send() directly (no handle needed).
    template <std::size_t SrcIdx, std::size_t DstIdx,
              schema::PodMessage T, std::size_t Cap>
    GraphBuilder& wire_ring(transport::Ring<T, Cap>& ring) noexcept {
        auto& out = graph_.template get<SrcIdx>().outlet();
        auto& in  = graph_.template get<DstIdx>().inlet();

        out.connect(ring);
        in.connect(ring);

        schema::WireDescriptor wd{};
        wd.message_hash    = transport::Ring<T, Cap>::type_hash;
        wd.capacity        = static_cast<uint32_t>(Cap);
        wd.slot_size_bytes = static_cast<uint32_t>(sizeof(T));
        wd.src_node_id     = static_cast<uint32_t>(SrcIdx);
        wd.dst_node_id     = static_cast<uint32_t>(DstIdx);
        wd.wire_id         = next_wire_id_++;
        wire_descs_.push_back(wd);
        ++graph_.wire_count_;
        return *this;
    }

    // ── build ─────────────────────────────────────────────────────────────
    // Emit the GraphSchemaDescriptor.
    // graph_hash = G::schema_hash() (compile-time fingerprint of node types
    // + HAL constants). Wire topology correctness is guaranteed at compile
    // time by connect()'s type constraints — Outlet<T> only accepts transports
    // whose message type is T.
    [[nodiscard]] schema::GraphSchemaDescriptor build() noexcept {
        // consteval hash evaluated at compile time; embedded as constant here.
        static constexpr schema::Hash128 kHash = G::schema_hash();
        return schema::make_schema_descriptor(
            kHash,
            graph_.node_count(),
            graph_.wire_count());
    }

    // Access the recorded wire descriptors (for GraphValidator).
    [[nodiscard]] std::vector<schema::WireDescriptor> const& wires() const noexcept {
        return wire_descs_;
    }

private:
    G&                                    graph_;
    std::vector<schema::WireDescriptor>   wire_descs_{};
    WireId                                next_wire_id_{0};
};

// ── Deduction guide ───────────────────────────────────────────────────────
// Allows: GraphBuilder builder{graph};  (without explicit template arg)
template <typename G>
GraphBuilder(G&) -> GraphBuilder<G>;

} // namespace phyriad::graph
// Made with my soul - Swately <3
