// framework/schema/include/phyriad/schema/CapabilityRequest.hpp
// Blind-proxy protocol — CapabilityRequest POD.
//
// An inferior maid that needs a capability it lacks (e.g. file read, net
// fetch) emits this POD to a tool-capable superior holon (typically the
// supervisor LLM).  The superior fetches the requested bytes VERBATIM —
// no LLM inference — and returns a PassthroughResponse with a matching
// payload_hash.
//
// See IM_REAL.md §6.3 for the cognitive-ownership rationale:
// "The maid owns the analysis; the supervisor is a dumb tool bus."
//
// References:
//   CONTEXT_PILLAR_P9_DESIGN.md §3 cluster D (arc 70)
//   IM_REAL.md §6.3 — wire contract proposal
//
// Size contract:
//   sizeof(CapabilityRequest) == 192 bytes (<= 256 assert below)
//   Fits a BlackboardSlot with kSlotPayloadMax == 256 bytes margin.
#pragma once
#include <phyriad/schema/PodMessage.hpp>
#include <cstdint>

namespace phyriad::schema {

// Capability bits (bitmask).  A maid declares which capabilities it can
// fulfill via attach-time schema hash (dogma 12); it emits a request for
// one it lacks.  Only a single bit should be set per request.
enum class CapabilityBit : std::uint32_t {
    FileRead       = 1u << 0,
    FileWrite      = 1u << 1,   // post-1.0 territory; declared for spec completeness
    NetFetch       = 1u << 2,
    SqlQuery       = 1u << 3,
    ProcessSpawn   = 1u << 4,
    OperatorInput  = 1u << 5,   // ask operator something interactively
    // Bits 6-31 reserved for future capabilities.
};

// CapabilityRequest — emitted by an inferior maid that needs a capability
// it lacks.  Routed to a tool-capable superior holon (typically the
// supervisor LLM) which fetches the requested bytes VERBATIM (no LLM
// inference) and returns PassthroughResponse with matching payload_hash.
// See IM_REAL.md §6.3 for the cognitive-ownership rationale.
struct CapabilityRequest {
    std::uint64_t request_id;           // monotonic; maid sets
    std::uint32_t requestor_holon_id;   // who's asking
    std::uint32_t capability_bit;       // CapabilityBit value (single, not mask)
    std::uint8_t  payload_descriptor[128]; // free-form per capability (e.g. file path, URL)
    std::uint8_t  payload_hash_expected[32]; // SHA256 the maid will verify against
                                             // (optional: zero bytes = no expectation)
    std::uint64_t timestamp_ns;
    std::uint32_t timeout_ms;           // 0 = wait forever (not recommended)
    std::uint32_t _pad;
};
PHYRIAD_ASSERT_POD(CapabilityRequest);
static_assert(sizeof(CapabilityRequest) <= 256,
    "CapabilityRequest must fit BlackboardSlot kSlotPayloadMax with margin");
static_assert(sizeof(CapabilityRequest) == 192,
    "CapabilityRequest layout changed — update wire docs if intentional");

} // namespace phyriad::schema
// Made with my soul - Swately <3
