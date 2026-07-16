// apps/ayama/action/src/ActionExecutor.cpp
// ActionExecutor — implementation.
//
// Uses phyriad::hw::set_process_affinity (FR-3) and phyriad::hw::set_process_priority (FR-9)
// for pin/priority operations — the ONLY invasive operations Ayama performs.
// These are the hw:: free functions in framework/topology (the formerly-
// proposed daemon pillar was retired in favor of in-process FR-3/FR-9 APIs).
//

#include <ayama/action/ActionExecutor.hpp>
#include <phyriad/topology/HardwareTopology.hpp>
#include <phyriad/hal/Timestamp.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace ayama::action {

ActionExecutor::ActionExecutor() noexcept {
    active_.fill(ActiveAction{});
    active_threads_.fill(ActiveThreadAction{});

    // compute the system-wide default affinity mask once. Used
    // by revert() instead of the per-action captured prev_mask, which may be
    // residual pinning from a previous Ayama session that crashed.
    const uint32_t n = phyriad::hw::topology().logical_core_count();
    if (n == 0u || n >= 64u) {
        default_affinity_mask_ = ~0ull;          // all 64 bits
    } else {
        default_affinity_mask_ = (1ull << n) - 1ull;  // low-n bits set
    }
}

ActionExecutor::~ActionExecutor() noexcept {
    revert_all();
}

// ── Internal helpers ──────────────────────────────────────────────────────
ActiveAction* ActionExecutor::find_active(uint32_t pid) noexcept {
    for (uint32_t i = 0u; i < n_active_; ++i)
        if (active_[i].active && active_[i].pid == pid)
            return &active_[i];
    return nullptr;
}

ActiveThreadAction* ActionExecutor::find_active_thread(uint32_t pid) noexcept {
    for (uint32_t i = 0u; i < n_active_threads_; ++i)
        if (active_threads_[i].active && active_threads_[i].pid == pid)
            return &active_threads_[i];
    return nullptr;
}

void ActionExecutor::log_entry(const ActiveAction& a, bool success,
                                bool is_revert) noexcept {
    ActionLogEntry e{};
    e.tsc_applied        = is_revert ? a.tsc_applied  : phyriad::hal::rdtsc();
    e.tsc_reverted       = is_revert ? phyriad::hal::rdtsc() : 0ull;
    e.target_pid         = a.pid;
    e.rule_id            = a.rule_id;
    e.prev_affinity_mask = a.prev_affinity_mask;
    e.prev_priority_class= a.prev_priority_class;
    // On apply: records the mask actually written, not zero.
    // On revert: new_affinity_mask is the system-default mask (what was
    // actually applied). prev_affinity_mask keeps the captured pre-Ayama
    // value so audit shows any drift transparently.
    e.new_affinity_mask  = is_revert ? default_affinity_mask_  : a.new_affinity_mask;
    e.new_priority_class = is_revert ? a.prev_priority_class : a.new_priority_class;
    e.success            = success ? 1u : 0u;
    // push_unchecked: circular-overwrite semantics matching the old ActionLogRing.
    // AuditLog and Publisher use external cursors + skip-ahead guard to handle
    // wrap-around — they never call drain(), so read_seq_ stays at 0.
    // In practice (≤32 active actions, low event rate) the ring never wraps.
    (void)log_.push_unchecked(e);
}

// ── Platform-level apply operations ──────────────────────────────────────
std::expected<void, phyriad::Error>
ActionExecutor::apply_pin_affinity(uint32_t pid, uint64_t mask,
                                    uint64_t& out_prev_mask) noexcept {
    // phyriad::hw::set_process_affinity (FR-3) reads the previous mask and applies
    // the new one atomically — returns the old mask on success.
    auto r = phyriad::hw::set_process_affinity(pid, mask);
    if (!r) {
        std::fprintf(stderr,
            "[Ayama][ActionExecutor] set_process_affinity(pid=%u, mask=0x%llx) failed: %d\n",
            pid, static_cast<unsigned long long>(mask),
            static_cast<int>(r.error().code));
        return std::unexpected(r.error());
    }
    out_prev_mask = *r;
    return {};
}

