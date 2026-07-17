// action/include/phynned/action/ActionLog.hpp
// ActionLog — audit-trail entry type and ring type alias.
//
// ActionLogEntry is a 56-byte POD written by ActionExecutor on every apply/
// revert and published to the UI via the SHM ring.
//
// ActionLogRing is a type alias for phyriad::ipc::Ring<ActionLogEntry, 128>.
// Using phyriad::ipc::Ring gives cache-line-separated producer/consumer cursors,
// an explicit drop counter, and the write_cursor()/peek_at() API that lets
// AuditLog and PhynnedAgentPublisher maintain independent read cursors without
// interfering with each other.
//
#pragma once

#include <phyriad/ipc/Ring.hpp>   // FR-12 — phyriad::ipc::Ring<T, Capacity>

#include <cstdint>

namespace phynned::action {

/// One entry in the action audit trail — 56 bytes.
/// Field order: all 8B fields first, then 4B, then 1B + padding.
/// This eliminates implicit padding before uint64_t members (56B total, no gaps).
struct alignas(8) ActionLogEntry {
    uint64_t tsc_applied;        //  8B  @0  — 0 until actually applied
    uint64_t tsc_reverted;       //  8B  @8  — 0 if still active
    uint64_t prev_affinity_mask; //  8B  @16 — captured before Phynned changed it
    uint64_t new_affinity_mask;  //  8B  @24
    uint32_t target_pid;         //  4B  @32
    uint32_t rule_id;            //  4B  @36
    uint32_t prev_priority_class;//  4B  @40
    uint32_t new_priority_class; //  4B  @44
    uint8_t  success;            //  1B  @48 — 1 = applied OK, 0 = failed
    uint8_t  _pad[7];            //  7B  @49
}; // total: 56B
static_assert(sizeof(ActionLogEntry)         == 56,  "ActionLogEntry must be 56B");
static_assert(alignof(ActionLogEntry)        == 8,   "ActionLogEntry must be 8B-aligned");
static_assert(__is_trivially_copyable(ActionLogEntry), "ActionLogEntry must be trivially copyable");

/// Capacity of the circular action log (power of 2, ≥ ring burst size).
inline constexpr uint32_t kActionLogCap = 128u;

/// Circular action log ring — producer overwrites, consumers track externally.
///
/// Producer: ActionExecutor (agent main thread) via push_unchecked().
///   push_unchecked() always writes (circular/overwrite semantics matching
///   the original ActionLogRing::push). The SPSC read_seq_ stays at 0 because
///   no consumer calls drain(). In practice the ring never wraps (≤32 active
///   actions at any time, kActionLogCap = 128).
///
/// Consumers: AuditLog + PhynnedAgentPublisher — each keeps an independent
///   uint64_t cursor and uses write_cursor() / peek_at() to iterate new
///   entries without touching read_seq_. Both consumers apply the wrap-around
///   skip-ahead guard:
///     if (head - cursor > kActionLogCap) cursor = head - kActionLogCap;
///
/// NOTE: phyriad::ipc::Ring<T, N> is non-copyable, non-movable (contains atomics
/// and large embedded storage).  Use by pointer or direct member.
using ActionLogRing = phyriad::ipc::Ring<ActionLogEntry, kActionLogCap>;

} // namespace phynned::action
// Made with my soul - Swately <3
