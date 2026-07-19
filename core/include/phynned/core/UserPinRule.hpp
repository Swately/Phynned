// core/include/phynned/core/UserPinRule.hpp
// evaluate_user_pin — the PURE decision for a W3 user "Always" pin.
//
// Mirrors evaluate_corral's structural safety: this returns a plain value with
// NO reference to the executor / AcProbe / any syscall. A user pin can ONLY be
// applied where AgentRuntime feeds this decision into the AC gate + executor
// (see §3c-user in AgentRuntime.cpp). Decoupling the decision from the action is
// what makes the safety invariants (R1 AC-gate routing, R4 flap guard) provable
// and unit-testable without a live agent.
//
// PRECEDENCE the caller enforces around this (frozen, master plan):
//   1. Safety      — AC gate (this flags `needs_ac_gate`; caller runs the probe)
//   2. User Never  — handled by the caller BEFORE calling here (action==Never)
//   3. User Always — this function (Freq / VCache)
//   4. Automatic   — game rules, corral (caller skips a user-ruled target)
//
// Threading: pure function. Privilege: none.
//
#pragma once

#include <phynned/observer/TargetProcess.hpp>   // TargetKind (POD header)
#include <phynned/observer/TargetMetrics.hpp>   // current_core_mask (POD header)

#include <cstdint>

namespace phynned::core {

/// One user-pin decision. Pure value type — carries no capability to act.
struct UserPinDecision {
    bool        would_pin{false};      ///< all non-AC predicates hold → candidate to place
    bool        needs_ac_gate{false};  ///< R1: kind==Game/Unknown → caller MUST AC-probe first
    bool        flap_warn{false};      ///< R4: process is on a foreign (non-full) mask → skip
    bool        already_placed{false}; ///< our own action already holds the target mask (no-op)
    uint64_t    target_mask{0ull};     ///< Freq/V-Cache mask the rule wants applied
    const char* reason{""};            ///< human-readable rule hit (static string)
};

/// Evaluate a user "Always" pin for one tracked process.
///
///   action           — config::ProcessRule::action: 1=Freq, 2=VCache. (0=Never
///                       must be handled by the caller and never reaches here.)
///   kind             — the process's classified TargetKind.
///   tm               — its live metrics (current_core_mask drives the flap guard).
///   our_active_mask  — executor.active_applied_mask(pid): the mask WE currently
///                      hold on this pid, or 0 if we have no active action.
///   vcache_mask      — V-Cache CCD core mask (0 on a non-X3D box → no-op).
///   system_mask      — full unrestricted system core mask (the "not pinned" state).
///   freq_mask        — Frequency CCD core mask (the corral/Freq target).
[[nodiscard]] inline UserPinDecision evaluate_user_pin(
    uint8_t                        action,
    observer::TargetKind           kind,
    const observer::TargetMetrics& tm,
    uint64_t                       our_active_mask,
    uint32_t                       vcache_mask,
    uint32_t                       system_mask,
    uint64_t                       freq_mask) noexcept
{
    UserPinDecision d{};

    // Resolve the target mask from the action.
    if (action == 1u) {           // Freq
        d.target_mask = freq_mask;
        d.reason      = "user Always -> Freq CCD";
    } else if (action == 2u) {    // VCache
        d.target_mask = static_cast<uint64_t>(vcache_mask);
        d.reason      = "user Always -> V-Cache CCD";
    } else {                      // Never / unknown — no placement decision here
        d.reason = "no placement action";
        return d;
    }

    // Graceful degradation on a non-X3D / homogeneous box (E2): no CCD split.
    if (vcache_mask == 0u || freq_mask == 0ull || d.target_mask == 0ull) {
        d.reason = "no CCD split (non-X3D) — nothing to pin";
        return d;
    }

    // R1: a Game or an unknown-class exe must pass the SAME §6 AC probe/oracle
    // gate as automatic placement before any handle is opened. Non-game, known
    // kinds (Comm/Browser/Productivity/Stream) skip the gate.
    d.needs_ac_gate = (kind == observer::TargetKind::Game ||
                       kind == observer::TargetKind::Unknown);

    if (our_active_mask != 0ull) {
        // We already hold an action on this pid.
        if ((our_active_mask & 0xFFFFFFFFull) == (d.target_mask & 0xFFFFFFFFull)) {
            d.already_placed = true;
            d.reason = "already placed by this user rule";
            return d;
        }
        // Ours but on a different mask — still not a foreign manager; place.
    } else {
        // R4 flap guard: we have NO action of ours, yet the process sits on a
        // restricted (non-full-system, readable) mask → another affinity manager
        // pinned it. Never fight it. current_core_mask==0 (unreadable) is NOT a
        // foreign-pin signal, so we still allow the pin there.
        const uint32_t cur = tm.current_core_mask;
        if (cur != 0u && cur != system_mask) {
            d.flap_warn = true;
            d.reason    = "skipped: pinned by another manager (flap guard)";
            return d;
        }
    }

    d.would_pin = true;
    return d;
}

} // namespace phynned::core
// Made with my soul - Swately <3