std::expected<void, phyriad::Error>
ActionExecutor::apply_set_priority(uint32_t pid, uint32_t pclass,
                                    uint32_t& out_prev_pclass) noexcept {
    // phyriad::hw::set_process_priority (FR-9) reads the previous priority class and
    // applies the new one — returns the old class on success.
    auto r = phyriad::hw::set_process_priority(pid, pclass);
    if (!r) {
        std::fprintf(stderr,
            "[Ayama][ActionExecutor] set_process_priority(pid=%u, class=%u) failed: %d\n",
            pid, pclass, static_cast<int>(r.error().code));
        return std::unexpected(r.error());
    }
    out_prev_pclass = *r;
    return {};
}

// ── apply() ───────────────────────────────────────────────────────────────
std::expected<void, phyriad::Error>
ActionExecutor::apply(const policy::PolicyDecision& d) noexcept {
    using policy::ActionKind;

    // If there's already an active action for this PID, skip (already applied).
    if (find_active(d.target_pid) != nullptr) return {};

    if (n_active_ >= kMaxActiveActions) {
        return std::unexpected(phyriad::Error{phyriad::ErrorCode::BufferFull});
    }

    ActiveAction a{};
    a.pid        = d.target_pid;
    a.rule_id    = d.rule_id;
    a.tsc_applied = phyriad::hal::rdtsc();
    a.active     = true;

    bool success = false;

    if (d.action_kind == ActionKind::PinAffinity) {
        auto r = apply_pin_affinity(d.target_pid, d.core_mask,
                                    a.prev_affinity_mask);
        if (r) {
            success = true;
            a.new_affinity_mask = d.core_mask;   // fix #12 — record applied mask
            std::fprintf(stdout,
                "[Ayama] PinAffinity: pid=%u mask=0x%llx (prev=0x%llx)\n",
                d.target_pid,
                static_cast<unsigned long long>(d.core_mask),
                static_cast<unsigned long long>(a.prev_affinity_mask));
        } else {
            return r;
        }
    } else if (d.action_kind == ActionKind::SetPriority) {
        auto r = apply_set_priority(d.target_pid, d.priority_class,
                                    a.prev_priority_class);
        if (r) {
            success = true;
            a.new_priority_class = d.priority_class;  // fix #12 — record applied class
        } else {
            return r;
        }
    } else if (d.action_kind == ActionKind::Revert) {
        revert(d.target_pid);
        return {};
    }

    if (success) {
        active_[n_active_++] = a;
        log_entry(a, true, false);
    }

    return {};
}

