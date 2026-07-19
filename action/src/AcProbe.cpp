// action/src/AcProbe.cpp
// AcProbe — implementation of the least-privilege anti-cheat probe (CR1 / M0).
//
// This module is the CR1 safety veto in code. Read the numbered steps below as
// the safety CONTRACT — the ordering (label -> classify -> open) and the "open
// nothing" guarantees on the refused paths are the whole point.
//
#include <phynned/action/AcProbe.hpp>
#include <phynned/action/ActionLog.hpp>

#include <cstddef>
#include <cstring>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace phynned::action {

using observer::AcClass;
using observer::AcDriverOracle;
using learn::LearnedEntry;
using learn::PerGameMemory;

// ── to_string ────────────────────────────────────────────────────────────────
const char* to_string(ProbeResult r) noexcept {
    switch (r) {
        case ProbeResult::Refused_DoNotProbe:    return "Refused_DoNotProbe";
        case ProbeResult::AlreadyLabeledBlocked: return "AlreadyLabeledBlocked";
        case ProbeResult::AlreadyLabeledAllowed: return "AlreadyLabeledAllowed";
        case ProbeResult::Allowed:               return "Allowed";
        case ProbeResult::Blocked:               return "Blocked";
    }
    return "?";
}

namespace {

// ── audit_row — append one AuditLog row for a probe handle event ─────────────
// event "ACOPEN"  = an OpenProcess ATTEMPT (written BEFORE the call). Counting
//                   these gives the number of handle opens the AC could observe.
// event "ACPROBE" = the probe OUTCOME (after the no-op set / on any failure).
// rule_id carries the exact DesiredAccess used; prev/new mask carry the affinity
// read + the no-op value set to it (equal on success).
void audit_row(AuditLog* audit, const char* event, uint32_t pid,
               uint32_t rights, uint64_t prev_mask, uint64_t new_mask,
               uint8_t success) noexcept {
    if (audit == nullptr || !audit->is_open()) return;
    ActionLogEntry e{};
    e.tsc_applied        = static_cast<uint64_t>(__builtin_ia32_rdtsc());
    e.tsc_reverted       = 0u;
    e.prev_affinity_mask = prev_mask;
    e.new_affinity_mask  = new_mask;
    e.target_pid         = pid;
    e.rule_id            = rights;     // exact OpenProcess DesiredAccess
    e.prev_priority_class = 0u;
    e.new_priority_class  = 0u;
    e.success            = success;
    audit->write(e, event);
}

// Bounded, always-null-terminating copy (no strncpy truncation warning).
void copy_str(char* dst, std::size_t dst_sz, const char* src) noexcept {
    if (dst_sz == 0u) return;
    std::size_t n = 0u;
    if (src != nullptr) {
        for (; n + 1u < dst_sz && src[n] != '\0'; ++n) dst[n] = src[n];
    }
    dst[n] = '\0';
}

// ── set_label — write the permanent per-exe AC-probe verdict into memory ─────
// Preserves any existing LearnedEntry (a real policy) and just stamps ac_probe;
// otherwise creates a minimal keyed entry carrying only the verdict.
void set_label(PerGameMemory& mem, const char* exe, uint8_t label) noexcept {
    if (exe == nullptr || exe[0] == '\0') return;
    if (LearnedEntry* e = mem.find(exe)) {
        e->ac_probe = label;
        return;
    }
    LearnedEntry ne{};
    copy_str(ne.exe, sizeof(ne.exe), exe);
    // Key on the current hardware_id so find() locates it again.
    copy_str(ne.hardware_id, sizeof(ne.hardware_id), mem.hardware_id());
    ne.ac_probe = label;
    mem.upsert(ne);
}

} // namespace

