// framework/profile/include/phyriad/profile/PerformanceProfile.hpp
// Performance profile and auto-tuning helpers — canonical home.
//
// Extracted from the `runtime` pillar by Problem 3 of
// PILLAR_COMPOSABILITY_AUDIT so that `pool` (and any other consumer) can take
// a profile dependency without dragging the full graph-runtime stack.
// The old `<phyriad/runtime/PerformanceProfile.hpp>` path still works as a
// shim that re-exports these symbols into the `phyriad::runtime` namespace
// for source compatibility.
//
// PerformanceProfile encapsulates all tunable knobs exposed to the runtime,
// merging the legacy ProfileKind-preset model with Phase 1.B per-node /
// per-wire fine-grained vectors.
//
// Tuning surface:
//   - ProfileKind preset (POWER_SAVE / BALANCED / LATENCY / THROUGHPUT / AUTO / CUSTOM)
//   - Sizing fields (ring_slot_count, channel_arena_mb)
//   - Hardware affinity (enable_cpu_pinning, prefer_v_cache_ccd, elevate_rt_priority,
//                        producer_core_hint, consumer_core_hint, numa_node_hint)
//   - Ring wait policy (ring_producer_wait, ring_consumer_wait, backoff limits)
//   - Slot-copy SIMD dispatch (slot_copy_mode: AUTO / SCALAR / AVX2 / AVX512 / NON_TEMPORAL)
//   - Memory (prefer_hugepages)
//   - Per-node tick budgets (node_profiles[i])     — Phase 1.B vector
//   - Per-wire ring tuning   (ring_profiles[i])    — Phase 1.B vector
//   - Tick-loop globals      (yield_sleep_ns, use_busy_wait_loop, allow_timer_coalescing)
//
// Factory API:
//   PerformanceProfile::make(ProfileKind)          — apply a coarse preset (4 presets)
//   PerformanceProfile::normalize()                — clamp & pow2-round fields
//   make_auto_profile(topo, hints, n_nodes, n_wires) — Phase 1.B graph-aware autotune
//   apply_profile_hints(profile, hints, cores, topo) — post-scheduling refinement
//
//   RingWaitMode + SlotCopyMode + affinity fields + normalize)
#pragma once
#include <phyriad/topology/HardwareTopology.hpp>
#include <phyriad/scheduler/Placement.hpp>
#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace phyriad::profile {

// ── ProfileKind ──────────────────────────────────────────────────────────────
enum class ProfileKind : uint8_t {
    POWER_SAVE,  // minimum power: small rings, SLEEPING wait, no pinning
    BALANCED,    // default: medium rings, YIELD wait, pinning on
    LATENCY,     // tight tail-latency: V-Cache CCD, RT priority, low spin limits
    THROUGHPUT,  // max throughput: large rings, BUSY_SPIN, pinning on
    AUTO,        // topology-derived sizing (uses make_topology_profile internally)
    CUSTOM,      // user filled fields manually (no preset overrides applied)
};

[[nodiscard]] inline const char* profile_name(ProfileKind k) noexcept {
    switch (k) {
        case ProfileKind::POWER_SAVE: return "power_save";
        case ProfileKind::BALANCED:   return "balanced";
        case ProfileKind::LATENCY:    return "latency";
        case ProfileKind::THROUGHPUT: return "throughput";
        case ProfileKind::AUTO:       return "auto";
        case ProfileKind::CUSTOM:     return "custom";
    }
    return "unknown";
}

// ── RingWaitMode ─────────────────────────────────────────────────────────────
// Maps 1:1 to RingWaitBusySpin / Pause / Yield / Backoff / Sleeping in
// transport/RingWaitPolicy.hpp.
enum class RingWaitMode : uint8_t {
    BUSY_SPIN,  // hot spin (no PAUSE) — lowest latency, highest power
    PAUSE,      // PAUSE-only — x86 _mm_pause, ARM YIELD
    YIELD,      // PAUSE → yield() — balanced
    BACKOFF,    // PAUSE → yield → SwitchToThread/sched_yield  — adaptive
    SLEEPING,   // BACKOFF → sleep_for(10µs) — low power, higher latency
};

