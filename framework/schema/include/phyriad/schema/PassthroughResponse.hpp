// framework/schema/include/phyriad/schema/PassthroughResponse.hpp
// Blind-proxy protocol — PassthroughResponse POD.
//
// The supervisor's reply to a CapabilityRequest.  The actual payload bytes
// do NOT travel in this POD (that would bloat the slot).  Instead they are
// written to a temporary Category at a canonical name such as
// "capability_payload_<request_id>" and the maid reads them there.
// PassthroughResponse signals completion + verification via payload_hash.
//
// CRITICAL invariant: InferenceDetected (error_code == 6) must be raised
// by the audit harness (audit_delegation.py) if any LLM call is detected
// between the CapabilityRequest event and the PassthroughResponse event in
// the maid_log.  The supervisor MUST NOT infer; it only fetches + forwards.
//
// References:
//   CONTEXT_PILLAR_P9_DESIGN.md §3 cluster D (arc 70)
//   IM_REAL.md §6.3 — wire contract proposal
//
// Size contract:
//   sizeof(PassthroughResponse) == 64 bytes (<= 128 assert below)
#pragma once
#include <phyriad/schema/PodMessage.hpp>
#include <cstdint>

namespace phyriad::schema {

enum class PassthroughError : std::uint16_t {
    OK                 = 0,
    NotFound           = 1,   // capability target doesn't exist
    AccessDenied       = 2,   // operator denied
    Timeout            = 3,
    InvalidCapability  = 4,   // requestor asked for unknown capability bit
    PayloadTooLarge    = 5,   // bytes exceed inline transfer size
    InferenceDetected  = 6,   // audit caught LLM call between fetch and forward — CRITICAL
};

// PassthroughResponse — supervisor's reply to a CapabilityRequest.
// payload_hash MUST be a SHA256 over the fetched bytes; the maid verifies
// it matches payload_hash_expected if that field was non-zero in the
// originating CapabilityRequest.
//
// The payload bytes themselves travel via a temporary Category named
// "capability_payload_<request_id>".  This POD is only the completion
// signal + verification envelope.
struct PassthroughResponse {
    std::uint64_t request_id;          // matches CapabilityRequest.request_id
    std::uint8_t  payload_hash[32];    // SHA256 of fetched bytes
    std::uint64_t payload_size_bytes;
    std::uint16_t error_code;          // PassthroughError value
    std::uint16_t _pad;
    std::uint32_t responder_holon_id;  // supervisor's holon id
    std::uint64_t timestamp_ns;
};
PHYRIAD_ASSERT_POD(PassthroughResponse);
static_assert(sizeof(PassthroughResponse) <= 128,
    "PassthroughResponse must fit a single slot");
static_assert(sizeof(PassthroughResponse) == 64,
    "PassthroughResponse layout changed — update wire docs if intentional");

} // namespace phyriad::schema
// Made with my soul - Swately <3
