// framework/schema/include/phyriad/schema/MessageTier.hpp
// Three-tier message type system for Phyriad transport layer.
//
// Tier 0 — POD (TriviallyCopyable)         : hot path, zero-copy, compile-time schema
// Tier 1 — Versioned (Cap'n Proto)         : control plane, structured evolution
// Tier 2 — Opaque (external/bridge)        : explicit copies, no schema guarantee
//
// ANTI-PATTERNS:
//   ❌ std::any in Tier 0 — heap alloc, breaks zero-copy
//   ❌ Cap'n Proto types as Tier 0 — not TriviallyCopyable
//   ❌ runtime dispatch over tiers — use if constexpr / concepts
#pragma once
#include "PodMessage.hpp"
#include "CapnpMessage.hpp"
#include "OpaqueMessage.hpp"

namespace phyriad::schema {

// Tier 0: zero-copy hot path
template <typename T>
concept Tier0Message = PodMessage<T>;

// Tier 1: structured control plane (Cap'n Proto backed)
template <typename T>
concept Tier1Message = CapnpMessage<T>;

// Tier 2: opaque bridge — explicit memory semantics
template <typename T>
concept Tier2Message = OpaqueMessage<T>;

// Any valid message in any tier
template <typename T>
concept AnyMessage = Tier0Message<T> || Tier1Message<T> || Tier2Message<T>;

// Tier classification helpers (compile-time)
template <typename T>
inline constexpr int tier_of = [] {
    if constexpr (Tier0Message<T>) return 0;
    if constexpr (Tier1Message<T>) return 1;
    if constexpr (Tier2Message<T>) return 2;
    return -1;
}();

} // namespace phyriad::schema
// Made with my soul - Swately <3