// ── SlotCopyMode ─────────────────────────────────────────────────────────────
// Maps 1:1 to phyriad::transport::SlotCopyMode in transport/SlotCopy.hpp.
// AUTO uses CPUID runtime dispatch (CPUID leaf 7 EBX bits 5/16).
enum class SlotCopyMode : uint8_t {
    AUTO,          // pick_slot_copy() — CPUID-driven (default)
    SCALAR,        // force scalar (memcpy)
    AVX2,          // force AVX2 256-bit
    AVX512,        // force AVX-512 512-bit (UB if CPU lacks AVX-512F)
    NON_TEMPORAL,  // MOVNTDQ streaming stores for slots >= 256B
};

// ── NodeTickProfile ──────────────────────────────────────────────────────────
// Per-node tuning parameters. Phase 1.B — preserved from refactor.
struct NodeTickProfile {
    uint64_t tick_budget_ns    {200'000};  // max ns a single tick() may take
    uint32_t spin_count        {128};      // busy-wait iterations before yield
    uint32_t yield_after_spins {4};        // yields before sleeping
    bool     latency_histogram {false};    // enable per-node latency recording
};

// ── RingProfile ──────────────────────────────────────────────────────────────
// Ring transport tuning per-wire. Phase 1.B — preserved from refactor.
struct RingProfile {
    uint32_t capacity              {64};     // ring slot count (power-of-2)
    uint32_t backpressure_watermark{48};     // producer parks when this many filled
    bool     strict_backpressure   {true};   // false = overwrite-on-full (lossy)
};

// ── PerformanceProfile ───────────────────────────────────────────────────────
// Aggregate profile — covers both the legacy preset model and the Phase 1.B
// per-node / per-wire vector model.
struct PerformanceProfile {
    ProfileKind kind{ProfileKind::BALANCED};

    // ── Sizing ────────────────────────────────────────────────────────────────
    uint32_t channel_arena_mb{256};     // shared-memory arena size (MiB)
    uint32_t ring_slot_count {1024};    // default ring capacity (pow2)

    // ── Hardware affinity ─────────────────────────────────────────────────────
    // Consumed by GraphRuntime::start() to pin worker threads to specific cores
    // and elevate priority. UINT32_MAX = "auto-resolve via Scheduler".
    bool     enable_cpu_pinning{false};       // pin worker threads to assigned cores
    bool     prefer_v_cache_ccd{false};       // AMD X3D: prefer CCD with V-Cache (96MB L3)
    bool     elevate_rt_priority{false};      // SCHED_FIFO / TIME_CRITICAL on workers
    uint32_t producer_core_hint{UINT32_MAX};  // legacy field — set by Scheduler.compute_placement
    uint32_t consumer_core_hint{UINT32_MAX};  // legacy field — idem
    uint32_t numa_node_hint    {UINT32_MAX};  // NUMA node for arena allocation

    // ── Memory ────────────────────────────────────────────────────────────────
    bool     prefer_hugepages{false};   // MEM_LARGE_PAGES / MAP_HUGETLB, fallback if denied

    // ── Ring wait policy ──────────────────────────────────────────────────────
    RingWaitMode ring_producer_wait{RingWaitMode::YIELD};
    RingWaitMode ring_consumer_wait{RingWaitMode::YIELD};
    uint32_t     ring_backoff_spin_limit {200};  // PAUSE iterations before yield()
    uint32_t     ring_backoff_yield_limit{400};  // yield() iterations before SwitchToThread

    // ── SIMD slot-copy dispatch ──────────────────────────────────────────────
    SlotCopyMode slot_copy_mode{SlotCopyMode::AUTO};

    // ── Per-node / per-wire fine-grained (Phase 1.B) ─────────────────────────
    std::vector<NodeTickProfile> node_profiles{};  // indexed by node_id
    std::vector<RingProfile>     ring_profiles{};  // indexed by wire_id

    // ── Tick-loop globals (Phase 1.B) ────────────────────────────────────────
    uint64_t yield_sleep_ns       {500};   // ns to sleep between yield batches
    bool     use_busy_wait_loop   {true};  // high-res busy-wait for outer loop
    bool     allow_timer_coalescing{false};// Windows timer-coalescence (lowers power)

    // ── Outer-loop rate cap (Phynned UI power fix, 2026-07-19) ──────────────
    // 0 = unpaced (previous behavior: tick as fast as nodes allow, idle branch
    // only yields — effectively burns a core in single-threaded GUI graphs).
    // >0 = the single-threaded run() loop paces ITSELF to this frequency with
    // sleep_until, capping every node (poll/logic/render/present) at that rate.
    // Made for dashboard-style UIs whose data changes ~1×/s: 60 Hz keeps input
    // latency ≤16 ms while cutting the spin. Multi-threaded mode ignores it.
    uint32_t target_loop_hz       {0u};

