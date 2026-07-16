// framework/graph/include/phyriad/api/GraphDSL.hpp
// Fluent DSL for constructing BuiltGraph descriptors.
//
// DslGraphBuilder is the entry point:
//   auto g = build_graph()
//       .node<MySource>("source", placement::vcache_pinned())
//       .wire("source").to("sink")
//       .validate()
//       .build();
//
// build() returns std::expected<BuiltGraph, ConfigError>.
// BuiltGraph::write_shm() serialises the descriptor to a SHM region.
//
// §3.H of PHASE_H_IMPLEMENTATION_PATTERNS.md

#pragma once
#include "Validation.hpp"
#include "NodeBuilder.hpp"
#include "WireBuilder.hpp"
#include <phyriad/schema/SchemaHash.hpp>
#include <phyriad/schema/SchemaDescriptor.hpp>
#include <phyriad/graph/GraphValidator.hpp>
#include <phyriad/scheduler/Placement.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace phyriad::api {

struct BuiltGraph {
    schema::GraphSchemaDescriptor header{};
    std::vector<schema::NodeDescriptor> nodes{};
    std::vector<schema::WireDescriptor> wires{};
    std::vector<scheduler::PlacementHint> hints{};

    [[nodiscard]] std::size_t shm_size() const noexcept {
        return sizeof(schema::GraphSchemaDescriptor)
            + nodes.size() * sizeof(schema::NodeDescriptor)
            + wires.size() * sizeof(schema::WireDescriptor);
    }

    void write_shm(void* dest) const noexcept {
        auto* p = static_cast<std::byte*>(dest);
        std::memcpy(p, &header, sizeof(header));
        p += sizeof(header);

        for (auto const& nd : nodes) {
            std::memcpy(p, &nd, sizeof(nd));
            p += sizeof(nd);
        }

        for (auto const& wd : wires) {
            std::memcpy(p, &wd, sizeof(wd));
            p += sizeof(wd);
        }
    }
};

class DslGraphBuilder {
public:
    DslGraphBuilder() noexcept = default;

    DslGraphBuilder(DslGraphBuilder const&) = delete;
    DslGraphBuilder& operator=(DslGraphBuilder const&) = delete;
    DslGraphBuilder(DslGraphBuilder&&) = default;
    DslGraphBuilder& operator=(DslGraphBuilder&&) = default;

    template <typename N>
    DslGraphBuilder& node(std::string_view name, scheduler::PlacementHint hint = {}) {
        NodeEntry_ e{};
        e.name    = std::string{name};
        e.node_id = static_cast<uint32_t>(node_entries_.size());
        e.hint    = hint;

        if constexpr (detail::HasOutputType<N>) {
            e.output_hash = schema::schema_hash<typename N::output_type>();
            e.outlet_hashes.push_back(e.output_hash);  // outlet 0
        }

        // Detect second outlet (window_state_type — UIThreadNode pattern)
        if constexpr (requires { typename N::window_state_type; }) {
            e.outlet_hashes.push_back(
                schema::schema_hash<typename N::window_state_type>());  // outlet 1
        }

        if constexpr (detail::HasInputType<N>)
            e.input_hash = schema::schema_hash<typename N::input_type>();

        node_entries_.push_back(std::move(e));
        return *this;
    }

    [[nodiscard]]
    WireBuilder wire(std::string_view from_name, uint32_t outlet = 0) noexcept {
        return WireBuilder{from_name, outlet, *this};
    }

    DslGraphBuilder& validate() noexcept {
        validate_requested_ = true;
        return *this;
    }

    [[nodiscard]]
    std::expected<BuiltGraph, ConfigError> build() noexcept {
        return do_build_();
    }

private:
    struct NodeEntry_ {
        std::string name{};
        uint32_t node_id{0};
        schema::Hash128 output_hash{};
        schema::Hash128 input_hash{};
        scheduler::PlacementHint hint{};
        std::vector<schema::Hash128> outlet_hashes{};
    };

    struct WireEntry_ {
        std::string from_name{};
        std::string to_name{};

        uint32_t from_outlet{0};
        uint32_t to_inlet{0};

        WirePolicy policy{WirePolicy::StrictWaitOrEvict};

        uint32_t from_id{UINT32_MAX};
        uint32_t to_id{UINT32_MAX};
    };

    std::vector<NodeEntry_> node_entries_{};
    std::vector<WireEntry_> wire_entries_{};
    bool validate_requested_{false};

    friend class WireBuilder;

    void add_wire_(
        std::string_view from,
        uint32_t from_outlet,
        std::string_view to,
        uint32_t to_inlet,
        WirePolicy p) noexcept
    {
        WireEntry_ e{};
        e.from_name = std::string{from};
        e.to_name = std::string{to};
        e.from_outlet = from_outlet;
        e.to_inlet = to_inlet;
        e.policy = p;
        wire_entries_.push_back(std::move(e));
    }

    static schema::Hash128 compute_graph_hash_(
        std::vector<NodeEntry_> const& nodes,
        std::vector<WireEntry_> const& wires) noexcept
    {
        constexpr uint64_t kP1 = 0x9E3779B185EBCA87ULL;
        constexpr uint64_t kP2 = 0xC2B2AE3D27D4EB4FULL;

        uint64_t lo = kP2;
        uint64_t hi = kP1;

        auto mix = [&](uint64_t v) noexcept {
            lo ^= v;
            lo = std::rotl(lo, 31);
            lo *= kP1;

            hi ^= v ^ lo;
            hi = std::rotl(hi, 27);
            hi *= kP2;
        };

        for (auto const& n : nodes) {
            mix(n.output_hash.low);
            mix(n.output_hash.high);
            mix(n.input_hash.low);
            mix(n.input_hash.high);
        }

        for (auto const& w : wires) {
            uint64_t edge =
                (uint64_t(w.from_id) << 48) |
                (uint64_t(w.from_outlet) << 32) |
                (uint64_t(w.to_id) << 16) |
                (uint64_t(w.to_inlet));

            mix(edge);
            mix(uint64_t(w.policy));
        }

        mix(schema::kPhyriadHalVersion);
        mix(uint64_t(hal::kDestructivePad));

        lo ^= lo >> 33;
        lo *= 0xFF51AFD7ED558CCDULL;
        lo ^= lo >> 33;

        hi ^= hi >> 33;
        hi *= 0xFF51AFD7ED558CCDULL;
        hi ^= hi >> 33;

        return schema::Hash128{lo, hi};
    }

