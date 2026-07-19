// action/include/phynned/action/AcProbe.hpp
// AcProbe — the least-privilege anti-cheat handle-open PROBE (CR1 / M0-safety).
//
// This is the CR1 safety mechanism made concrete: the ONE place the mass-router
// is ever allowed to open a handle on a foreground game, and only after the
// zero-handle AcDriverOracle has said it is safe. It is deliberately standalone
// (no ActionExecutor integration yet — that comes after M0 proves this safe).
//
// The safety contract (see AcProbe.cpp for the exact steps):
//   1. A prior per-exe label short-circuits everything (open NOTHING on a title
//      already labelled ALLOWED or BLOCKED — one probe per identity, EVER).
//   2. AcDriverOracle::classify_foreground_game gates the open. do-not-probe
//      classes (SilentPunish_C / UnknownAc) => Refused, ZERO handles opened.
//   3. A single least-privilege OpenProcess (see kProbeAccess) — never a cheat
//      shape (no VM_READ / VM_WRITE / ALL_ACCESS / heavy QUERY).
//   4. A NO-OP SetProcessAffinityMask (set the mask to the same value just read)
//      exercises the SET right WITHOUT changing the game's affinity.
//   Every OpenProcess attempt is audit-logged BEFORE the call.
//
// Threading: single-thread (agent main thread). Privilege: none beyond the
//            SET_INFORMATION handle it opens on the probe-allowed target.
//
#pragma once

#include <cstdint>

#include <phynned/observer/AcDriverOracle.hpp>
#include <phynned/learn/PerGameMemory.hpp>
#include <phynned/action/AuditLog.hpp>

namespace phynned::action {

// ── ProbeResult — the outcome of one probe_and_label call ────────────────────
enum class ProbeResult : uint8_t {
    Refused_DoNotProbe    = 0,  ///< oracle said do-not-probe; ZERO handles opened
    AlreadyLabeledBlocked = 1,  ///< prior BLOCKED label; ZERO handles opened
    AlreadyLabeledAllowed = 2,  ///< prior ALLOWED label; no re-probe (caller may route)
    Allowed               = 3,  ///< probed now: SET right worked, no-op set OK
    Blocked               = 4,  ///< probed now: open/set denied (or conservative)
};

/// Human-readable name for the audit trail / harness output.
[[nodiscard]] const char* to_string(ProbeResult r) noexcept;

// ── The least-privilege probe access rights ──────────────────────────────────
// PROCESS_SET_INFORMATION (0x0200) is the operative SET right — the only right
// SetProcessAffinityMask needs. GetProcessAffinityMask, however, requires a
// query right to READ the current mask for a true no-op set; the LIGHTEST such
// right is PROCESS_QUERY_LIMITED_INFORMATION (0x1000), the same least-privilege
// query the SR1 mitigation blesses (Task-Manager-shaped, NOT a cheat shape).
// Verified on-box 2026-07-17: a 0x0200-only handle fails GetProcessAffinityMask
// with ERROR_ACCESS_DENIED, so 0x1000 is the minimal addition that lets the
// no-op read succeed. NEVER VM_READ / VM_WRITE / PROCESS_QUERY_INFORMATION /
// ALL_ACCESS (the cheat / LSASS-dumper shape the AV/EDR + AC boundary watch for).
inline constexpr uint32_t kProbeSetRight   = 0x0200u; ///< PROCESS_SET_INFORMATION
inline constexpr uint32_t kProbeQueryRight = 0x1000u; ///< PROCESS_QUERY_LIMITED_INFORMATION
inline constexpr uint32_t kProbeAccess     = kProbeSetRight | kProbeQueryRight; ///< 0x1200

// ── AcProbe ──────────────────────────────────────────────────────────────────
class AcProbe {
public:
    /// The CR1 core. Returns a ProbeResult and, when it actually probes, writes
    /// a permanent ALLOWED/BLOCKED label into `mem` for `exe_name` and audit-logs
    /// the handle open to `audit` (may be nullptr to skip auditing).
    ///
    /// Opens AT MOST one handle, and ONLY when the oracle permits it. The
    /// Refused / AlreadyLabeled* paths open no handle at all.
    [[nodiscard]] static ProbeResult probe_and_label(
        uint32_t                              pid,
        const char*                           exe_name,
        const observer::AcDriverOracle&       oracle,
        learn::PerGameMemory&                 mem,
        AuditLog*                             audit) noexcept;
};

} // namespace phynned::action
// Made with my soul - Swately <3
