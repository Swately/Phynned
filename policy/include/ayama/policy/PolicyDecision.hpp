// apps/ayama/policy/include/ayama/policy/PolicyDecision.hpp
// PolicyDecision — POD output of the PolicyEngine for a single target.
//
// IPC-safe: trivially_copyable, standard_layout, 32 bytes.
//
// Threading: written by PolicyEngine (agent thread); read via SHM seqlock.
// Resource:  32B × 16 slots = 512 B.
//
#pragma once
#include <cstdint>

namespace ayama::policy {

/// What kind of action the policy engine has decided to take.
enum class ActionKind : uint8_t {
    None         = 0,  ///< No action.
    PinAffinity  = 1,  ///< Set process CPU affinity mask.
    SetPriority  = 2,  ///< Change priority class.
    Revert       = 3,  ///< Undo a previous Ayama action.
};

/// A single policy decision — 32 bytes.
/// Field order: 8B fields first, then 4B, then 1B + padding.
/// This eliminates implicit padding before uint64_t members (32B total, no gaps).
struct alignas(8) PolicyDecision {
    uint64_t   core_mask;        //  8B  @0  — for PinAffinity
    uint64_t   decided_tsc;      //  8B  @8  — TSC when this decision was made
    uint32_t   target_pid;       //  4B  @16
    uint32_t   rule_id;          //  4B  @20 — which rule produced this decision
    uint32_t   priority_class;   //  4B  @24 — for SetPriority (NORMAL_PRIORITY_CLASS etc.)
    ActionKind action_kind;      //  1B  @28
    uint8_t    confidence;       //  1B  @29 — 0..100
    uint8_t    _pad[2];          //  2B  @30
}; // total: 32B
static_assert(sizeof(PolicyDecision)        == 32, "PolicyDecision must be 32B");
static_assert(alignof(PolicyDecision)       == 8,  "PolicyDecision must be 8B-aligned");
static_assert(__is_trivially_copyable(PolicyDecision), "PolicyDecision must be trivially copyable");

/// Maximum decisions emitted per evaluation cycle.
inline constexpr uint32_t kMaxDecisionsPerCycle = 16u;

} // namespace ayama::policy
// Made with my soul - Swately <3
