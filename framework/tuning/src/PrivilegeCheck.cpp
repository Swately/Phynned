// framework/tuning/src/PrivilegeCheck.cpp
// PrivilegeCheck::probe() — platform implementation.
#include <phyriad/tuning/PrivilegeCheck.hpp>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>

namespace phyriad::tuning {

[[nodiscard]] PrivilegeInfo PrivilegeCheck::probe() noexcept {
    PrivilegeInfo info{};

    // Always-available capabilities on Windows.
    info.can_raise_timer_resolution = true;
    info.can_set_affinity           = true;

    // ── Priority class queries (independent of admin level) ────────────────
    // If the process is already at HIGH/REALTIME priority, it can set its
    // threads to the same class. This signals "can run threads in RT class"
    // — NOT an admin signal.
    const DWORD cls = GetPriorityClass(GetCurrentProcess());
    const bool is_rt   = (cls == REALTIME_PRIORITY_CLASS);
    const bool is_high = (cls == HIGH_PRIORITY_CLASS);
    info.can_set_rt_prio = is_rt || is_high;

    // ── Elevation check — the correct "admin" signal on Windows ───────────
    // TokenElevation directly reflects "Run as Administrator" UAC elevation.
    // (GetPriorityClass is wrong for this: a normal admin process runs at
    // NORMAL_PRIORITY_CLASS and would never be detected as Admin even when
    // UAC-elevated. TokenElevation is authoritative.)
    bool is_elevated = false;
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) && token) {
        TOKEN_ELEVATION elev{};
        DWORD sz = sizeof(elev);
        if (GetTokenInformation(token, TokenElevation, &elev, sz, &sz)) {
            is_elevated = (elev.TokenIsElevated != 0);
        }

        // SeLockMemoryPrivilege generally requires explicit assignment via
        // secpol.msc or gpedit ("Lock pages in memory"), even for admins.
        // Tracked as an independent signal; does NOT imply Admin level.
        LUID luid{};
        if (LookupPrivilegeValueA(nullptr, SE_LOCK_MEMORY_NAME, &luid)) {
            PRIVILEGE_SET ps{};
            ps.PrivilegeCount = 1;
            ps.Control        = PRIVILEGE_SET_ALL_NECESSARY;
            ps.Privilege[0].Luid       = luid;
            ps.Privilege[0].Attributes = 0;

            BOOL has_it = FALSE;
            // ::PrivilegeCheck to reach the Win32 API, not phyriad::tuning::PrivilegeCheck.
            if (::PrivilegeCheck(token, &ps, &has_it) && has_it) {
                info.can_lock_pages = true;
            }
        }
        CloseHandle(token);
    }

    // ── Privilege levels ───────────────────────────────────────────────────
    // Admin    = UAC elevated (TokenIsElevated). Required for ETW kernel
    //            sessions and SetProcessAffinityMask on other users' processes.
    // Elevated = not UAC-elevated but holds SeLockMemoryPrivilege or RT class
    //            (rare: service accounts).
    // Partial  = can set RT priority on own threads only (HIGH priority class).
    // None     = normal process with no special privileges.
    if (is_elevated) {
        info.level = PrivilegeLevel::Admin;
    } else if (info.can_lock_pages || is_rt) {
        info.level = PrivilegeLevel::Elevated;
    } else if (info.can_set_rt_prio) {
        info.level = PrivilegeLevel::Partial;
    } else {
        info.level = PrivilegeLevel::None;
    }

    return info;
}

} // namespace phyriad::tuning

#else  // POSIX / Linux

#include <sys/resource.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef __linux__
#  include <sys/capability.h>
#endif

namespace phyriad::tuning {

[[nodiscard]] PrivilegeInfo PrivilegeCheck::probe() noexcept {
    PrivilegeInfo info{};

    // Timer resolution: always available on Linux (clock_nanosleep, etc.)
    info.can_raise_timer_resolution = true;
    info.can_set_affinity = true;

    const bool is_root = (geteuid() == 0);
    info.can_lock_pages  = is_root;
    info.can_set_rt_prio = is_root;

    // Check RLIMIT_RTPRIO.
    if (!is_root) {
        struct rlimit rl{};
        if (getrlimit(RLIMIT_RTPRIO, &rl) == 0) {
            if (rl.rlim_cur > 0 || rl.rlim_cur == RLIM_INFINITY) {
                info.can_set_rt_prio = true;
            }
        }

        // Check RLIMIT_MEMLOCK.
        if (getrlimit(RLIMIT_MEMLOCK, &rl) == 0) {
            if (rl.rlim_cur >= 64 * 1024 * 1024 || rl.rlim_cur == RLIM_INFINITY) {
                info.can_lock_pages = true;
            }
        }
    }

    if (is_root) {
        info.level = PrivilegeLevel::Admin;
    } else if (info.can_set_rt_prio && info.can_lock_pages) {
        info.level = PrivilegeLevel::Elevated;
    } else if (info.can_set_rt_prio) {
        info.level = PrivilegeLevel::Partial;
    } else {
        info.level = PrivilegeLevel::None;
    }

    return info;
}

} // namespace phyriad::tuning

#endif  // !_WIN32
// Made with my soul - Swately <3
