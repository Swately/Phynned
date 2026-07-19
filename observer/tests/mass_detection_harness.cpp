// observer/tests/mass_detection_harness.cpp
// MASS-router detection harness (2026-07-17, BR1 / gate 3).
//
// Drives ProcessObserver::refresh() (now track-all-touchable) on the real box and
// reports:
//   - the tracked touchable count (should be 100s, vs the handful under the old
//     pattern-gate),
//   - spot-checks that the denylist / self-managed / PPL exclusions actually fire
//     (csrss / services / audiodg present on the box but NOT tracked; THIS process,
//     after self-restricting its own affinity, is excluded as self-managed),
//   - agent-side RSS after also sampling every tracked PID through the (slimmed,
//     MASS-capped) MetricsCollector — so the number reflects the real mass path.
//
// Exercises the widened caps (kMaxTargets=1024, uint16 pid_hash slot, slim PidState
// + pooled HotTrackState) end to end. Non-destructive: opens only
// PROCESS_QUERY_LIMITED_INFORMATION handles; never sets any affinity on another
// process.
//

#include <phynned/observer/ProcessObserver.hpp>
#include <phynned/observer/MetricsCollector.hpp>
#include <phynned/observer/TargetProcess.hpp>
#include <phyriad/process/ProcessEnumerator.hpp>

#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <psapi.h>
#endif

namespace obs = phynned::observer;

static bool tracked_has_name(const obs::TargetProcess* t, uint32_t n,
                             const char* needle) {
    for (uint32_t i = 0u; i < n; ++i) {
#ifdef _WIN32
        if (_stricmp(t[i].name, needle) == 0) return true;
#else
        if (std::strcmp(t[i].name, needle) == 0) return true;
#endif
    }
    return false;
}

static bool enum_has_name(const char* needle) {
    static thread_local phyriad::proc::ProcessEntry buf[phyriad::proc::kMaxProcesses];
    const uint32_t n = phyriad::proc::enumerate_processes(buf,
                                                          phyriad::proc::kMaxProcesses);
    for (uint32_t i = 0u; i < n; ++i) {
#ifdef _WIN32
        if (_stricmp(buf[i].name, needle) == 0) return true;
#else
        if (std::strcmp(buf[i].name, needle) == 0) return true;
#endif
    }
    return false;
}

int main() {
    int failures = 0;

#ifdef _WIN32
    // Case (d) self-managed: restrict THIS process to CPU0 only so the touchable
    // filter must exclude us (proc_mask != sys_mask).
    const DWORD_PTR self_mask = 0x1;
    const bool self_pinned =
        SetProcessAffinityMask(GetCurrentProcess(), self_mask) != 0;
    const uint32_t self_pid =
        static_cast<uint32_t>(GetCurrentProcessId());
    std::printf("[harness] self pid=%u self-affinity-restricted=%s\n",
                self_pid, self_pinned ? "yes" : "no");
#else
    const uint32_t self_pid = 0u;
#endif

    obs::ProcessObserver observer;
    // Track-all-touchable: NO patterns registered. Under the old pattern-gate this
    // would track 0 processes; under the MASS inversion it tracks the herd.
    for (int i = 0; i < 3; ++i) observer.refresh();

    static obs::TargetProcess snap[obs::kMaxTargets];
    const uint32_t n = observer.snapshot(snap, obs::kMaxTargets);
    std::printf("[harness] tracked touchable count = %u  (kMaxTargets=%u)\n",
                n, obs::kMaxTargets);

    // Print a sample of tracked names (first 24).
    std::printf("[harness] sample tracked: ");
    for (uint32_t i = 0u; i < n && i < 24u; ++i)
        std::printf("%s ", snap[i].name);
    std::printf("%s\n", (n > 24u) ? "..." : "");

    // ── Exclusion spot-checks ─────────────────────────────────────────────
    // Each name should be PRESENT in raw enumeration but ABSENT from tracked.
    const char* denylisted[] = { "csrss.exe", "services.exe", "audiodg.exe",
                                 "smss.exe", "wininit.exe" };
    for (const char* nm : denylisted) {
        const bool present = enum_has_name(nm);
        const bool tracked = tracked_has_name(snap, n, nm);
        if (present && tracked) {
            std::printf("[FAIL] denylisted %s is TRACKED (should be excluded)\n", nm);
            ++failures;
        } else {
            std::printf("[ok]   %s: present=%d tracked=%d\n",
                        nm, present ? 1 : 0, tracked ? 1 : 0);
        }
    }

#ifdef _WIN32
    // (d) self-managed: THIS process (self-restricted above) must NOT be tracked.
    if (self_pinned) {
        bool self_tracked = false;
        for (uint32_t i = 0u; i < n; ++i)
            if (snap[i].pid == self_pid) { self_tracked = true; break; }
        if (self_tracked) {
            std::printf("[FAIL] self (self-managed affinity) is TRACKED\n");
            ++failures;
        } else {
            std::printf("[ok]   self (self-managed affinity) excluded\n");
        }
    }
#endif

    // ── Exercise the MASS metrics path (slim states + pooled hot-track) ───
    obs::MetricsCollector metrics;
    (void)metrics.start();
    static uint32_t pids[obs::kMaxTargets];
    static obs::TargetMetrics tm[obs::kMaxTargets];
    for (int pass = 0; pass < 6; ++pass) {
        const uint32_t m = observer.snapshot(snap, obs::kMaxTargets);
        for (uint32_t i = 0u; i < m; ++i) pids[i] = snap[i].pid;
        metrics.sample(pids, m, tm);
        observer.refresh();
    }
    std::printf("[harness] metrics sampled over the full tracked set (%u pids)\n", n);

#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        std::printf("[harness] RSS (WorkingSet) = %.2f MB   (budget < 20 MB)\n",
                    static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0));
    }
#endif

    std::printf("\n[%s] mass_detection_harness  (tracked=%u, failures=%d)\n",
                (failures == 0) ? "PASS" : "FAIL", n, failures);
    return failures == 0 ? 0 : 1;
}
// Made with my soul - Swately <3