// ── apply_differential_pin ────────────────────────────────────────────────
std::expected<void, phyriad::Error>
ActionExecutor::apply_differential_pin(uint32_t pid,
                                        uint32_t tid,
                                        uint64_t process_mask,
                                        uint64_t thread_mask,
                                        uint32_t rule_id) noexcept {
    // Skip if already applied for this PID (idempotent).
    if (find_active_thread(pid) != nullptr) return {};

    if (n_active_threads_ >= kMaxActiveThreadActions) {
        return std::unexpected(phyriad::Error{phyriad::ErrorCode::BufferFull});
    }

    // Clamp both masks to the system-supported logical CPU set. Caller may
    // pass ~0ull as a "full system" sentinel, but SetProcessAffinityMask
    // rejects bits for non-existent cores. default_affinity_mask_ is
    // (1 << logical_core_count) - 1, computed once at construction.
    const uint64_t safe_process_mask = process_mask & default_affinity_mask_;
    const uint64_t safe_thread_mask  = thread_mask  & default_affinity_mask_;
    if (safe_process_mask == 0ull || safe_thread_mask == 0ull) {
        // Both must have at least one core in the system mask. If caller
        // passed a mask that intersected to empty, that's a programmer error.
        return std::unexpected(phyriad::Error{phyriad::ErrorCode::InvalidArgument});
    }

    // ── Step 1: set process affinity ────────────────────────────────────
    auto pr = phyriad::hw::set_process_affinity(pid, safe_process_mask);
    if (!pr) {
        std::fprintf(stderr,
            "[Ayama][ActionExecutor] apply_differential_pin: "
            "set_process_affinity(pid=%u, mask=0x%llx) failed: %d\n",
            pid, static_cast<unsigned long long>(safe_process_mask),
            static_cast<int>(pr.error().code));
        return std::unexpected(pr.error());
    }
    const uint64_t prev_process_mask = *pr;

    // ── Step 2: set thread affinity (GFR-Ayama-2) ──────────────────────
    auto tr = phyriad::hw::set_thread_affinity(tid, safe_thread_mask);
    if (!tr) {
        // Roll back process affinity change to leave system consistent.
        (void)phyriad::hw::set_process_affinity(pid, prev_process_mask);
        std::fprintf(stderr,
            "[Ayama][ActionExecutor] apply_differential_pin: "
            "set_thread_affinity(tid=%u, mask=0x%llx) failed: %d (rolled back)\n",
            tid, static_cast<unsigned long long>(safe_thread_mask),
            static_cast<int>(tr.error().code));
        return std::unexpected(tr.error());
    }
    const uint64_t prev_thread_mask = *tr;

    // ── Record active state for revert ──────────────────────────────────
    ActiveThreadAction& a = active_threads_[n_active_threads_++];
    a.pid                   = pid;
    a.tid                   = tid;
    a.prev_process_mask     = prev_process_mask;
    a.prev_thread_mask      = prev_thread_mask;
    a.applied_process_mask  = safe_process_mask;  // post-clamp value
    a.applied_thread_mask   = safe_thread_mask;   // post-clamp value
    a.tsc_applied           = phyriad::hal::rdtsc();
    a.active                = true;

    std::fprintf(stdout,
        "[Ayama] DifferentialPin: pid=%u tid=%u proc=0x%llx thread=0x%llx "
        "(prev_proc=0x%llx prev_thread=0x%llx)\n",
        pid, tid,
        static_cast<unsigned long long>(safe_process_mask),
        static_cast<unsigned long long>(safe_thread_mask),
        static_cast<unsigned long long>(prev_process_mask),
        static_cast<unsigned long long>(prev_thread_mask));

    // Audit log: synthesize an ActionLogEntry. Reuse the existing log_entry
    // helper by constructing a temporary ActiveAction shim with rule_id 7.
    ActiveAction shim{};
    shim.pid                 = pid;
    shim.rule_id             = rule_id;
    shim.prev_affinity_mask  = prev_process_mask;
    shim.new_affinity_mask   = safe_process_mask;  // what was actually applied
    shim.tsc_applied         = a.tsc_applied;
    shim.active              = true;
    log_entry(shim, true, false);

    return {};
}

void ActionExecutor::revert_thread_action(ActiveThreadAction& a) noexcept {
    if (!a.active) return;
    // Revert thread affinity first (it's the more invasive change).
    (void)phyriad::hw::set_thread_affinity(a.tid, default_affinity_mask_);
    // Then revert process affinity.
    (void)phyriad::hw::set_process_affinity(a.pid, default_affinity_mask_);

    std::fprintf(stdout,
        "[Ayama] DifferentialPin revert: pid=%u tid=%u -> default=0x%llx "
        "(captured prev_proc=0x%llx prev_thread=0x%llx)\n",
        a.pid, a.tid,
        static_cast<unsigned long long>(default_affinity_mask_),
        static_cast<unsigned long long>(a.prev_process_mask),
        static_cast<unsigned long long>(a.prev_thread_mask));

    // Audit log: revert entry. Shim uses rule_id-7 to indicate differential.
    ActiveAction shim{};
    shim.pid                 = a.pid;
    shim.rule_id             = 7u;  // PinHotThreadDifferential
    shim.prev_affinity_mask  = a.prev_process_mask;
    shim.tsc_applied         = a.tsc_applied;
    shim.active              = true;
    log_entry(shim, true, true);

    a.active = false;
}

