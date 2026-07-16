// framework/schema/include/phyriad/schema/OpaqueMessage.hpp
// Tier 2 message concept: opaque byte span for bridging external systems.
//
// Tier 2 explicitly gives up zero-copy semantics — a copy is made at the
// bridge boundary. Use only for interop with legacy code, external libraries,
// or systems with incompatible memory models.
//
// A Tier 2 message type must expose:
//   - data()  → const void*    (start of serialized bytes)
//   - size()  → size_t         (byte count)
//   - type_id → uint32_t       (opaque application-defined tag; not schema-validated)
//
// ANTI-PATTERNS:
//   ❌ Putting a Tier 2 type into a Tier 0 ring (schema_hash would be meaningless)
//   ❌ Using Tier 2 for internal message passing (defeats type safety entirely)
#pragma once
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace phyriad::schema {

template <typename T>
concept OpaqueMessage = requires(const T& t) {
    { t.data()    } -> std::convertible_to<const void*>;
    { t.size()    } -> std::convertible_to<size_t>;
    { T::type_id  } -> std::convertible_to<uint32_t>;
};

// ── Built-in Tier 2 carrier ───────────────────────────────────────
// Lightweight non-owning view over a pre-serialized byte buffer.
// The caller owns the buffer lifetime — this type never allocates.
struct OpaqueSpan {
    const void* ptr{nullptr};
    size_t      len{0};
    static constexpr uint32_t type_id = 0xFFFFFFFFu; // sentinel for opaque

    [[nodiscard]] const void* data() const noexcept { return ptr; }
    [[nodiscard]] size_t      size() const noexcept { return len; }
};
static_assert(OpaqueMessage<OpaqueSpan>);

} // namespace phyriad::schema
// Made with my soul - Swately <3
