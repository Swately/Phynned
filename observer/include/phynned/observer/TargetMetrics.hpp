// observer/include/phynned/observer/TargetMetrics.hpp
// TargetMetrics — live performance snapshot for an observed process.
//
// IPC-safe: trivially_copyable, standard_layout, 128 bytes (2 cache lines).
// Crosses SHM boundary between phynned-agent and UI clients.
//
// Threading: written by MetricsCollector (agent thread); read via seqlock SHM.
// Resource:  128B per slot × 32 slots = 4 KB.
// Privilege: GetProcessTimes/NtQuerySystemInformation — no admin required.
//            ETW context-switch events — admin preferred (fallback available).
//
#pragma once
#include <cstdint>

namespace phynned::observer {

/// Live performance snapshot — 128 bytes, 2 cache lines.
struct alignas(8) TargetMetrics {
    // ── Identity ──────────────────────────────────────────────────────────
    uint32_t pid;                      //  4B

    // ── Thread activity ───────────────────────────────────────────────────
    uint32_t observed_threads;         //  4B  — threads seen in last sample
    uint32_t migrations_per_sec;       //  4B  — worst thread migration rate
    uint32_t involuntary_ctxsw_sec;    //  4B  — total involuntary ctx-switches/s

    // ── CPU usage ─────────────────────────────────────────────────────────
    float    cpu_usage_pct;            //  4B  — 0..100, sum across threads

    // ── Frame timing (0 if non-graphical app) ────────────────────────────
    float    frame_time_avg_ms;        //  4B
    float    frame_time_p99_ms;        //  4B
    float    frame_time_variance_ms;   //  4B

    // ── Core placement ────────────────────────────────────────────────────
    uint32_t current_core_mask;        //  4B  — cores currently used by threads
    uint32_t allowed_core_mask;        //  4B  — mask Phynned constrained to (0 = none)

    // ── Timing ────────────────────────────────────────────────────────────
    uint64_t last_sample_tsc;          //  8B

    // ── Pressure ──────────────────────────────────────────────────────────
    uint8_t  pressure_level;           //  1B  — 0=green, 1=yellow, 2=red
    uint8_t  is_throttled;             //  1B  — 1 if CPU was thermal-throttling
    uint8_t  _pad_align[2];            //  2B  — align uint32_t below

    // ── Hot thread (differential pinning, Rule 7) ────────────
    uint32_t hot_tid;                  //  4B  — TID of process's hottest thread
                                       //         (highest sustained CPU time).
                                       //         0 = not yet determined.
                                       //         Computed by MetricsCollector
                                       //         via GFR-Phynned-3 extract_threads.

    uint8_t  _pad[4];                  //  4B  — explicit padding (was 10B; took
                                       //         4B for hot_tid + 2B for align)

    // ── Window title ──────────────────────────────────────────────────────
    char     window_title[64];         // 64B  — UTF-8, null-terminated
};
static_assert(sizeof(TargetMetrics)        == 128, "TargetMetrics must be 128B");
static_assert(alignof(TargetMetrics)       == 8,   "TargetMetrics must be 8B-aligned");
static_assert(__is_trivially_copyable(TargetMetrics), "TargetMetrics must be trivially copyable");

} // namespace phynned::observer
// Made with my soul - Swately <3
