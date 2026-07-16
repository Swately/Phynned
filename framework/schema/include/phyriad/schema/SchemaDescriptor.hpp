// framework/schema/include/phyriad/schema/SchemaDescriptor.hpp
// Persistent SHM schema descriptor — written once at graph initialization,
// validated by every new process that attaches to the shared memory arena.
//
// Layout rules:
//   - POD only (TriviallyCopyable): safe to map from raw SHM with mmap/MapViewOfFile
//   - alignas(64): occupies exactly one cache line, avoids false sharing on first read
//   - Size == 64 bytes: enforced by static_assert; 36 bytes reserved for V2 extension
//   - schema_hash covers the entire graph wire topology, not just individual types
//
// Attach protocol (Block C wires it in):
//   1. Producer writes GraphSchemaDescriptor at offset 0 of the SHM region
//   2. Consumer reads and validates schema_hash before accessing any ring buffer
//   3. Mismatch → return std::unexpected(Error{ErrorCode::SchemaMismatch, ...})
//   4. No migration in V1 — hard fail, restart with matching binary
#pragma once
#include "SchemaHash.hpp"   // Hash128, kPhyriadHalVersion, hal::kDestructivePad (via Cacheline.hpp)
#include <cstdint>
#include <type_traits>

namespace phyriad::schema {

// ── Wire descriptor ───────────────────────────────────────────────
// One per wire in the graph. Embedded as a variable-length array after
// GraphSchemaDescriptor in SHM. Not accessed in hot path.
struct WireDescriptor {
    Hash128  message_hash;
    uint32_t capacity;
    uint32_t slot_size_bytes;
    uint32_t src_node_id;
    uint32_t src_outlet;
    uint32_t dst_node_id;
    uint32_t dst_inlet;
    uint32_t wire_id;
};
static_assert(sizeof(WireDescriptor) == 48);
static_assert(std::is_trivially_copyable_v<WireDescriptor>);

// ── Node descriptor ───────────────────────────────────────────────
struct NodeDescriptor {
    uint32_t node_id;
    uint32_t type_hash_lo;   // lower 32 bits of schema_hash (for fast reject)
    uint32_t type_hash_hi;
    uint32_t _pad{0};
    char     name[48];       // null-terminated, truncated if longer
};
static_assert(sizeof(NodeDescriptor) == 64);
static_assert(std::is_trivially_copyable_v<NodeDescriptor>);

// ── Graph schema descriptor — SHM header ─────────────────────────
// Must be exactly 64 bytes (one cache line). Placed at offset 0 of the
// SHM region; followed by NodeDescriptor[num_nodes] + WireDescriptor[num_wires].
struct alignas(64) GraphSchemaDescriptor {
    Hash128  graph_hash;            // 16B: hash of entire graph topology
    uint64_t hal_version;           // 8B:  kPhyriadHalVersion at compile time
    uint32_t cache_line_size;       // 4B:  hal::kDestructivePad (must match)
    uint32_t num_nodes;             // 4B
    uint32_t num_wires;             // 4B
    uint32_t phyriad_magic;           // 4B:  0x50485952u ("PHYR") sanity sentinel
    uint8_t  reserved_v2[24];       // 24B: zeroed, reserved for V2 extensions
};
static_assert(sizeof(GraphSchemaDescriptor) == 64,
    "GraphSchemaDescriptor must be exactly 64 bytes (one cache line)");
static_assert(std::is_trivially_copyable_v<GraphSchemaDescriptor>);
static_assert(alignof(GraphSchemaDescriptor) == 64);

inline constexpr uint32_t kPhyriadMagic = 0x50485952u; // "PHYR"

// ── Factory helper ────────────────────────────────────────────────
// Creates a zeroed descriptor with the invariant fields pre-filled.
// Called once during graph construction (Block E).
[[nodiscard]] inline GraphSchemaDescriptor make_schema_descriptor(
    Hash128  graph_hash,
    uint32_t num_nodes,
    uint32_t num_wires) noexcept
{
    GraphSchemaDescriptor d{};
    d.graph_hash       = graph_hash;
    d.hal_version      = kPhyriadHalVersion;
    d.cache_line_size  = static_cast<uint32_t>(hal::kDestructivePad);
    d.num_nodes        = num_nodes;
    d.num_wires        = num_wires;
    d.phyriad_magic      = kPhyriadMagic;
    return d;
}

// ── Validation ────────────────────────────────────────────────────
// Returns true if the descriptor is valid for THIS binary.
// A false return must produce Error{ErrorCode::SchemaMismatch}.
[[nodiscard]] inline bool validate_schema_descriptor(
    const GraphSchemaDescriptor& d) noexcept
{
    if (d.phyriad_magic      != kPhyriadMagic)            return false;
    if (d.hal_version      != kPhyriadHalVersion)       return false;
    if (d.cache_line_size  != hal::kDestructivePad)   return false;
    return true;
}

} // namespace phyriad::schema
// Made with my soul - Swately <3