// ── revert() ──────────────────────────────────────────────────────────────
void ActionExecutor::revert(uint32_t pid) noexcept {
    // First check for differential-pin thread action and revert that path.
    if (ActiveThreadAction* ta = find_active_thread(pid)) {
        revert_thread_action(*ta);
        // Compact thread table
        uint32_t write = 0u;
        for (uint32_t i = 0u; i < n_active_threads_; ++i) {
            if (active_threads_[i].active) {
                if (write != i) active_threads_[write] = active_threads_[i];
                ++write;
            }
        }
        n_active_threads_ = write;
        // Note: a differential pin replaces Rule 1 for this PID, so there
        // should NOT be a parallel ActiveAction entry. Fall through anyway
        // to be defensive (handles case where both were somehow applied).
    }

    ActiveAction* a = find_active(pid);
    if (!a) return;

    // Restore to system-default affinity, not the per-action captured mask.
    // The captured mask may be a residual from a previous Ayama instance
    // that crashed without calling revert_all(); restoring it would
    // propagate the leak.
    //
    // Restoring to the system-wide default (all logical cores) guarantees
    // clean state regardless of crash-recovery history. See ActiveAction.hpp
    // member-comment for the tradeoff analysis.
    //
    // We still apply the revert only if the action actually changed affinity
    // (prev_affinity_mask != 0 means apply() captured something — i.e. a
    // PinAffinity action was executed for this PID).
    if (a->prev_affinity_mask != 0ull) {
        (void)phyriad::hw::set_process_affinity(pid, default_affinity_mask_);
        std::fprintf(stdout,
            "[Ayama] Revert: pid=%u -> mask=0x%llx (default; captured prev=0x%llx)\n",
            pid,
            static_cast<unsigned long long>(default_affinity_mask_),
            static_cast<unsigned long long>(a->prev_affinity_mask));
    }

    // Restore previous priority (priority class isn't affected by bug #19 —
    // Windows priority classes are stable across process lifetime).
    if (a->prev_priority_class != 0u) {
        (void)phyriad::hw::set_process_priority(pid, a->prev_priority_class);
    }

    // Audit log: prev_affinity_mask in the entry keeps the originally captured
    // value (may be residual leak — visible to operators); new_affinity_mask
    // reflects what was ACTUALLY applied on revert (default_affinity_mask_).
    // See log_entry() — the is_revert branch uses default_affinity_mask_.
    log_entry(*a, true, true);
    a->active = false;

    // Compact active table
    uint32_t write = 0u;
    for (uint32_t i = 0u; i < n_active_; ++i) {
        if (active_[i].active) {
            if (write != i) active_[write] = active_[i];
            ++write;
        }
    }
    n_active_ = write;
}

// ── revert_all() ──────────────────────────────────────────────────────────
void ActionExecutor::revert_all() noexcept {
    // Revert differential-pin actions first (each touches both process AND
    // thread affinity). revert_thread_action() does not compact the array,
    // so a straight loop is safe here.
    for (uint32_t i = 0u; i < n_active_threads_; ++i) {
        if (!active_threads_[i].active) continue;
        revert_thread_action(active_threads_[i]);
    }
    n_active_threads_ = 0u;

    // Then revert process-only actions (Rule 1 etc.).
    //
    // Inline the per-PID revert logic without compaction, then zero n_active_
    // once at the end. Calling revert(active_[i].pid) here would compact the
    // array mid-iteration, shifting indices and causing the outer i++ to skip
    // entries — leaving some actions unreversed at the OS level while
    // n_active_ was already zeroed.
    for (uint32_t i = 0u; i < n_active_; ++i) {
        ActiveAction& a = active_[i];
        if (!a.active) continue;

        if (a.prev_affinity_mask != 0ull) {
            (void)phyriad::hw::set_process_affinity(a.pid, default_affinity_mask_);
            std::fprintf(stdout,
                "[Ayama] Revert: pid=%u -> mask=0x%llx "
                "(default; captured prev=0x%llx)\n",
                a.pid,
                static_cast<unsigned long long>(default_affinity_mask_),
                static_cast<unsigned long long>(a.prev_affinity_mask));
        }
        if (a.prev_priority_class != 0u) {
            (void)phyriad::hw::set_process_priority(a.pid, a.prev_priority_class);
        }
        log_entry(a, true, true);
        a.active = false;
    }
    n_active_ = 0u;
}

