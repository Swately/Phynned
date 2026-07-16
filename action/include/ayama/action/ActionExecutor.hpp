// apps/ayama/action/include/ayama/action/ActionExecutor.hpp
// ActionExecutor — apply PolicyDecisions and record previous state for revert.
//
// Uses phyriad::hw::set_process_affinity (FR-3) and phyriad::hw::set_process_priority
// (FR-9) — the two free functions exported by phyriad_topology that wrap
// SetProcessAffinityMask / SetPriorityClass with PROCESS_SET_INFORMATION.
// Before applying any change, captures the current affinity/priority so
// revert() can restore it.
//
// All applied actions are logged to an ActionLogRing for UI visibility.
// revert_all() is called on shutdown — guaranteed to run even on crash
// (registered as atexit handler if requested).
//
// Threading: single-thread (agent main thread). NOT re-entrant.
// Resource:  zero heap; all storage in-class.
// Privilege: SetProcessAffinityMask and SetPriorityClass require that the
//            calling process has PROCESS_SET_INFORMATION access on the
//            target. With admin this is always available. Without admin,
//            may fail for elevated targets → returns PermissionDenied.
//
#pragma once

#include <ayama/action/ActionLog.hpp>
#include <ayama/policy/PolicyDecision.hpp>
#include <phyriad/schema/Error.hpp>

#include <array>
#include <cstdint>
#include <expected>

namespace ayama::action {

/// State of one active action (pre-revert snapshot).
struct ActiveAction {
    uint32_t pid;                  ///< Target PID.
    uint32_t rule_id;
    uint64_t prev_affinity_mask;   ///< Affinity before Ayama touched it.
    uint32_t prev_priority_class;  ///< Priority class before.
    uint64_t new_affinity_mask;    ///< Affinity Ayama applied (recorded in audit log).
    uint32_t new_priority_class;   ///< Priority Ayama applied.
    uint64_t tsc_applied;
    bool     active;               ///< True if this action is still in effect.
};

/// Per-thread differential-pin tracking for Rule 7 (PinHotThreadDifferential).
/// One entry per (pid, hot_tid) pair when Rule 7 (PinHotThreadDifferential)
/// is applied. Tracks BOTH the previous process mask AND the previous thread
/// mask so revert can restore both atomically.
struct ActiveThreadAction {
    uint32_t pid;
    uint32_t tid;
    uint64_t prev_process_mask;    ///< Process affinity before differential pin.
    uint64_t prev_thread_mask;     ///< Thread affinity before differential pin.
    uint64_t applied_process_mask; ///< Process mask Ayama applied (typically full system).
    uint64_t applied_thread_mask;  ///< Thread mask Ayama applied (V-Cache CCD).
    uint64_t tsc_applied;
    bool     active;
};

class ActionExecutor {
public:
    ActionExecutor() noexcept;
    ~ActionExecutor() noexcept;

    ActionExecutor(ActionExecutor const&)            = delete;
    ActionExecutor& operator=(ActionExecutor const&) = delete;

    // ── Apply ──────────────────────────────────────────────────────────────
    /// Apply a policy decision. Captures previous state for revert.
    /// Returns Ok on success. On PermissionDenied: logs to stderr, no crash.
    [[nodiscard]] std::expected<void, phyriad::Error>
    apply(const policy::PolicyDecision& d) noexcept;

    /// Apply differential pin (Rule 7 — PinHotThreadDifferential).
    ///
    /// Semantics:
    ///   1. Set process affinity to `process_mask` (typically the full system
    ///      mask — releases workers to use all cores).
    ///   2. Set thread affinity for `tid` to `thread_mask` (typically the
    ///      V-Cache CCD mask — pins the hot thread).
    ///
    /// Records both prev masks for clean revert. Symmetric with apply().
    /// Failure on either step leaves the system in a consistent state
    /// (first step rolled back if second fails).
    [[nodiscard]] std::expected<void, phyriad::Error>
    apply_differential_pin(uint32_t pid,
                           uint32_t tid,
                           uint64_t process_mask,
                           uint64_t thread_mask,
                           uint32_t rule_id) noexcept;

