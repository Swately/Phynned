// framework/scheduler/tests/ccd_placement_test.cpp
// CCDAwarePlacement: tests for CcdPreference in PlacementHint.
//
// Tests:
//   §1  Simulated 7950X3D: PreferVCache → CCD0 core; PreferFreq → CCD1 core.
//   §2  Single-CCD topology: both preferences degrade to "all cores".
//   §3  V-Cache CCD saturated: node with PreferVCache falls back to CCD1.
//   §4  PlacementPlan always has valid core_id.
//   §5  CcdPreference::None behaves identically to no hint (existing behavior).
//

#include <phyriad/scheduler/Scheduler.hpp>
#include <phyriad/topology/HardwareTopology.hpp>
#include <cstdio>
#include <vector>

namespace sch = phyriad::scheduler;

// ── Minimal test harness ──────────────────────────────────────────────────────
static int g_pass{0};
static int g_fail{0};

#define SECTION(msg) std::printf("  § %s\n", (msg))
#define EXPECT(cond)                                                            \
    do {                                                                        \
        if (cond) { ++g_pass; }                                                 \
        else {                                                                  \
            ++g_fail;                                                           \
            std::printf("  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                       \
    } while (false)

// ── Topology builders ─────────────────────────────────────────────────────────

// Build a synthetic 7950X3D-like topology:
//   CCD0 (ccd_id=0): 8 cores, has_v_cache=true,  max_freq_mhz=4200  (L3 boosted)
//   CCD1 (ccd_id=1): 8 cores, has_v_cache=false, max_freq_mhz=5700  (higher freq)
// Two CCDs, two NUMA nodes (simplified: NUMA0 = CCD0, NUMA1 = CCD1).
static phyriad::HardwareTopology make_7950x3d_topo() {
    phyriad::HardwareTopology topo{};
    for (uint32_t i = 0; i < 16u; ++i) {
        const uint32_t ccd = i / 8u;   // 0..7 → CCD0, 8..15 → CCD1
        phyriad::CoreInfo c{};
        c.logical_id   = i;
        c.physical_id  = i;
        c.smt_sibling  = UINT32_MAX;  // no SMT for simplicity
        c.ccd_id       = ccd;
        c.ccx_id       = ccd;         // one CCX per CCD in this simulation
        c.numa_node    = 0u;          // single NUMA node (common for desktop)
        c.has_v_cache  = (ccd == 0u);
        c.max_freq_mhz = (ccd == 0u) ? 4200u : 5700u;
        c.l3_cache_kb  = (ccd == 0u) ? 98304u : 32768u;
        topo.cores.push_back(c);
    }
    // Add CCD0 cores to vcache_cores
    for (uint32_t i = 0; i < 8u; ++i)
        topo.vcache_cores.push_back(i);
    return topo;
}

// Single-CCD topology (Intel 12-core, no hybrid, no V-Cache, one CCD).
static phyriad::HardwareTopology make_single_ccd_topo() {
    phyriad::HardwareTopology topo{};
    for (uint32_t i = 0; i < 12u; ++i) {
        phyriad::CoreInfo c{};
        c.logical_id   = i;
        c.physical_id  = i;
        c.smt_sibling  = UINT32_MAX;
        c.ccd_id       = 0u;
        c.ccx_id       = 0u;
        c.numa_node    = 0u;
        c.has_v_cache  = false;
        c.max_freq_mhz = 5000u;
        topo.cores.push_back(c);
    }
    return topo;
}

// ── Helper: return the ccd_id of the assigned core ───────────────────────────
static uint32_t assigned_ccd(phyriad::HardwareTopology const& topo,
                              uint32_t core_id) {
    for (auto const& c : topo.cores)
        if (c.logical_id == core_id) return c.ccd_id;
    return UINT32_MAX;
}

// ── §1 7950X3D simulation ─────────────────────────────────────────────────────
static void test_7950x3d_preferences() {
    SECTION("Test 1: 7950X3D — PreferVCache→CCD0, PreferFreq→CCD1");

    auto topo = make_7950x3d_topo();
    sch::Scheduler sched{sch::PlacementPolicy{.kind = sch::PlacementPolicyKind::Pinned}};

    // Node 0: PreferVCache
    // Node 1: PreferFreq
    std::vector<sch::PlacementHint> hints(2);
    hints[0].ccd_preference = sch::CcdPreference::PreferVCache;
    hints[1].ccd_preference = sch::CcdPreference::PreferFreq;

    auto plan = sched.compute_placement(topo, 2u, hints, {});
    EXPECT(plan.is_complete);
    EXPECT(plan.assignments.size() == 2u);

    const uint32_t ccd0 = assigned_ccd(topo, plan.assignments[0].core_id);
    const uint32_t ccd1 = assigned_ccd(topo, plan.assignments[1].core_id);
    std::printf("    node0 (PreferVCache) → core %u CCD%u (expected CCD0)\n",
        plan.assignments[0].core_id, ccd0);
    std::printf("    node1 (PreferFreq)   → core %u CCD%u (expected CCD1)\n",
        plan.assignments[1].core_id, ccd1);

    EXPECT(ccd0 == 0u);  // V-Cache CCD
    EXPECT(ccd1 == 1u);  // High-freq CCD
}

// ── §2 Single-CCD topology — both preferences degrade ────────────────────────
static void test_single_ccd_degradation() {
    SECTION("Test 2: Single-CCD — PreferVCache and PreferFreq degrade to all cores");

    auto topo = make_single_ccd_topo();
    sch::Scheduler sched{sch::PlacementPolicy{.kind = sch::PlacementPolicyKind::Pinned}};

    std::vector<sch::PlacementHint> hints(2);
    hints[0].ccd_preference = sch::CcdPreference::PreferVCache;
    hints[1].ccd_preference = sch::CcdPreference::PreferFreq;

    auto plan = sched.compute_placement(topo, 2u, hints, {});
    EXPECT(plan.is_complete);
    EXPECT(plan.assignments[0].core_id != UINT32_MAX);  // got some valid core
    EXPECT(plan.assignments[1].core_id != UINT32_MAX);
    // Both must land in CCD0 (the only CCD)
    EXPECT(assigned_ccd(topo, plan.assignments[0].core_id) == 0u);
    EXPECT(assigned_ccd(topo, plan.assignments[1].core_id) == 0u);
}

// ── §3 V-Cache CCD ineligible (budget excludes) — fallback to CCD1 ───────────
// Scenario: CCD0 cores are marked as efficiency cores. Budget disallows
// efficiency cores. All CCD0 candidates are ineligible → fallback fires.
// This tests the second-pass (global) fallback path of the CCD filter.
static void test_vcache_ccd_budget_excluded_fallback() {
    SECTION("Test 3: CCD0 budget-excluded — PreferVCache falls back to CCD1");

    // Topology:
    //   CCD0: 8 "E-cores" with V-Cache (hypothetical — tests fallback logic)
    //   CCD1: 8 P-cores without V-Cache
    phyriad::HardwareTopology topo{};
    for (uint32_t i = 0; i < 16u; ++i) {
        const uint32_t ccd = i / 8u;
        phyriad::CoreInfo c{};
        c.logical_id          = i;
        c.physical_id         = i;
        c.smt_sibling         = UINT32_MAX;
        c.ccd_id              = ccd;
        c.ccx_id              = ccd;
        c.numa_node           = 0u;
        c.has_v_cache         = (ccd == 0u);
        c.max_freq_mhz        = (ccd == 0u) ? 4200u : 5700u;
        c.is_efficiency_core  = (ccd == 0u);  // CCD0 = E-core (budget-excluded)
        c.efficiency_class    = (ccd == 0u) ? 0u : 1u;
        topo.cores.push_back(c);
    }
    for (uint32_t i = 0; i < 8u; ++i) topo.vcache_cores.push_back(i);

    // Budget: E-cores not allowed → all CCD0 cores ineligible.
    sch::ResourceBudget budget{};
    budget.allow_efficiency_cores = false;

    sch::Scheduler sched{sch::PlacementPolicy{.kind = sch::PlacementPolicyKind::Pinned}};

    std::vector<sch::PlacementHint> hints(1);
    hints[0].ccd_preference = sch::CcdPreference::PreferVCache;

    auto plan = sched.compute_placement(topo, 1u, hints, budget);
    EXPECT(plan.is_complete);
    EXPECT(plan.assignments.size() == 1u);

    const uint32_t ccd_n0 = assigned_ccd(topo, plan.assignments[0].core_id);
    std::printf("    node0 (PreferVCache, CCD0 excluded by budget) → core %u CCD%u (expected CCD1)\n",
        plan.assignments[0].core_id, ccd_n0);
    EXPECT(ccd_n0 == 1u);  // fallback to CCD1 (CCD0 cores are E-cores, excluded)
}

// ── §4 PlacementPlan always has valid core_id ─────────────────────────────────
static void test_plan_always_valid() {
    SECTION("Test 4: PlacementPlan has valid core_id in all cases");

    auto topo = make_7950x3d_topo();
    sch::Scheduler sched{sch::PlacementPolicy{.kind = sch::PlacementPolicyKind::Pinned}};

    // Place 20 nodes (more than cores) with mixed preferences.
    std::vector<sch::PlacementHint> hints(20);
    for (uint32_t i = 0; i < 20u; ++i) {
        if      (i % 3 == 0) hints[i].ccd_preference = sch::CcdPreference::PreferVCache;
        else if (i % 3 == 1) hints[i].ccd_preference = sch::CcdPreference::PreferFreq;
        else                  hints[i].ccd_preference = sch::CcdPreference::None;
    }

    auto plan = sched.compute_placement(topo, 20u, hints, {});
    EXPECT(plan.is_complete);
    bool all_valid = true;
    for (auto const& a : plan.assignments) {
        if (a.core_id == UINT32_MAX) all_valid = false;
    }
    EXPECT(all_valid);
}

// ── §5 CcdPreference::None is backward-compatible ────────────────────────────
static void test_none_is_backward_compatible() {
    SECTION("Test 5: CcdPreference::None same as no hint");

    auto topo = make_7950x3d_topo();
    sch::Scheduler sched1{sch::PlacementPolicy{.kind = sch::PlacementPolicyKind::Pinned}};
    sch::Scheduler sched2{sch::PlacementPolicy{.kind = sch::PlacementPolicyKind::Pinned}};

    // Plan A: PlacementHint with ccd_preference=None (default).
    std::vector<sch::PlacementHint> hints_none(4);
    // ccd_preference defaults to None — no explicit set needed.

    // Plan B: empty hints span (uses default PlacementHint per node).
    auto planA = sched1.compute_placement(topo, 4u, hints_none, {});
    auto planB = sched2.compute_placement(topo, 4u, {}, {});

    EXPECT(planA.is_complete && planB.is_complete);
    for (uint32_t i = 0; i < 4u; ++i) {
        EXPECT(planA.assignments[i].core_id == planB.assignments[i].core_id);
    }
}

// ── main ──────────────────────────────────────────────────────────────────────
int main() {
    std::printf("[ccd_placement_test] CCDAwarePlacement\n");
    std::printf("----------------------------------------------------------------\n");

    test_7950x3d_preferences();
    test_single_ccd_degradation();
    test_vcache_ccd_budget_excluded_fallback();
    test_plan_always_valid();
    test_none_is_backward_compatible();

    std::printf("----------------------------------------------------------------\n");
    const int total = g_pass + g_fail;
    if (g_fail == 0)
        std::printf("[OK] %d/%d tests passed\n", g_pass, total);
    else
        std::printf("[FAIL] %d/%d tests FAILED\n", g_fail, total);

    return g_fail ? 1 : 0;
}
// Made with my soul - Swately <3
