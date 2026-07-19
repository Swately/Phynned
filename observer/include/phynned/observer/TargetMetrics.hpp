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
#include <cstddef>   // offsetof — MR-1 byte-identical-layout proof

namespace phynned::observer {

/// advice_ccd sentinel (MR-2): the corral WOULD move this active background
/// process off the V-Cache CCD onto the Frequency CCD. Written by AgentRuntime's
/// §3c corral pass over the shadow-router value; whether it is actually applied
/// depends on the LIVE switch (default DRY-RUN = nothing applied).
inline constexpr uint8_t kAdviceCcdWouldCorral = 3u;

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

    // ── SHADOW ROUTER advice (MR-1, 2026-07-17) ──────────────────────────
    // Read-only recommendation written by learn::RouteAdvisor each tick. This
    // is ADVICE ONLY — it is NOT a placement and structurally cannot reach the
    // executor (it rides the read-only SHM publish → log + UI). Carved from the
    // former `_pad_align[2]` (the 2 align bytes before hot_tid) so the struct
    // stays 128B byte-identical and hot_tid alignment is preserved.
    uint8_t  advice_ccd;               //  1B  — 0=none/LeaveAlone, 1=VCache,
                                       //         2=Frequency, 3=WouldCorral (MR-2:
                                       //         active bg the corral WOULD move to
                                       //         the Freq CCD; see kAdviceCcdWouldCorral)
    uint8_t  advice_confidence;        //  1B  — 0..100 (heuristic proxy; MR-3 A/B arbitrates)

    // ── Hot thread (differential pinning, Rule 7) ────────────
    uint32_t hot_tid;                  //  4B  — TID of process's hottest thread
                                       //         (highest sustained CPU time).
                                       //         0 = not yet determined.
                                       //         Computed by MetricsCollector
                                       //         via GFR-Phynned-3 extract_threads.

    uint32_t working_set_mb;           //  4B  — current resident set in MB (RSS).
                                       //         Sampled by MetricsCollector from
                                       //         ProcessMetrics::working_set_bytes.
                                       //         Carved from the former `_pad[4]`
                                       //         → struct stays 128B byte-identical.
                                       //         Feeds the RouteAdvisor 32/96MB proxy.

    // ── Window title ──────────────────────────────────────────────────────
    char     window_title[64];         // 64B  — UTF-8, null-terminated
};
static_assert(sizeof(TargetMetrics)        == 128, "TargetMetrics must be 128B");
static_assert(alignof(TargetMetrics)       == 8,   "TargetMetrics must be 8B-aligned");
static_assert(__is_trivially_copyable(TargetMetrics), "TargetMetrics must be trivially copyable");
// MR-1 byte-identical-layout proof: the advice/working-set fields were carved
// from the former `_pad_align[2]` (@50) and `_pad[4]` (@56); these offset asserts
// prove hot_tid and window_title did NOT move, so the SHM/IPC layout is unchanged.
static_assert(offsetof(TargetMetrics, advice_ccd)        == 50, "advice bytes reuse _pad_align");
static_assert(offsetof(TargetMetrics, advice_confidence) == 51, "advice bytes reuse _pad_align");
static_assert(offsetof(TargetMetrics, hot_tid)           == 52, "hot_tid must not move");
static_assert(offsetof(TargetMetrics, working_set_mb)    == 56, "working_set_mb reuses _pad[4]");
static_assert(offsetof(TargetMetrics, window_title)      == 60, "window_title must not move");

} // namespace phynned::observer
// Made with my soul - Swately <3