    // Applies a coarse preset. Does NOT size node_profiles/ring_profiles —
    // those remain empty until the runtime's create() pass fills them
    // (or the caller invokes make_auto_profile() instead).
    [[nodiscard]] static PerformanceProfile make(ProfileKind k) noexcept {
        PerformanceProfile p;
        p.kind = k;
        switch (k) {
            case ProfileKind::POWER_SAVE:
                p.channel_arena_mb         = 64;
                p.ring_slot_count          = 256;
                p.ring_producer_wait       = RingWaitMode::SLEEPING;
                p.ring_consumer_wait       = RingWaitMode::SLEEPING;
                p.ring_backoff_spin_limit  =  50;
                p.ring_backoff_yield_limit = 100;
                p.enable_cpu_pinning       = false;
                p.elevate_rt_priority      = false;
                p.slot_copy_mode           = SlotCopyMode::SCALAR;
                p.allow_timer_coalescing   = true;
                break;
            case ProfileKind::BALANCED:
                p.channel_arena_mb         = 256;
                p.ring_slot_count          = 1024;
                p.ring_producer_wait       = RingWaitMode::YIELD;
                p.ring_consumer_wait       = RingWaitMode::YIELD;
                p.ring_backoff_spin_limit  = 200;
                p.ring_backoff_yield_limit = 400;
                p.enable_cpu_pinning       = true;
                p.elevate_rt_priority      = false;
                p.slot_copy_mode           = SlotCopyMode::AUTO;
                break;
            case ProfileKind::LATENCY:
                p.channel_arena_mb         = 256;
                p.ring_slot_count          = 1024;
                p.ring_producer_wait       = RingWaitMode::YIELD;
                p.ring_consumer_wait       = RingWaitMode::YIELD;
                p.ring_backoff_spin_limit  = 100;
                p.ring_backoff_yield_limit = 200;
                p.enable_cpu_pinning       = true;
                p.prefer_v_cache_ccd       = true;
                p.elevate_rt_priority      = true;
                p.slot_copy_mode           = SlotCopyMode::AUTO;
                break;
            case ProfileKind::THROUGHPUT:
                p.channel_arena_mb         = 1024;
                p.ring_slot_count          = 65536;
                p.ring_producer_wait       = RingWaitMode::BUSY_SPIN;
                p.ring_consumer_wait       = RingWaitMode::BUSY_SPIN;
                p.ring_backoff_spin_limit  = 400;
                p.ring_backoff_yield_limit = 800;
                p.enable_cpu_pinning       = true;
                p.prefer_v_cache_ccd       = false;
                p.elevate_rt_priority      = false;
                p.slot_copy_mode           = SlotCopyMode::AUTO;
                break;
            case ProfileKind::AUTO:
            case ProfileKind::CUSTOM:
                // AUTO is filled by make_auto_profile(); CUSTOM leaves defaults.
                break;
        }
        return p;
    }