    // ── Revert ─────────────────────────────────────────────────────────────
    /// Revert the active action for `pid` (if any). No-op if no action active.
    /// Also reverts any differential-pin thread actions for this PID.
    void revert(uint32_t pid) noexcept;

    /// Revert ALL active actions. Called on shutdown.
    void revert_all() noexcept;

    /// Drop any active actions whose target PID is no longer in the
    /// caller-supplied "live" set. Does NOT call SetProcessAffinityMask
    /// because the process is already gone — the kernel cleaned it up;
    /// we just need to evict our bookkeeping entries so they stop showing
    /// up in the UI Actions table and the active_count() value.
    ///
    /// `live_pids` may be unsorted; an O(N*M) scan is fine here (both N
    /// and M are <= 32 in steady state).
    ///
    /// Returns the number of entries pruned (across both `active_` and
    /// `active_threads_`).
    uint32_t prune_dead(const uint32_t* live_pids, uint32_t n_live) noexcept;

    // ── Log access ────────────────────────────────────────────────────────
    [[nodiscard]] const ActionLogRing& action_log() const noexcept {
        return log_;
    }

    /// Snapshot the last N log entries. Returns count written.
    [[nodiscard]] uint32_t snapshot_log(ActionLogEntry* out,
                                        uint32_t max) const noexcept;

    [[nodiscard]] uint32_t active_count() const noexcept;

private:
    // ── Pre-allocated active action table ────────────────────────────────
    static constexpr uint32_t kMaxActiveActions = 32u;
    std::array<ActiveAction, kMaxActiveActions> active_{};
    uint32_t n_active_{0u};

    // Per-thread active action table for Rule 7 (PinHotThreadDifferential).
    static constexpr uint32_t kMaxActiveThreadActions = 32u;
    std::array<ActiveThreadAction, kMaxActiveThreadActions> active_threads_{};
    uint32_t n_active_threads_{0u};

    ActiveThreadAction* find_active_thread(uint32_t pid) noexcept;
    void revert_thread_action(ActiveThreadAction& a) noexcept;

    // System-wide default affinity mask cached at construction time.
    // revert() restores targets to this mask instead of the per-action
    // `prev_affinity_mask` captured at apply time.
    //
    // Rationale: if a previous Ayama instance crashed without graceful
    // shutdown, the residual pinned affinity persists in the target. The
    // next instance reads that residual as the target's "previous" affinity
    // when it applies a new policy, and on revert would restore the residual
    // value — never the true OS default. Over multiple crash/restart cycles
    // "default" affinity drifts narrower.
    //
    // Tradeoff: this loses preservation of any pre-Ayama manual user pinning
    // (e.g. user ran `Start /AFFINITY 0xF` before launching the game). For
    // Ayama's target audience (gamers, streamers), this is a non-issue —
    // they don't manually pin. The benefit of guaranteed clean revert across
    // crash recovery dramatically outweighs the cost.
    uint64_t default_affinity_mask_{~0ull};

    ActionLogRing log_;
    // snapshot_log() is stateless — uses write_cursor()/peek_at() with a
    // local cursor each call, so no persistent cursor field is needed.

    ActiveAction* find_active(uint32_t pid) noexcept;
    void log_entry(const ActiveAction& a, bool success,
                   bool is_revert = false) noexcept;

    [[nodiscard]] std::expected<void, phyriad::Error>
    apply_pin_affinity(uint32_t pid, uint64_t mask,
                       uint64_t& out_prev_mask) noexcept;

    [[nodiscard]] std::expected<void, phyriad::Error>
    apply_set_priority(uint32_t pid, uint32_t pclass,
                       uint32_t& out_prev_pclass) noexcept;
};

} // namespace ayama::action
// Made with my soul - Swately <3
