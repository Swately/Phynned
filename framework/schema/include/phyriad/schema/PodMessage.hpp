// framework/schema/include/phyriad/schema/PodMessage.hpp
// Tier 0 message concept: TriviallyCopyable POD suitable for zero-copy SHM transport.
//
// A type satisfying PodMessage<T> can be:
//   - Copied via memcpy into a ring slot (zero allocation)
//   - Fingerprinted at compile-time via schema_hash<T>()
//   - Exchanged cross-process without serialization overhead
//
// std::variant<T...> is allowed IF all alternatives satisfy PodMessage individually
// AND the total size fits within kMaxPodSize.
// std::any is PROHIBITED — it introduces heap allocation.
#pragma once
#include "SchemaHash.hpp"
#include <cstddef>
#include <type_traits>

namespace phyriad::schema {

// Maximum size of a Tier 0 message.
// Fits in a single hardware page on most architectures; allows arrays of prices/volumes.
inline constexpr size_t kMaxPodSize = 4096u;

template <typename T>
concept PodMessage =
    std::is_trivially_copyable_v<T>     &&
    std::is_standard_layout_v<T>        &&
    !std::is_polymorphic_v<T>           &&
    !std::has_virtual_destructor_v<T>   &&
    sizeof(T) >= 1                      &&
    sizeof(T) <= kMaxPodSize            &&
    alignof(T) <= 64u;                   // max alignment = one cache line

// ── Compile-time validation helpers ──────────────────────────────
// Apply these to your message types in headers to get early error messages.
//
// Example:
//   struct MarketTick { uint64_t price; uint64_t volume; uint32_t side; };
//   PHYRIAD_ASSERT_POD(MarketTick);
//
#define PHYRIAD_ASSERT_POD(T)                                            \
    static_assert(std::is_trivially_copyable_v<T>,                     \
        #T " must be TriviallyCopyable (no non-trivial constructors)"); \
    static_assert(std::is_standard_layout_v<T>,                        \
        #T " must be StandardLayout (no virtual base, single base)");   \
    static_assert(sizeof(T) <= phyriad::schema::kMaxPodSize,             \
        #T " exceeds kMaxPodSize (4096 bytes)");                        \
    static_assert(alignof(T) <= 64u,                                   \
        #T " alignment exceeds 64 bytes (one cache line maximum)")

// schema_hash<T>() is inherited from SchemaHash.hpp.
// Usage: static constexpr auto h = schema_hash<MyTick>();

// ── Canonical sample type (also serves as concept smoke-test) ─────
struct SampleTick {
    uint64_t price{0};
    uint64_t volume{0};
    uint32_t side{0};
    uint32_t sequence{0};
};
static_assert(PodMessage<SampleTick>);
PHYRIAD_ASSERT_POD(SampleTick);
static_assert(schema_hash<SampleTick>() != Hash128{},
    "schema_hash<SampleTick>() must be non-zero");

} // namespace phyriad::schema
// Made with my soul - Swately <3