    // Clamp out-of-range fields and round ring_slot_count up to a power of two.
    // Emits a stderr warning when adjustments are made so users notice misconfig.
    void normalize() noexcept {
        if (ring_slot_count == 0u) ring_slot_count = 256u;
        if (!std::has_single_bit(ring_slot_count)) {
            const uint32_t rounded = std::bit_ceil(ring_slot_count);
            std::fprintf(stderr,
                "[phyriad] PerformanceProfile::normalize: ring_slot_count %u "
                "not power-of-two — rounding up to %u\n",
                ring_slot_count, rounded);
            ring_slot_count = rounded;
        }
        if (channel_arena_mb < 8u)    channel_arena_mb = 8u;
        if (channel_arena_mb > 8192u) channel_arena_mb = 8192u;
        if (ring_backoff_spin_limit  > ring_backoff_yield_limit)
            ring_backoff_yield_limit = ring_backoff_spin_limit + 1u;
        // kind == CUSTOM after any explicit override caller did before normalize.
        if (kind != ProfileKind::AUTO) kind = ProfileKind::CUSTOM;
    }
};

// ── make_auto_profile ────────────────────────────────────────────────────────
// Phase 1.B — graph-aware autotune. Walks PlacementHints and returns a profile
// with node_profiles/ring_profiles sized + tuned per ThreadRole.
//
// Heuristics (based on PlacementHint::role and hint flags):
//   - prefer_vcache hint      → tighter spin budget (100 µs), more spins
//   - ThreadRole::COMPUTE     → 150 µs budget, 256 spins, latency histogram
//   - ThreadRole::IO          → 500 µs budget, 32 spins
//   - ThreadRole::UI_MAIN     → 8 ms budget (frame), 8 spins
//   - prefer_isolated_core    → tighten budget by 50 µs
[[nodiscard]] inline PerformanceProfile
make_auto_profile(
    phyriad::HardwareTopology const& topo,
    std::vector<phyriad::scheduler::PlacementHint> const& hints,
    uint32_t node_count,
    uint32_t wire_count) noexcept
{
    // Use default-constructed profile (no preset overrides). Callers who want
    // multi-threaded mode must explicitly use PerformanceProfile::make(BALANCED|
    // LATENCY|THROUGHPUT) and optionally layer per-node tuning on top. This
    // keeps run_graph_for/run_graph_n_ticks and runtime_test in their original
    // single-threaded deterministic mode.
    PerformanceProfile p{};
    p.kind = ProfileKind::AUTO;
    p.node_profiles.resize(node_count);
    p.ring_profiles.resize(wire_count);

    // Walk placement hints and tune each node.
    for (uint32_t nid = 0; nid < node_count; ++nid) {
        auto& np = p.node_profiles[nid];

        if (nid < hints.size()) {
            auto const& h = hints[nid];

            // V-Cache preference → tighter budget.
            if (h.prefer_vcache) {
                np.tick_budget_ns = 100'000;
                np.spin_count     = 512;
            }

            // Role-based defaults (applied after, so role wins over flag).
            using enum phyriad::scheduler::ThreadRole;
            switch (h.role) {
            case COMPUTE:
                np.tick_budget_ns    = 150'000;
                np.spin_count        = 256;
                np.latency_histogram = true;
                break;
            case IO:
                np.tick_budget_ns    = 500'000;
                np.spin_count        = 32;
                np.yield_after_spins = 2;
                break;
            case UI_MAIN:
                np.tick_budget_ns    = 8'000'000;  // 8 ms frame budget
                np.spin_count        = 8;
                np.yield_after_spins = 1;
                break;
            default:
                break;
            }

            // Isolation hint → tighten budget (dedicated physical core assumed).
            if (h.prefer_isolated_core) {
                if (np.tick_budget_ns > 50'000)
                    np.tick_budget_ns -= 50'000;
            }
        }

        // topo is available but we defer per-core V-Cache tuning to
        // apply_profile_hints() where actual assignments are known.
        (void)topo;
    }

    // Wire ring defaults.
    for (uint32_t wid = 0; wid < wire_count; ++wid) {
        auto& rp = p.ring_profiles[wid];
        rp.capacity               = 64u;
        rp.backpressure_watermark = 48u;
        rp.strict_backpressure    = true;
    }

    return p;
}

// ── apply_profile_hints ──────────────────────────────────────────────────────
// Overlay per-node PlacementHints on an existing PerformanceProfile.
// Called after scheduling so actual core assignments are known.
//
// assigned_cores[nid] = logical_id of the core the scheduler chose.
inline void apply_profile_hints(
    PerformanceProfile& profile,
    std::vector<phyriad::scheduler::PlacementHint> const& hints,
    std::vector<uint32_t> const& assigned_cores,
    phyriad::HardwareTopology const& topo) noexcept
{
    const uint32_t n = static_cast<uint32_t>(profile.node_profiles.size());

    // Pre-build a sorted-unique set view of vcache_cores for O(log N) lookup
    // instead of legacy O(N) linear scan per node (perf strategy improvement).
    auto const& vc = topo.vcache_cores;
    auto is_vcache_id = [&](uint32_t id) noexcept -> bool {
        return std::binary_search(vc.begin(), vc.end(), id);
    };

    for (uint32_t nid = 0; nid < n; ++nid) {
        auto& np = profile.node_profiles[nid];

        if (nid < assigned_cores.size()) {
            const uint32_t core_id = assigned_cores[nid];
            if (is_vcache_id(core_id)) {
                // V-Cache assigned — tighten tick budget and spin more.
                np.tick_budget_ns = std::min(np.tick_budget_ns, uint64_t{100'000});
                np.spin_count     = std::max(np.spin_count,     uint32_t{512});
            }
        }

        // Honour explicit latency histogram request for COMPUTE nodes.
        if (nid < hints.size() &&
            hints[nid].role == phyriad::scheduler::ThreadRole::COMPUTE)
        {
            np.latency_histogram = true;
        }
    }
}

} // namespace phyriad::profile
// Made with my soul - Swately <3