    std::expected<BuiltGraph, ConfigError> do_build_() noexcept {
        for (size_t i = 0; i < node_entries_.size(); ++i) {
            for (size_t j = i + 1; j < node_entries_.size(); ++j) {
                if (node_entries_[i].name == node_entries_[j].name) {
                    return std::unexpected(ConfigError::make(
                        ConfigErrorCode::DuplicateNodeName,
                        "Duplicate node name: " + node_entries_[i].name));
                }
            }
        }

        auto find_node = [&](std::string const& name) -> uint32_t {
            for (auto const& n : node_entries_)
                if (n.name == name) return n.node_id;
            return UINT32_MAX;
        };

        for (auto& w : wire_entries_) {
            w.from_id = find_node(w.from_name);
            w.to_id = find_node(w.to_name);

            if (w.from_id == UINT32_MAX)
                return std::unexpected(ConfigError::make(
                    ConfigErrorCode::UnknownNodeName,
                    "Unknown source node"));

            if (w.to_id == UINT32_MAX)
                return std::unexpected(ConfigError::make(
                    ConfigErrorCode::UnknownNodeName,
                    "Unknown destination node"));
        }

        for (size_t wi = 0; wi < wire_entries_.size(); ++wi) {
            auto const& w = wire_entries_[wi];
            auto const& src = node_entries_[w.from_id];
            auto const& dst = node_entries_[w.to_id];

            bool src_typed = (src.output_hash.low || src.output_hash.high);
            bool dst_typed = (dst.input_hash.low || dst.input_hash.high);

            if (src_typed && dst_typed && src.output_hash != dst.input_hash) {
                return std::unexpected(ConfigError::make(
                    ConfigErrorCode::TypeMismatch,
                    "Wire type mismatch"));
            }
        }

        if (node_entries_.empty() || wire_entries_.empty()) {
            return std::unexpected(ConfigError::make(
                ConfigErrorCode::EmptyGraph,
                "Empty graph"));
        }

        const uint32_t n_nodes = (uint32_t)node_entries_.size();
        const uint32_t n_wires = (uint32_t)wire_entries_.size();

        std::vector<schema::NodeDescriptor> nd_vec(n_nodes);
        std::vector<schema::WireDescriptor> wd_vec(n_wires);
        std::vector<scheduler::PlacementHint> hint_vec(n_nodes);

        for (auto const& e : node_entries_) {
            auto& nd = nd_vec[e.node_id];

            nd.node_id = e.node_id;
            nd.type_hash_lo = (uint32_t)(e.output_hash.low & 0xFFFFFFFFu);
            nd.type_hash_hi = (uint32_t)(e.output_hash.high & 0xFFFFFFFFu);

            const size_t nlen = std::min(e.name.size(), sizeof(nd.name) - 1);
            std::memcpy(nd.name, e.name.data(), nlen);
            nd.name[nlen] = '\0';

            hint_vec[e.node_id] = e.hint;
        }

        for (size_t i = 0; i < wire_entries_.size(); ++i) {
            auto const& w = wire_entries_[i];
            auto const& src = node_entries_[w.from_id];
            auto& wd = wd_vec[i];

            if (w.from_outlet < src.outlet_hashes.size())
                wd.message_hash = src.outlet_hashes[w.from_outlet];
            else
                wd.message_hash = src.output_hash;

            wd.capacity = (wd.capacity == 0) ? 64u : wd.capacity;
            wd.slot_size_bytes = 0;

            wd.src_node_id = w.from_id;
            wd.src_outlet = w.from_outlet;

            wd.dst_node_id = w.to_id;
            wd.dst_inlet = w.to_inlet;

            wd.wire_id = (uint32_t)i;
        }

        schema::Hash128 graph_hash =
            compute_graph_hash_(node_entries_, wire_entries_);

        BuiltGraph result{};
        result.header = schema::make_schema_descriptor(
            graph_hash, n_nodes, n_wires);

        result.nodes = std::move(nd_vec);
        result.wires = std::move(wd_vec);
        result.hints = std::move(hint_vec);

        if (validate_requested_) {
            auto vr = graph::GraphValidator::validate_all(
                std::span<const schema::WireDescriptor>{
                    result.wires.data(),
                    result.wires.size()
                },
                n_nodes);

            if (!vr.ok()) {
                return std::unexpected(ConfigError::make(
                    ConfigErrorCode::ValidationFailed,
                    "Graph validation failed"));
            }
        }

        return result;
    }
};

inline DslGraphBuilder& WireBuilder::to(
    std::string_view dest,
    uint32_t inlet,
    WirePolicy policy) noexcept
{
    builder_->add_wire_(
        from_,
        from_outlet_,
        dest,
        inlet,
        policy
    );

    return *builder_;
}

[[nodiscard]]
inline DslGraphBuilder build_graph() noexcept {
    return {};
}

struct GraphDSL {
    using Builder = DslGraphBuilder;
};

} // namespace phyriad::api
// Made with my soul - Swately <3
