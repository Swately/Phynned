// framework/graph/include/phyriad/api/WireRegistry.hpp
// Registry of ring-buffer factories keyed by schema::Hash128.
//
// WireRegistry maps a 128-bit message type fingerprint to a factory that
// creates a WireHandle (Ring<T, Cap>) for that type.
//
// Usage:
//   WireRegistry wr;
//   wr.register_type<MarketTick>();
//   auto result = wr.create_wire(schema_hash<MarketTick>(), 0, 0, 1, 1024);
//
// Note: NodeRegistry::register_node<N>() automatically calls
// register_type<N::output_type>() and register_type<N::input_type>()
// on its embedded WireRegistry. Direct use is only needed when registering
// wire types for nodes not going through NodeRegistry.
//
#pragma once
#include "WireHandle.hpp"
#include <phyriad/schema/SchemaHash.hpp>
#include <phyriad/schema/Error.hpp>
#include <expected>
#include <functional>
#include <string_view>
#include <unordered_map>

namespace phyriad::api {

class WireRegistry {
public:
    WireRegistry()  = default;
    ~WireRegistry() = default;

    WireRegistry(WireRegistry const&)            = delete;
    WireRegistry& operator=(WireRegistry const&) = delete;
    WireRegistry(WireRegistry&&)                 = default;
    WireRegistry& operator=(WireRegistry&&)      = default;

    // ── register_type<T> ──────────────────────────────────────────────────────
    // Register a ring factory for message type T.
    // Duplicate registrations of the same type are silently ignored.
    template <schema::PodMessage T>
    void register_type() noexcept {
        const uint64_t key = schema::schema_hash<T>().low;
        if (factories_.count(key)) return;

        factories_.emplace(key, Factory{
            [](uint32_t wire_id, uint32_t src, uint32_t dst, uint32_t cap) noexcept {
                return WireHandle::make<T>(wire_id, src, dst, cap);
            }
        });
    }

    // ── create_wire ───────────────────────────────────────────────────────────
    // Create a WireHandle for the type identified by type_hash.
    // Returns Error::InvalidHandle if no factory is registered for type_hash.
    [[nodiscard]] std::expected<WireHandle, phyriad::Error>
    create_wire(schema::Hash128 type_hash, uint32_t wire_id,
                uint32_t src, uint32_t dst, uint32_t capacity) const noexcept {
        auto it = factories_.find(type_hash.low);
        if (it == factories_.end()) {
            return std::unexpected(phyriad::Error{
                .code           = ErrorCode::SchemaMismatch,
                .source_node_id = wire_id,
                .timestamp_ns   = 0});
        }
        WireHandle h = it->second.make(wire_id, src, dst, capacity);
        if (!h.valid()) {
            return std::unexpected(phyriad::Error{
                .code           = ErrorCode::InvalidHandle,
                .source_node_id = wire_id,
                .timestamp_ns   = 0});
        }
        return h;
    }

    [[nodiscard]] bool has_type(schema::Hash128 type_hash) const noexcept {
        return factories_.count(type_hash.low) > 0;
    }

    [[nodiscard]] std::size_t type_count() const noexcept { return factories_.size(); }

private:
    struct Factory {
        std::function<WireHandle(uint32_t, uint32_t, uint32_t, uint32_t)> make;
    };

    std::unordered_map<uint64_t, Factory> factories_;
};

} // namespace phyriad::api
// Made with my soul - Swately <3
