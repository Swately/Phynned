// learn/src/RouteAdvisor.cpp
// RouteAdvisor — MR-1 SHADOW ROUTER implementation (advice only; never places).
//
// The decision tree is cheap arithmetic over metrics already gathered by
// MetricsCollector this tick — no per-candidate syscall. See RouteAdvisor.hpp for
// the scope boundary: this file includes NOTHING from action/ and returns pure
// data, so it structurally cannot apply an affinity.
//
#include <phynned/learn/RouteAdvisor.hpp>
#include <phyriad/topology/HardwareTopology.hpp>   // hw::v_cache_cores()

#include <vector>

namespace phynned::learn {

// ── Constructor: cache the V-Cache core mask once ──────────────────────────
// hw::v_cache_cores() is a cached singleton (self-probes on first call), so it
// is safe to call at construction regardless of when the agent probes topology.
// On the 7950X3D it returns {0..15} → mask 0x0000FFFF. Empty on non-X3D → 0,
// which advise() reads as "no asymmetry → monitor-only".
RouteAdvisor::RouteAdvisor() noexcept {
    uint32_t mask = 0u;
    const std::vector<uint32_t> vc = phyriad::hw::v_cache_cores();
    for (const uint32_t core : vc) {
        if (core < 32u) mask |= (1u << core);  // TargetMetrics masks are uint32_t (≤32 cores)
    }
    v_cache_mask_ = mask;
}

// ── on_ccd — already_there test ────────────────────────────────────────────
bool RouteAdvisor::on_ccd(uint32_t current_core_mask,
                          RouteAdvice::Ccd ccd) const noexcept {
    // Unknown / free-floating placement: cannot claim it is already there.
    // NOTE (real-code surprise, MR-1): TargetMetrics::current_core_mask is not
    // yet populated by MetricsCollector (always 0 today), so already_there is
    // effectively always false for the shadow herd. The logic below is
    // future-proof: it becomes meaningful the moment current_core_mask is filled.
    if (current_core_mask == 0u) return false;
    switch (ccd) {
        case RouteAdvice::Ccd::VCache:
            // All used cores must lie inside the V-Cache set.
            return (current_core_mask & ~v_cache_mask_) == 0u;
        case RouteAdvice::Ccd::Frequency:
            // On a 2-CCD split, "on the frequency CCD" == touches no V-Cache core.
            return (current_core_mask & v_cache_mask_) == 0u;
        case RouteAdvice::Ccd::LeaveAlone:
        default:
            return false;
    }
}

// ── advise — the shadow decision ───────────────────────────────────────────
RouteAdvice RouteAdvisor::advise(const observer::TargetMetrics& m,
                                 observer::TargetKind           kind,
                                 bool is_game_managed) const noexcept {
    RouteAdvice a{};   // defaults: LeaveAlone, conf 0

    // ── Graceful degradation (E2): no V-Cache CCD → monitor-only ───────────
    if (v_cache_mask_ == 0u) {
        a.reason = "no V-Cache CCD detected — monitor-only (no CCD asymmetry to route on)";
        return a;
    }

    // ── Exclusions (checked first; these are NEVER routing candidates) ─────
    // System processes are never touched by Phynned.
    if (kind == observer::TargetKind::System) {
        a.reason = "system process — never touched";
        return a;
    }
    // Games / pattern-eligible processes are owned by the existing AC-gated
    // placement path (PolicyEngine → executor.apply). The shadow router must not
    // second-guess them; mark and skip.
    if (is_game_managed) {
        a.reason = "game/eligible — managed by existing placement path";
        return a;
    }

    // ── The M3 idle herd: below the activity floor → leave alone ───────────
    if (m.cpu_usage_pct < kActiveCpuPct) {
        a.reason = "idle (<3% CPU) — background herd, left alone (M3)";
        return a;
    }

    // ── Active candidate → position the working set vs the two L3 sizes ─────
    // The proxy (objective E7 "working-set positioning vs the 32/96 MB boundaries"):
    //   WS ≤ 32 MB          : fits BOTH CCDs' L3 → equal cache residency either
    //                         way → the higher-clock frequency CCD wins for compute.
    //   32 MB < WS ≤ 96 MB  : fits the V-Cache 96 MB L3 but SPILLS out of the 32 MB
    //                         freq-L3 → V-Cache turns those misses into hits (the
    //                         documented V-Cache win region).
    //   WS > 96 MB          : exceeds BOTH L3s → DRAM-bound; neither CCD keeps the
    //                         working set resident, so cache-routing yields no
    //                         residency benefit. Conservative: LeaveAlone (a pure-
    //                         compute win on clocks is possible but unverifiable at
    //                         this shadow layer → deferred to MR-3's A/B arbiter).
    // ALL of these are HEURISTIC PROXIES — working-set size alone cannot prove
    // cache-sensitivity (a 50 MB cache-insensitive compile would be mis-called
    // VCache here); MR-3's cycles-per-unit-work A/B is the ground truth. Hence the
    // modest confidence and the explicit "proxy" wording below.
    const uint32_t ws = m.working_set_mb;

    if (ws > kVCacheL3Mb) {
        // > 96 MB — DRAM-bound; no cache-residency lever on either CCD.
        a.ccd        = RouteAdvice::Ccd::LeaveAlone;
        a.confidence = 30u;   // low — genuinely uncertain without A/B
        a.reason     = "WS >96MB exceeds both L3s -> DRAM-bound, no cache lever "
                       "(frequency win possible but unverified -> MR-3)";
    } else if (ws > kFreqL3Mb) {
        // (32, 96] — fits V-Cache L3, spills freq-L3 -> cache-sensitive proxy.
        a.ccd        = RouteAdvice::Ccd::VCache;
        a.confidence = 60u;   // moderate — the documented V-Cache region, still a proxy
        a.reason     = "WS 32-96MB fits 96MB V-Cache, exceeds 32MB freq-L3 -> "
                       "cache-sensitive (heuristic proxy; MR-3 A/B arbitrates)";
    } else {
        // <= 32 MB — fits both L3s; clock/compute-bound -> higher-clock CCD.
        a.ccd        = RouteAdvice::Ccd::Frequency;
        a.confidence = 55u;   // moderate — small WS is cache-neutral, still a proxy
        a.reason     = "WS <=32MB fits both L3s -> cache-neutral, compute/clock-bound "
                       "-> higher-clock CCD (heuristic proxy; MR-3 A/B arbitrates)";
    }

    a.already_there = on_ccd(m.current_core_mask, a.ccd);
    return a;
}

} // namespace phynned::learn
// Made with my soul - Swately <3
