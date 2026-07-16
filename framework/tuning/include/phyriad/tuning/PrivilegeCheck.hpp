// framework/tuning/include/phyriad/tuning/PrivilegeCheck.hpp
// Runtime privilege detection for OS tuning operations.
//
// PrivilegeCheck determines whether the process has the required OS-level
// privileges to perform tuning operations that need elevated access:
//   - Windows: SeIncreaseBasePriorityPrivilege, SeLockMemoryPrivilege
//   - Linux:   CAP_SYS_NICE, CAP_SYS_ADMIN, RLIMIT_MEMLOCK = unlimited
//
// This is a query-only module.  To elevate privileges, the process must be
// started with the appropriate rights — PrivilegeCheck never modifies them.
//
#pragma once
#include <cstdint>

namespace phyriad::tuning {

// ── PrivilegeLevel ────────────────────────────────────────────────────────────
enum class PrivilegeLevel : uint8_t {
    None      = 0,  // no tuning privileges; all operations will fall back
    Partial   = 1,  // some tuning (thread priority) but not real-time class
    Elevated  = 2,  // full real-time class assignment; huge-page lock allowed
    Admin     = 3,  // process has administrator / root + all relevant caps
};

// ── PrivilegeInfo ─────────────────────────────────────────────────────────────
struct PrivilegeInfo {
    PrivilegeLevel level           {PrivilegeLevel::None};
    bool           can_set_rt_prio {false};  // SCHED_FIFO / REALTIME class
    bool           can_lock_pages  {false};  // mlock / SeLockMemoryPrivilege
    bool           can_set_affinity{true};   // SetThreadAffinityMask / sched_setaffinity
    bool           can_raise_timer_resolution {false};  // timeBeginPeriod / HPET
};

// ── PrivilegeCheck ────────────────────────────────────────────────────────────
class PrivilegeCheck {
public:
    // Probe current process privileges.  Cheap (one syscall chain).
    [[nodiscard]] static PrivilegeInfo probe() noexcept;

    // True if we can set OS thread priority to REALTIME / SCHED_FIFO.
    [[nodiscard]] static bool can_set_realtime_priority() noexcept {
        return probe().can_set_rt_prio;
    }

    // True if we can call timeBeginPeriod(1) or set HPET resolution.
    [[nodiscard]] static bool can_set_timer_resolution() noexcept {
        return probe().can_raise_timer_resolution;
    }
};

// ── FR-10: check_privilege_level() free function ─────────────────────────────
// Convenience wrapper for callers that only need the PrivilegeLevel enum
// (e.g. to short-circuit a feature when not elevated) without allocating
// a full PrivilegeInfo on the stack.
// Equivalent to: PrivilegeCheck::probe().level
[[nodiscard]] inline PrivilegeLevel check_privilege_level() noexcept {
    return PrivilegeCheck::probe().level;
}

} // namespace phyriad::tuning
// Made with my soul - Swately <3