// ── prune_dead() ──────────────────────────────────────────────────────────
// Walk the active tables and drop any entry whose PID is no longer present
// in `live_pids`. Used by AgentRuntime each tick after observer.snapshot()
// to keep the active action set in sync with the live target set — without
// this, an action applied to a process that later exited would remain in
// the UI's "active actions" table forever (until revert_all on shutdown).
//
// We do NOT call set_process_affinity / set_process_priority on dead PIDs;
// the kernel already freed those handles, and a syscall against a dead PID
// would just return ERROR_INVALID_PARAMETER without doing anything useful.
// Pruning is bookkeeping-only: emit a "revert" audit log entry so operators
// can see the action ended, then compact the tables.
uint32_t ActionExecutor::prune_dead(const uint32_t* live_pids,
                                     uint32_t n_live) noexcept
{
    auto is_live = [&](uint32_t pid) noexcept -> bool {
        for (uint32_t i = 0u; i < n_live; ++i) {
            if (live_pids[i] == pid) return true;
        }
        return false;
    };

    uint32_t pruned = 0u;

    // ── Process-level actions (Rule 1 etc.) ──────────────────────────────
    {
        uint32_t write = 0u;
        for (uint32_t i = 0u; i < n_active_; ++i) {
            const ActiveAction& a = active_[i];
            if (!a.active) continue;
            if (is_live(a.pid)) {
                if (write != i) active_[write] = a;
                ++write;
            } else {
                // Log target-loss revert for the UI/audit trail.
                ActiveAction dead = a;
                dead.active = true;  // ensure log_entry treats it as committed
                log_entry(dead, true, true);
                ++pruned;
            }
        }
        n_active_ = write;
    }

    // ── Thread-level differential pins (Rule 7) ──────────────────────────
    {
        uint32_t write = 0u;
        for (uint32_t i = 0u; i < n_active_threads_; ++i) {
            const ActiveThreadAction& a = active_threads_[i];
            if (!a.active) continue;
            if (is_live(a.pid)) {
                if (write != i) active_threads_[write] = a;
                ++write;
            } else {
                ++pruned;
            }
        }
        n_active_threads_ = write;
    }

    return pruned;
}

// ── snapshot_log() ────────────────────────────────────────────────────────
uint32_t ActionExecutor::snapshot_log(ActionLogEntry* out,
                                       uint32_t max) const noexcept {
    if (!out || max == 0u) return 0u;
    const uint64_t head = log_.write_cursor();
    // Start from (head - max) so we get the most recent `max` entries.
    uint64_t cursor = (head > static_cast<uint64_t>(max))
                      ? head - static_cast<uint64_t>(max) : 0ull;
    uint32_t n = 0u;
    while (cursor < head && n < max) {
        // Guard against wrap-around overwrite by the producer.
        if (head - cursor > kActionLogCap) cursor = head - kActionLogCap;
        ActionLogEntry e{};
        if (log_.peek_at(cursor, e)) out[n++] = e;
        ++cursor;
    }
    return n;
}

uint32_t ActionExecutor::active_count() const noexcept {
    return n_active_;
}

} // namespace ayama::action
// Made with my soul - Swately <3
