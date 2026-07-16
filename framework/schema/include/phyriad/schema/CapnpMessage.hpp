// framework/schema/include/phyriad/schema/CapnpMessage.hpp
// Tier 1 message concept: Cap'n Proto backed, versioned, for control plane traffic.
//
// Cap'n Proto types are NOT TriviallyCopyable — they use arena allocation and
// reference semantics. They are suitable for:
//   - Configuration distribution
//   - Structured telemetry
//   - Schema negotiation at startup
//   - Any message that needs schema evolution (add/remove fields safely)
//
// Cap'n Proto integration:
//   - find_package(CapnProto) or FetchContent in CMake
//   - Compile .capnp schemas with capnp compile -oc++ schema.capnp
//   - Each generated ::Reader / ::Builder pair satisfies CapnpMessage<T>
//
// ANTI-PATTERNS:
//   ❌ Using Cap'n Proto types in hot-path Tier 0 channels
//   ❌ Putting Cap'n Proto builders in ring buffer slots
//   ❌ Serializing to std::string before sending (defeats the purpose)
#pragma once
#include <type_traits>

namespace phyriad::schema {

// Marker tag: inherit from this (or specialize the trait) to opt into Tier 1.
// Cap'n Proto generated types do NOT inherit from CapnpMessageBase automatically
// — you must either specialize IsCapnpMessage<T> or wrap in a tagged struct.
struct CapnpMessageBase {};

// Specialization hook: set to true for Cap'n Proto generated types.
// Usage: template<> struct IsCapnpMessage<MyCap::Reader> : std::true_type {};
template <typename T>
struct IsCapnpMessage : std::bool_constant<std::is_base_of_v<CapnpMessageBase, T>> {};

template <typename T>
concept CapnpMessage =
    IsCapnpMessage<T>::value    &&
    !std::is_trivially_copyable_v<T>;  // Cap'n Proto types must NOT be POD

// ── Helpers ───────────────────────────────────────────────────────

// Register a Cap'n Proto generated type as Tier 1 without modifying it.
// Place in the same header that includes the generated .capnp.h:
//   PHYRIAD_REGISTER_CAPNP(::MySchema::MyMessage::Reader);
#define PHYRIAD_REGISTER_CAPNP(T)                           \
    template<> struct phyriad::schema::IsCapnpMessage<T>    \
        : std::true_type {}

} // namespace phyriad::schema
// Made with my soul - Swately <3