// ── probe_and_label — THE safety contract ────────────────────────────────────
ProbeResult AcProbe::probe_and_label(
    uint32_t                        pid,
    const char*                     exe_name,
    const AcDriverOracle&           oracle,
    PerGameMemory&                  mem,
    AuditLog*                       audit) noexcept
{
    // ── Step 1 — prior label short-circuits, opening NOTHING ─────────────────
    if (exe_name != nullptr && exe_name[0] != '\0') {
        if (const LearnedEntry* e = mem.find(exe_name)) {
            if (e->ac_probe == learn::AcProbeBlocked) {
                return ProbeResult::AlreadyLabeledBlocked;   // never re-probe
            }
            if (e->ac_probe == learn::AcProbeAllowed) {
                return ProbeResult::AlreadyLabeledAllowed;   // caller may route
            }
        }
    }

    // ── Step 2 — zero-handle classification gate ─────────────────────────────
    // The oracle NEVER opens a handle on the game; it reads the exe identity and
    // the box's running AC drivers/services. A do-not-probe class means we open
    // no handle at all — no audit row, provably zero opens.
    const AcClass klass = oracle.classify_foreground_game(exe_name);
    if (!AcDriverOracle::probe_allowed(klass)) {
        return ProbeResult::Refused_DoNotProbe;              // OPEN NOTHING
    }

#ifdef _WIN32
    // ── Step 3 — the single least-privilege probe ────────────────────────────
    // Audit the OPEN ATTEMPT *before* the call (durable intent-to-open).
    audit_row(audit, "ACOPEN", pid, kProbeAccess, 0u, 0u, /*success=*/0u);

    const HANDLE h = OpenProcess(kProbeAccess, FALSE, pid);
    if (h == nullptr) {
        // ACCESS_DENIED is the (a) CleanBlock signal — the AC strips the handle
        // at OpenProcess. Any OTHER failure is treated conservatively as BLOCKED.
        set_label(mem, exe_name, learn::AcProbeBlocked);
        audit_row(audit, "ACPROBE", pid, kProbeAccess, 0u, 0u, /*success=*/0u);
        return ProbeResult::Blocked;
    }

    // ── Step 4 — read the current mask, then NO-OP set it to the same value ──
    DWORD_PTR proc_mask = 0u;
    DWORD_PTR sys_mask  = 0u;
    if (!GetProcessAffinityMask(h, &proc_mask, &sys_mask)) {
        // Cannot read the mask => cannot form a safe no-op => conservative BLOCK.
        CloseHandle(h);
        set_label(mem, exe_name, learn::AcProbeBlocked);
        audit_row(audit, "ACPROBE", pid, kProbeAccess, 0u, 0u, /*success=*/0u);
        return ProbeResult::Blocked;
    }

    SetLastError(0u);
    const BOOL set_ok = SetProcessAffinityMask(h, proc_mask);   // NO-OP: same mask
    CloseHandle(h);

    if (!set_ok) {
        // ACCESS_DENIED (or any failure) on the SET right => BLOCKED.
        set_label(mem, exe_name, learn::AcProbeBlocked);
        audit_row(audit, "ACPROBE", pid, kProbeAccess,
                  static_cast<uint64_t>(proc_mask),
                  static_cast<uint64_t>(proc_mask), /*success=*/0u);
        return ProbeResult::Blocked;
    }

    // SET right exercised, affinity unchanged => ALLOWED (label permanent).
    set_label(mem, exe_name, learn::AcProbeAllowed);
    audit_row(audit, "ACPROBE", pid, kProbeAccess,
              static_cast<uint64_t>(proc_mask),
              static_cast<uint64_t>(proc_mask), /*success=*/1u);
    return ProbeResult::Allowed;
#else
    // Non-Windows: no affinity-handle model here; treat as conservative BLOCK.
    (void)pid;
    set_label(mem, exe_name, learn::AcProbeBlocked);
    audit_row(audit, "ACPROBE", pid, kProbeAccess, 0u, 0u, 0u);
    return ProbeResult::Blocked;
#endif
}

} // namespace phynned::action
// Made with my soul - Swately <3
