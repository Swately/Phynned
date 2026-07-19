// action/include/phynned/action/ActionExecutor.hpp
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

#include <phynned/action/ActionLog.hpp>
#include <phynned/action/RevertJournal.hpp>   // DR1 — crash-durable write-ahead journal
#include <phynned/policy/PolicyDecision.hpp>
#include <phyriad/schema/Error.hpp>

#include <array>
#include <cstdint>
#include <expected>

namespace phynned::action {

/// State of one active action (pre-revert snapshot).
struct ActiveAction {
    uint32_t pid;                  ///< Target PID.
    uint32_t rule_id;
    uint64_t prev_affinity_mask;   ///< Affinity before Phynned touched it (the DR1 revert target).
    uint32_t prev_priority_class;  ///< Priority class before.
    uint64_t new_affinity_mask;    ///< Affinity Phynned applied (recorded in audit log).
    uint32_t new_priority_class;   ///< Priority Phynned applied.
    uint64_t tsc_applied;
    // ── DR1 journal key (pid, creation_time) + exe basename ──────────────────
    // creation_time survives PID recycle; both mirror the RevertJournal record
    // so revert()/revert_all() can flip the journal entry to REVERTED.
    uint64_t creation_time;        ///< Process creation FILETIME (0 if unavailable).
    char     exe_name[64];         ///< Basename, null-terminated (journal key + audit).
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
    uint64_t applied_process_mask; ///< Process mask Phynned applied (typically full system).
    uint64_t applied_thread_mask;  ///< Thread mask Phynned applied (V-Cache CCD).
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
    /// Thin wrapper that self-queries exe/creation-time for the DR1 journal.
    [[nodiscard]] std::expected<void, phyriad::Error>
    apply(const policy::PolicyDecision& d) noexcept;

    /// DR1 overload — plumbs the target's exe basename + process creation-time
    /// (FILETIME) into the write-ahead RevertJournal so a placement survives a
    /// `taskkill /f` of the agent: the next instance's recover() reverts it to
    /// its CAPTURED prev mask (not all-cores). `creation_time == 0` or a
    /// null/empty `exe_name` → apply() self-queries them from the live PID
    /// (TargetProcess carries neither field — see AgentRuntime plumbing).
    [[nodiscard]] std::expected<void, phyriad::Error>
    apply(const policy::PolicyDecision& d,
          const char* exe_name,
          uint64_t    creation_time) noexcept;

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

    /// Selectively revert only the process-level active actions whose
    /// `rule_id` matches `rule_id` (R3 — use-modes). Reuses the exact per-action
    /// revert path (captured prev mask + journal mark_reverted + audit log), then
    /// compacts the table. Used by corral-off and profile transitions to revert
    /// ONLY corral (kCorralRuleId) or user-pin (kUserRuleId) placements while
    /// leaving game pins untouched. Thread-level differential pins (Rule 7, a
    /// game path) are never corral/user rules, so they are intentionally not
    /// touched here. Returns the number of actions reverted.
    ///
    /// `exe_filter` (optional): additionally restrict to actions whose recorded
    /// exe_name matches case-insensitively. Used by user-rule REMOVAL so that
    /// deleting an Always rule also undoes the pins that rule caused (same
    /// off-means-undo semantics the operator chose for the corral switch),
    /// without touching pins from OTHER user rules.
    uint32_t revert_by_rule_id(uint32_t rule_id,
                               const char* exe_filter = nullptr) noexcept;

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

    /// Fix A (MR-2): the affinity mask Phynned currently HAS applied to `pid`,
    /// or 0 if there is no active action for it. Lets AgentRuntime reconcile
    /// TargetMetrics::allowed_core_mask each tick (MetricsCollector zeroes it every
    /// sample), so the UI "routed" (green affinity) signal reflects live executor
    /// state and clears automatically the moment an action is reverted/pruned.
    /// Checks the process-level table first, then the differential-pin table.
    [[nodiscard]] uint64_t active_applied_mask(uint32_t pid) const noexcept;

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
    //
    // INVARIANT CHANGE (DR1, S3 — 2026-07-17): revert()/revert_all() now restore
    // each process-level action to its CAPTURED `prev_affinity_mask`, NOT to this
    // default. The crash-residual-drift hazard that formerly forced the all-cores
    // revert is gone: the write-ahead RevertJournal (opened in the constructor)
    // authoritatively distinguishes "my own live placement" from "a crashed prior
    // instance's residual". On start the constructor calls journal_.recover() and
    // reverts every surviving record whose process_still_matches() to its captured
    // prev BEFORE the tick loop, so a residual can no longer be misread as a
    // "previous" mask and re-restored narrower each crash cycle.
    //
    // default_affinity_mask_ is now used only as (a) the CLAMP for
    // apply_differential_pin's ~0ull "full system" sentinel, (b) the revert
    // target for the NON-journaled differential-pin thread actions (which still
    // revert to default — they are not journaled, so captured-prev would
    // reintroduce the drift), and (c) a fallback when an action has no captured
    // prev (prev_affinity_mask == 0 → nothing to restore, left untouched).
    uint64_t default_affinity_mask_{~0ull};

    // ── DR1: crash-durable write-ahead revert journal ────────────────────────
    // Opened to default_path() in the constructor; record_pending (flushed)
    // BEFORE each Set*, mark_applied after, mark_reverted on revert. recover()
    // runs in the constructor to reconcile a crashed instance's residual.
    RevertJournal journal_;

    ActionLogRing log_;
    // snapshot_log() is stateless — uses write_cursor()/peek_at() with a
    // local cursor each call, so no persistent cursor field is needed.

    ActiveAction* find_active(uint32_t pid) noexcept;
    void log_entry(const ActiveAction& a, bool success,
                   bool is_revert = false) noexcept;

    [[nodiscard]] std::expected<void, phyriad::Error>
    apply_set_priority(uint32_t pid, uint32_t pclass,
                       uint32_t& out_prev_pclass) noexcept;
};

} // namespace phynned::action
// Made with my soul - Swately <3
