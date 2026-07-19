// learn/include/phynned/learn/RouteAdvisor.hpp
// RouteAdvisor — MR-1 SHADOW ROUTER: a read-only, measurement-driven DECISION
// layer that recommends a target CCD per active process WITHOUT placing anything.
//
// SCOPE BOUNDARY (the whole point of MR-1): this class produces ADVICE ONLY.
// It performs NO syscalls, holds NO handles, and NEVER touches an affinity mask.
// It cannot construct a policy::PolicyDecision, cannot write decision_buf, and
// cannot reach action::ActionExecutor. Its output is a plain value type
// (RouteAdvice) consumed only by the log + UI advice channel. If a change here
// ever caused an affinity to be applied, MR-1 is destroyed — that is structurally
// impossible: RouteAdvisor has no dependency on action/ and returns pure data.
//
// The routing thesis (7950X3D dual-CCD, ON_BOX_FACTS §6.4):
//   V-Cache CCD  = logical cores 0-15, 96 MB L3, max 5.25 GHz  → cache-sensitive work
//   Frequency CCD= logical cores 16-31, 32 MB L3, max 5.8 GHz  → clock/compute-bound work
//   idle herd    = leave alone (M3 "lo más barato")
//
// These are HEURISTIC PROXIES built from working-set position vs the 32/96 MB L3
// boundaries + CPU activity. The real ground-truth arbiter is the per-app A/B
// route (MR-3), OUT OF SCOPE here. Confidence values are deliberately modest and
// every recommendation is labelled a proxy in its reason string.
//
// Threading: single-thread (agent main thread). No state mutated by advise().
// Resource:  the V-Cache core mask is cached once (from hw::v_cache_cores()).
// Privilege: none — pure arithmetic over already-gathered metrics.
//
#pragma once

#include <phynned/observer/TargetMetrics.hpp>   // POD header (no link dep)
#include <phynned/observer/TargetProcess.hpp>   // TargetKind (POD header)

#include <cstdint>

namespace phynned::learn {

/// One shadow recommendation for a single candidate process. Pure value type —
/// carries no capability to act; the caller may only log/display it.
struct RouteAdvice {
    enum class Ccd : uint8_t {
        LeaveAlone = 0u,   ///< idle herd / system / game / DRAM-bound / no-X3D
        VCache     = 1u,   ///< route toward the V-Cache CCD (cores 0-15)
        Frequency  = 2u,   ///< route toward the frequency CCD (cores 16-31)
    };

    Ccd         ccd{Ccd::LeaveAlone};
    uint8_t     confidence{0u};       ///< 0..100 — heuristic proxy strength, never certainty
    const char* reason{""};           ///< human-readable rule hit (M4a), static string
    bool        already_there{false}; ///< current placement already on the recommended CCD
};

class RouteAdvisor {
public:
    // ── Tunable decision cutoffs (reasoned; see RouteAdvisor.cpp comments) ──
    /// Below this CPU%, a process is the idle herd → LeaveAlone (M3, do-no-harm).
    static constexpr float    kActiveCpuPct   = 3.0f;
    /// Working set that fits BOTH CCDs' L3 (≤ 32 MB freq-L3) → clocks win.
    static constexpr uint32_t kFreqL3Mb       = 32u;
    /// Working set that fits ONLY the V-Cache 96 MB L3 (32 < WS ≤ 96) → cache win.
    static constexpr uint32_t kVCacheL3Mb     = 96u;

    /// Caches the V-Cache core mask from hw::v_cache_cores(). On a non-X3D /
    /// homogeneous box the mask is 0 → advise() degrades to monitor-only
    /// (LeaveAlone for everything), per objective E2 graceful degradation.
    RouteAdvisor() noexcept;

    /// Recommend a CCD for one candidate. Pure function of its inputs; no side
    /// effects. `is_game_managed` = the process is handled by the existing
    /// AC-gated game-placement path (is_placement_eligible) and must be excluded
    /// from advice here.
    [[nodiscard]] RouteAdvice advise(
        const observer::TargetMetrics& m,
        observer::TargetKind           kind,
        bool                           is_game_managed) const noexcept;

    /// The cached V-Cache core mask (bit i set ⇒ logical core i has V-Cache).
    /// 0 on non-X3D. Exposed for tests / diagnostics.
    [[nodiscard]] uint32_t v_cache_mask() const noexcept { return v_cache_mask_; }

private:
    uint32_t v_cache_mask_{0u};

    /// already_there test: is `current_core_mask` fully within the recommended
    /// CCD's core set? (No cores outside it.) On a 2-CCD split, "on the frequency
    /// CCD" == "touches no V-Cache core". current_core_mask==0 (unknown/floating)
    /// ⇒ false (we can't claim it's already placed).
    [[nodiscard]] bool on_ccd(uint32_t current_core_mask,
                              RouteAdvice::Ccd ccd) const noexcept;
};

} // namespace phynned::learn
// Made with my soul - Swately <3
