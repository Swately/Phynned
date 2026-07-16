// framework/graph/include/phyriad/api/NodeRegistry.hpp
// Registry that maps node names to NodeHandle factories.
//
// NodeRegistry:
//   - Stores one factory per named node type (name → NodeHandle::make<N>)
//   - Automatically registers N::output_type and N::input_type in the embedded
//     WireRegistry when register_node<N>() is called
//
// GraphRuntime calls instantiate(name, id) for each NodeDescriptor in BuiltGraph
// and wire_registry().create_wire(...) for each WireDescriptor.
//
// Usage:
//   NodeRegistry reg;
//   reg.register_node<MarketSource>("source");
//   reg.register_node<PriceTransform>("transform");
//   reg.register_node<CountSink>("sink");
//
//   auto result = reg.instantiate("source", node_id);
//
#pragma once
#include "NodeHandle.hpp"
#include "WireRegistry.hpp"
#include <phyriad/node/Runnable.hpp>
#include <phyriad/node/Categories.hpp>
#include <phyriad/schema/SchemaHash.hpp>
#include <phyriad/schema/Error.hpp>
#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace phyriad::api {

class NodeRegistry {
public:
    NodeRegistry()  = default;
    ~NodeRegistry() = default;

    NodeRegistry(NodeRegistry const&)            = delete;
    NodeRegistry& operator=(NodeRegistry const&) = delete;
    NodeRegistry(NodeRegistry&&)                 = default;
    NodeRegistry& operator=(NodeRegistry&&)      = default;

    // ── register_node<N>(name) ────────────────────────────────────────────────
    // Register type N under the given name. Also auto-registers N::output_type
    // and N::input_type in the embedded WireRegistry.
    // Duplicate name → existing entry is replaced.
    template <Runnable N>
    void register_node(std::string name) noexcept {
        schema::Hash128 out_hash{};
        schema::Hash128 in_hash{};

        if constexpr (node::Source<N>) {
            out_hash = schema::schema_hash<typename N::output_type>();
            wire_reg_.register_type<typename N::output_type>();
        }
        // Also register actor-style input nodes: have input_type + inlet() but
        // no on_message() (they call inlet().receive() from tick() directly).
        // This mirrors the same check in NodeHandle::make / NodeHandle::wrap.
        if constexpr (node::Sink<N> ||
                      (requires { typename N::input_type; } &&
                       requires(N& n) { n.inlet(); })) {
            in_hash = schema::schema_hash<typename N::input_type>();
            wire_reg_.register_type<typename N::input_type>();
        }

        entries_.insert_or_assign(std::move(name), Entry{
            .node_factory = [](NodeId id) noexcept {
                return NodeHandle::make<N>(id);
            },
            .output_hash = out_hash,
            .input_hash  = in_hash,
        });
    }

    // ── register_factory ─────────────────────────────────────────────────────
    // Register a custom factory callable for a named node.
    // Use for nodes that are not default-constructible (e.g., UIThreadNode,
    // RenderNode<S>) where the caller retains ownership via NodeHandle::wrap.
    //
    // out_hash / in_hash: schema hashes for the node's output/input types.
    // Pass {} if the node has no output or no input.
    template <typename Factory>
        requires requires(Factory f, NodeId id) { { f(id) } -> std::same_as<NodeHandle>; }
    void register_factory(std::string        name,
                          Factory            factory,
                          schema::Hash128    out_hash = {},
                          schema::Hash128    in_hash  = {}) noexcept {
        entries_.insert_or_assign(std::move(name), Entry{
            .node_factory = std::move(factory),
            .output_hash  = out_hash,
            .input_hash   = in_hash,
        });
    }

    // ── instantiate ───────────────────────────────────────────────────────────
    // Create a NodeHandle for the named type. Returns InvalidHandle if the name
    // was not registered, or if the underlying allocation fails.
    [[nodiscard]] std::expected<NodeHandle, phyriad::Error>
    instantiate(std::string_view name, NodeId id) const noexcept {
        auto it = entries_.find(std::string{name});
        if (it == entries_.end()) {
            return std::unexpected(phyriad::Error{
                .code           = ErrorCode::InvalidHandle,
                .source_node_id = id,
                .timestamp_ns   = 0});
        }
        NodeHandle h = it->second.node_factory(id);
        if (!h.valid()) {
            return std::unexpected(phyriad::Error{
                .code           = ErrorCode::InvalidHandle,
                .source_node_id = id,
                .timestamp_ns   = 0});
        }
        return h;
    }

    // ── Type hash queries ─────────────────────────────────────────────────────
    [[nodiscard]] schema::Hash128 output_type_of(std::string_view name) const noexcept {
        auto it = entries_.find(std::string{name});
        return (it != entries_.end()) ? it->second.output_hash : schema::Hash128{};
    }

    [[nodiscard]] schema::Hash128 input_type_of(std::string_view name) const noexcept {
        auto it = entries_.find(std::string{name});
        return (it != entries_.end()) ? it->second.input_hash : schema::Hash128{};
    }

    [[nodiscard]] bool has_node(std::string_view name) const noexcept {
        return entries_.count(std::string{name}) > 0;
    }

    [[nodiscard]] std::size_t node_count() const noexcept { return entries_.size(); }

    // ── Wire registry access ──────────────────────────────────────────────────
    [[nodiscard]] WireRegistry const& wire_registry() const noexcept { return wire_reg_; }
    [[nodiscard]] WireRegistry&       wire_registry()       noexcept { return wire_reg_; }

private:
    struct Entry {
        std::function<NodeHandle(NodeId)> node_factory;
        schema::Hash128                   output_hash;
        schema::Hash128                   input_hash;
    };

    std::unordered_map<std::string, Entry> entries_;
    WireRegistry                           wire_reg_;
};

} // namespace phyriad::api
// Made with my soul - Swately <3
