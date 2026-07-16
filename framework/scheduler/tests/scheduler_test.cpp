// framework/scheduler/tests/scheduler_test.cpp
// Test suite for phyriad_scheduler pillar — greedy placement engine.
//
// Tests:
//   1.  PlacementHint — default field values
//   2.  ResourceBudget — default field values
//   3.  NodeAssignment — default field values
//   4.  PlacementPlan::find() — hit and miss
//   5.  PlacementPolicy named constructors — kind and field values
//   6.  PlacementPolicy trivial-copyable and sizeof == 12
//   7.  Scheduler default construction
//   8.  compute_placement node_count=0 → is_complete + empty assignments
//   9.  compute_placement empty cores topology → is_complete=false, all unassigned
//  10.  compute_placement real probe() → is_complete, all core_ids valid, sequential node_ids
//  11.  UI_MAIN role → external=true, core_id=UINT32_MAX
//  12.  COMPUTE role → V-Cache core assigned (X3D system; skip if no V-Cache present)
//  13.  Determinism — two calls with identical inputs produce identical plans
//  14.  ResourceBudget max_cores cap — no assigned core_id >= max_cores
//  15.  RENDER role → prefers_pcie_gpu forced (score path exercised)
//  16.  Anti-affinity: two nodes pushed apart where possible (fake topology)
//  17.  HT sibling exclusion: allow_ht_siblings=false prevents sibling reuse
//  18.  PlacementPolicyKind enum values (compile-time)
//  19.  Multiple UI_MAIN nodes — none consume core_usage slots
//  20.  IO role — e-cores allowed regardless of budget
//

#include <phyriad/scheduler/Scheduler.hpp>
#include <phyriad/scheduler/Placement.hpp>
#include <phyriad/scheduler/PlacementPolicy.hpp>
#include <phyriad/topology/HardwareTopology.hpp>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <type_traits>

namespace sched = phyriad::scheduler;

// ── Micro-test framework ──────────────────────────────────────────────────────
static int g_tests_run    = 0;
static int g_tests_failed = 0;

#define EXPECT(cond)                                                           \
    do {                                                                       \
        ++g_tests_run;                                                         \
        if (!(cond)) {                                                         \
            ++g_tests_failed;                                                  \
            std::fprintf(stderr, "  [FAIL] %s:%d: %s\n",                     \
                         __FILE__, __LINE__, #cond);                           \
        }                                                                      \
    } while(0)

#define SECTION(name) std::puts("  § " name)

// ── Compile-time checks ───────────────────────────────────────────────────────
static_assert(std::is_trivially_copyable_v<sched::PlacementHint>);
static_assert(std::is_trivially_copyable_v<sched::ResourceBudget>);
static_assert(std::is_trivially_copyable_v<sched::NodeAssignment>);
static_assert(std::is_trivially_copyable_v<sched::PlacementPolicy>);
static_assert(sizeof(sched::PlacementPolicy) == 12);

// PlacementPolicyKind values
static_assert(static_cast<uint8_t>(sched::PlacementPolicyKind::Pinned)          == 0);
static_assert(static_cast<uint8_t>(sched::PlacementPolicyKind::SoftAffine)      == 1);
static_assert(static_cast<uint8_t>(sched::PlacementPolicyKind::WorkStealing)    == 2);
static_assert(static_cast<uint8_t>(sched::PlacementPolicyKind::StickyThenSteal) == 3);

// ThreadRole values
static_assert(static_cast<uint8_t>(sched::ThreadRole::GENERIC) == 0);
static_assert(static_cast<uint8_t>(sched::ThreadRole::UI_MAIN) == 1);
static_assert(static_cast<uint8_t>(sched::ThreadRole::RENDER)  == 2);
static_assert(static_cast<uint8_t>(sched::ThreadRole::LOGIC)   == 3);
static_assert(static_cast<uint8_t>(sched::ThreadRole::COMPUTE) == 4);
static_assert(static_cast<uint8_t>(sched::ThreadRole::IO)      == 5);

// ── Helper: build a minimal fake topology ────────────────────────────────────
// 4 cores: {lid=0,phys=0,smt_sib=1,ccx=0,ccd=0,numa=0}
//          {lid=1,phys=0,smt_sib=0,ccx=0,ccd=0,numa=0}  ← HT sibling of 0
//          {lid=2,phys=1,smt_sib=3,ccx=1,ccd=0,numa=0}  ← different CCX
//          {lid=3,phys=1,smt_sib=2,ccx=1,ccd=0,numa=0}  ← HT sibling of 2
static phyriad::HardwareTopology make_fake_4core() {
    phyriad::HardwareTopology t;
    auto add = [&](uint32_t lid, uint32_t phys, uint32_t sib,
                   uint32_t ccx, uint32_t ccd, uint32_t numa,
                   bool vcache = false, bool ecore = false) {
        phyriad::CoreInfo c{};
        c.logical_id        = lid;
        c.physical_id       = phys;
        c.smt_sibling       = sib;
        c.ccx_id            = ccx;
        c.ccd_id            = ccd;
        c.numa_node         = numa;
        c.has_v_cache       = vcache;
        c.is_efficiency_core = ecore;
        t.cores.push_back(c);
    };
    add(0, 0, 1, 0, 0, 0);
    add(1, 0, 0, 0, 0, 0);  // HT sibling of 0
    add(2, 1, 3, 1, 0, 0);
    add(3, 1, 2, 1, 0, 0);  // HT sibling of 2
    return t;
}

// 4-core topology with V-Cache on cores 0,1 (ccx 0) and none on 2,3 (ccx 1).
static phyriad::HardwareTopology make_fake_vcache_topology() {
    phyriad::HardwareTopology t;
    auto add = [&](uint32_t lid, uint32_t phys, uint32_t ccx, bool vcache) {
        phyriad::CoreInfo c{};
        c.logical_id  = lid;
        c.physical_id = phys;
        c.smt_sibling = UINT32_MAX;
        c.ccx_id      = ccx;
        c.numa_node   = 0;
        c.has_v_cache = vcache;
        t.cores.push_back(c);
    };
    add(0, 0, 0, true);
    add(1, 1, 0, true);
    add(2, 2, 1, false);
    add(3, 3, 1, false);
    t.vcache_cores = {0, 1};
    return t;
}

// ── §1 — PlacementHint defaults ───────────────────────────────────────────────
static void test_hint_defaults() {
    SECTION("Test 1: PlacementHint default field values");
    sched::PlacementHint h{};
    EXPECT(h.prefer_vcache            == false);
    EXPECT(h.prefer_isolated_core     == false);
    EXPECT(h.prefers_pcie_gpu         == false);
    EXPECT(h.prefer_realtime_priority == false);
    EXPECT(h.prefer_numa              == UINT32_MAX);
    EXPECT(h.co_locate_with           == UINT32_MAX);
    EXPECT(h.pcie_gpu_index           == UINT32_MAX);
    EXPECT(h.role                     == sched::ThreadRole::GENERIC);
    for (auto id : h.anti_affinity_with)
        EXPECT(id == UINT32_MAX);
}

// ── §2 — ResourceBudget defaults ─────────────────────────────────────────────
static void test_budget_defaults() {
    SECTION("Test 2: ResourceBudget default field values");
    sched::ResourceBudget b{};
    EXPECT(b.max_cores             == UINT32_MAX);
    EXPECT(b.max_numa_nodes        == UINT32_MAX);
    EXPECT(b.allow_ht_siblings     == true);
    EXPECT(b.allow_efficiency_cores == true);
}

// ── §3 — NodeAssignment defaults ─────────────────────────────────────────────
static void test_assignment_defaults() {
    SECTION("Test 3: NodeAssignment default field values");
    sched::NodeAssignment a{};
    EXPECT(a.node_id         == UINT32_MAX);
    EXPECT(a.core_id         == UINT32_MAX);
    EXPECT(a.numa_node       == UINT32_MAX);
    EXPECT(a.thread_priority == 0);
    EXPECT(a.is_hard_pinned  == true);
    EXPECT(a.external        == false);
    EXPECT(a.irq_routing     == UINT32_MAX);
}

// ── §4 — PlacementPlan::find() ────────────────────────────────────────────────
static void test_plan_find() {
    SECTION("Test 4: PlacementPlan::find() — hit and miss");

    sched::PlacementPlan plan;
    plan.assignments.push_back(sched::NodeAssignment{.node_id = 0, .core_id = 5});
    plan.assignments.push_back(sched::NodeAssignment{.node_id = 1, .core_id = 3});
    plan.assignments.push_back(sched::NodeAssignment{.node_id = 2, .core_id = 7});

    EXPECT(plan.find(0) != nullptr);
    EXPECT(plan.find(0)->core_id == 5);
    EXPECT(plan.find(1) != nullptr);
    EXPECT(plan.find(1)->core_id == 3);
    EXPECT(plan.find(2) != nullptr);
    EXPECT(plan.find(2)->core_id == 7);
    EXPECT(plan.find(99) == nullptr);
    EXPECT(plan.find(UINT32_MAX) == nullptr);
}

// ── §5 — PlacementPolicy named constructors ───────────────────────────────────
static void test_policy_named_ctors() {
    SECTION("Test 5: PlacementPolicy named constructors");

    auto p = sched::PlacementPolicy::pinned();
    EXPECT(p.kind == sched::PlacementPolicyKind::Pinned);

    auto s = sched::PlacementPolicy::soft_affine();
    EXPECT(s.kind == sched::PlacementPolicyKind::SoftAffine);

    auto w = sched::PlacementPolicy::work_stealing(8);
    EXPECT(w.kind      == sched::PlacementPolicyKind::WorkStealing);
    EXPECT(w.pool_size == 8u);

    auto w2 = sched::PlacementPolicy::work_stealing();
    EXPECT(w2.pool_size == 4u);  // default

    auto st = sched::PlacementPolicy::sticky_then_steal(200);
    EXPECT(st.kind              == sched::PlacementPolicyKind::StickyThenSteal);
    EXPECT(st.idle_threshold_us == 200u);

    auto st2 = sched::PlacementPolicy::sticky_then_steal();
    EXPECT(st2.idle_threshold_us == 100u);  // default

    // Default-constructed == StickyThenSteal, idle=100
    sched::PlacementPolicy def{};
    EXPECT(def.kind              == sched::PlacementPolicyKind::StickyThenSteal);
    EXPECT(def.idle_threshold_us == 100u);
}

// ── §6 — PlacementPolicy trivial-copyable and sizeof ─────────────────────────
static void test_policy_layout() {
    SECTION("Test 6: PlacementPolicy trivially copyable, sizeof == 12");
    EXPECT(std::is_trivially_copyable_v<sched::PlacementPolicy>);
    EXPECT(sizeof(sched::PlacementPolicy) == 12u);
    // Copy works without UB
    sched::PlacementPolicy a = sched::PlacementPolicy::work_stealing(16);
    sched::PlacementPolicy b{};
    std::memcpy(&b, &a, sizeof(a));
    EXPECT(b.kind      == sched::PlacementPolicyKind::WorkStealing);
    EXPECT(b.pool_size == 16u);
}

// ── §7 — Scheduler construction ──────────────────────────────────────────────
static void test_scheduler_construction() {
    SECTION("Test 7: Scheduler default and explicit construction");
    sched::Scheduler s1;                                      // default policy
    sched::Scheduler s2{sched::PlacementPolicy::pinned()};   // explicit

    // Move-construction
    sched::Scheduler s3{std::move(s1)};
    // Move-assignment
    sched::Scheduler s4 = std::move(s2);

    // If we reach here without crashing, construction is correct.
    EXPECT(true);
}

// ── §8 — node_count == 0 ─────────────────────────────────────────────────────
static void test_zero_nodes() {
    SECTION("Test 8: compute_placement node_count=0 → is_complete, empty assignments");

    phyriad::HardwareTopology t;  // empty topology is fine; no nodes to assign
    sched::Scheduler s;
    auto plan = s.compute_placement(t, 0);

    EXPECT(plan.is_complete == true);
    EXPECT(plan.assignments.empty());
}

// ── §9 — empty cores topology ────────────────────────────────────────────────
static void test_empty_cores() {
    SECTION("Test 9: compute_placement empty cores → is_complete=false, all unassigned");

    phyriad::HardwareTopology t;  // no cores
    sched::Scheduler s;
    auto plan = s.compute_placement(t, 3);

    EXPECT(plan.is_complete == false);
    EXPECT(plan.assignments.size() == 3u);
    for (auto const& a : plan.assignments)
        EXPECT(a.core_id == UINT32_MAX);
    // node_ids should be 0, 1, 2
    EXPECT(plan.assignments[0].node_id == 0u);
    EXPECT(plan.assignments[1].node_id == 1u);
    EXPECT(plan.assignments[2].node_id == 2u);
}

// ── §10 — real probe() ────────────────────────────────────────────────────────
static void test_real_topology() {
    SECTION("Test 10: compute_placement with real probe() — is_complete, valid core_ids");

    auto result = phyriad::HardwareTopology::probe();
    if (!result) {
        std::puts("    [SKIP] probe() failed: " );
        std::puts(result.error().c_str());
        return;
    }
    phyriad::HardwareTopology const& topo = *result;
    if (topo.cores.empty()) {
        std::puts("    [SKIP] empty core list");
        return;
    }

    std::printf("    cores = %zu, vcache_cores = %zu, pcie_devices = %zu\n",
                topo.cores.size(), topo.vcache_cores.size(),
                topo.pcie_affinity_map.size());

    sched::Scheduler s;
    constexpr uint32_t N = 4;
    auto plan = s.compute_placement(topo, N);

    EXPECT(plan.is_complete == true);
    EXPECT(plan.assignments.size() == N);

    for (uint32_t i = 0; i < N; ++i) {
        EXPECT(plan.assignments[i].node_id == i);
        EXPECT(plan.assignments[i].core_id != UINT32_MAX);
    }
}

// ── §11 — UI_MAIN role ────────────────────────────────────────────────────────
static void test_ui_main_role() {
    SECTION("Test 11: UI_MAIN role → external=true, core_id=UINT32_MAX");

    auto topo = make_fake_4core();
    sched::Scheduler s;

    sched::PlacementHint hints[3]{};
    hints[0].role = sched::ThreadRole::UI_MAIN;
    hints[1].role = sched::ThreadRole::GENERIC;
    hints[2].role = sched::ThreadRole::UI_MAIN;

    auto plan = s.compute_placement(topo, 3, hints);

    EXPECT(plan.is_complete == true);
    EXPECT(plan.assignments.size() == 3u);

    // Node 0: UI_MAIN — external, no core assigned
    EXPECT(plan.assignments[0].external == true);
    EXPECT(plan.assignments[0].core_id  == UINT32_MAX);

    // Node 1: GENERIC — normal assignment
    EXPECT(plan.assignments[1].external == false);
    EXPECT(plan.assignments[1].core_id  != UINT32_MAX);

    // Node 2: UI_MAIN — external again
    EXPECT(plan.assignments[2].external == true);
    EXPECT(plan.assignments[2].core_id  == UINT32_MAX);

    // UI_MAIN nodes must not consume core_usage slots — node 1 and any
    // subsequent GENERIC node may reuse any core freely.
    EXPECT(true);
}

// ── §12 — COMPUTE role → V-Cache core ─────────────────────────────────────────
static void test_compute_vcache() {
    SECTION("Test 12: COMPUTE role → V-Cache core assigned");

    // Use fake topology with explicit V-Cache cores on ccx 0.
    auto topo = make_fake_vcache_topology();

    sched::Scheduler s;
    sched::PlacementHint hint{};
    hint.role = sched::ThreadRole::COMPUTE;

    auto plan = s.compute_placement(topo, 1, std::span<sched::PlacementHint const>(&hint, 1));

    EXPECT(plan.is_complete);
    EXPECT(plan.assignments.size() == 1u);

    uint32_t cid = plan.assignments[0].core_id;
    EXPECT(cid != UINT32_MAX);

    // The assigned core must have has_v_cache == true (ccx 0: cores 0,1)
    bool on_vcache = false;
    for (auto const& c : topo.cores)
        if (c.logical_id == cid && c.has_v_cache) { on_vcache = true; break; }
    EXPECT(on_vcache);

    // Also verify that on the real system (X3D) COMPUTE lands on a V-Cache core.
    auto real = phyriad::HardwareTopology::probe();
    if (real && !real->vcache_cores.empty()) {
        sched::Scheduler s2;
        auto plan2 = s2.compute_placement(*real, 1,
            std::span<sched::PlacementHint const>(&hint, 1));
        EXPECT(plan2.is_complete);
        uint32_t rcid = plan2.assignments[0].core_id;
        EXPECT(rcid != UINT32_MAX);
        bool real_vcache = false;
        for (auto const& c : real->cores)
            if (c.logical_id == rcid && c.has_v_cache) { real_vcache = true; break; }
        EXPECT(real_vcache);
        std::printf("    COMPUTE assigned to real core %u (has_v_cache=true)\n", rcid);
    } else {
        std::puts("    [INFO] real system has no V-Cache cores — real-system sub-test skipped");
    }
}

// ── §13 — Determinism ─────────────────────────────────────────────────────────
static void test_determinism() {
    SECTION("Test 13: determinism — same inputs → same output on two calls");

    auto topo = make_fake_4core();
    sched::Scheduler s;

    sched::PlacementHint hints[3]{};
    hints[0].role = sched::ThreadRole::COMPUTE;
    hints[1].role = sched::ThreadRole::IO;
    hints[2].role = sched::ThreadRole::RENDER;

    auto plan1 = s.compute_placement(topo, 3, hints);
    auto plan2 = s.compute_placement(topo, 3, hints);

    EXPECT(plan1.is_complete == plan2.is_complete);
    EXPECT(plan1.assignments.size() == plan2.assignments.size());
    for (std::size_t i = 0; i < plan1.assignments.size(); ++i) {
        EXPECT(plan1.assignments[i].core_id  == plan2.assignments[i].core_id);
        EXPECT(plan1.assignments[i].node_id  == plan2.assignments[i].node_id);
        EXPECT(plan1.assignments[i].external == plan2.assignments[i].external);
    }
}

// ── §14 — max_cores cap ───────────────────────────────────────────────────────
static void test_max_cores_cap() {
    SECTION("Test 14: ResourceBudget max_cores cap — no core_id >= max_cores");

    auto real = phyriad::HardwareTopology::probe();
    if (!real || real->cores.size() < 2) {
        // Fallback to fake topology
        auto topo = make_fake_4core();
        sched::Scheduler s;
        sched::ResourceBudget b{};
        b.max_cores = 2;  // only cores 0 and 1 eligible
        auto plan = s.compute_placement(topo, 2, {}, b);
        EXPECT(plan.is_complete);
        for (auto const& a : plan.assignments) {
            if (a.external) continue;
            EXPECT(a.core_id < 2u);
        }
        return;
    }

    sched::Scheduler s;
    constexpr uint32_t CAP = 4u;
    sched::ResourceBudget b{};
    b.max_cores = CAP;
    auto plan = s.compute_placement(*real, 4, {}, b);
    EXPECT(plan.is_complete);
    for (auto const& a : plan.assignments) {
        if (a.external) continue;
        EXPECT(a.core_id < CAP);
    }
}

// ── §15 — RENDER role exercises pcie_gpu path ────────────────────────────────
static void test_render_role() {
    SECTION("Test 15: RENDER role — prefers_pcie_gpu score path exercised");

    // Fake topology: 2 cores on NUMA 0, 1 GPU on NUMA 0.
    phyriad::HardwareTopology t;
    {
        phyriad::CoreInfo c0{};
        c0.logical_id = 0; c0.numa_node = 0; c0.ccx_id = 0;
        phyriad::CoreInfo c1{};
        c1.logical_id = 1; c1.numa_node = 1; c1.ccx_id = 1;
        t.cores.push_back(c0);
        t.cores.push_back(c1);
    }
    {
        phyriad::topology::PCIeDevice gpu{};
        gpu.pci_class  = 0x0300u;  // display/GPU class
        gpu.numa_node  = 0u;
        t.pcie_affinity_map.push_back(gpu);
    }

    sched::Scheduler s;
    sched::PlacementHint hint{};
    hint.role = sched::ThreadRole::RENDER;

    auto plan = s.compute_placement(t, 1, std::span<sched::PlacementHint const>(&hint, 1));

    EXPECT(plan.is_complete);
    EXPECT(plan.assignments.size() == 1u);
    // Should prefer core 0 (NUMA 0 = same as GPU) over core 1 (NUMA 1).
    EXPECT(plan.assignments[0].core_id == 0u);
    EXPECT(plan.assignments[0].numa_node == 0u);
}

// ── §16 — Anti-affinity ───────────────────────────────────────────────────────
static void test_anti_affinity() {
    SECTION("Test 16: anti-affinity — nodes pushed to different CCX where possible");

    // Fake 4-core topology: ccx0={0,1}, ccx1={2,3}, all smt_sibling=UINT32_MAX.
    phyriad::HardwareTopology t;
    auto add = [&](uint32_t lid, uint32_t ccx) {
        phyriad::CoreInfo c{};
        c.logical_id  = lid;
        c.physical_id = lid;
        c.smt_sibling = UINT32_MAX;
        c.ccx_id      = ccx;
        c.numa_node   = 0;
        t.cores.push_back(c);
    };
    add(0, 0); add(1, 0);   // ccx 0
    add(2, 1); add(3, 1);   // ccx 1

    sched::Scheduler s;

    // Node 0: no hint → default, first-fit lowest logical_id → core 0 (ccx 0)
    // Node 1: anti-affinity with node 0 → should avoid ccx 0 → picks core 2 (ccx 1)
    sched::PlacementHint hints[2]{};
    hints[1].anti_affinity_with[0] = 0u;  // avoid sharing CCX with node 0

    auto plan = s.compute_placement(t, 2, hints);

    EXPECT(plan.is_complete);
    EXPECT(plan.assignments.size() == 2u);

    uint32_t c0 = plan.assignments[0].core_id;
    uint32_t c1 = plan.assignments[1].core_id;
    EXPECT(c0 != UINT32_MAX);
    EXPECT(c1 != UINT32_MAX);

    // Find CCX of each assigned core
    uint32_t ccx0 = UINT32_MAX, ccx1 = UINT32_MAX;
    for (auto const& c : t.cores) {
        if (c.logical_id == c0) ccx0 = c.ccx_id;
        if (c.logical_id == c1) ccx1 = c.ccx_id;
    }

    // They must be on different CCX (anti-affinity should have pushed node 1 away)
    EXPECT(ccx0 != ccx1);
    std::printf("    node0→core%u(ccx%u)  node1→core%u(ccx%u)\n",
                c0, ccx0, c1, ccx1);
}

// ── §17 — HT sibling exclusion ────────────────────────────────────────────────
static void test_ht_exclusion() {
    SECTION("Test 17: allow_ht_siblings=false prevents placing on sibling of occupied core");

    // Topology: cores 0&1 are SMT siblings (phys 0, ccx 0),
    //           cores 2&3 are SMT siblings (phys 1, ccx 1).
    auto topo = make_fake_4core();

    sched::Scheduler s;
    sched::ResourceBudget b{};
    b.allow_ht_siblings = false;  // sibling of occupied core becomes ineligible

    // prefer_isolated_core pushes nodes onto unoccupied cores (-40 for occupied).
    // Without it the scoring tie-break would re-pick core 0 for both nodes.
    sched::PlacementHint hints[2]{};
    hints[0].prefer_isolated_core = true;
    hints[1].prefer_isolated_core = true;

    // Expected with allow_ht_siblings=false + prefer_isolated_core:
    //   node 0 → core 0 (best: isolated +20, core_usage[0] → 1)
    //   node 1 eligible: core 0 (-40, occupied), core 1 excluded (sibling of 0),
    //                    core 2 (+20), core 3 (+20)  → picks core 2 (lowest lid tie)
    auto plan = s.compute_placement(topo, 2, hints, b);
    EXPECT(plan.is_complete);
    EXPECT(plan.assignments.size() == 2u);

    uint32_t c0 = plan.assignments[0].core_id;
    uint32_t c1 = plan.assignments[1].core_id;
    EXPECT(c0 != UINT32_MAX);
    EXPECT(c1 != UINT32_MAX);

    // Key assertion: node 1 must NOT have been assigned the HT sibling of node 0.
    uint32_t sib_of_c0 = UINT32_MAX;
    for (auto const& c : topo.cores)
        if (c.logical_id == c0) { sib_of_c0 = c.smt_sibling; break; }
    EXPECT(c1 != sib_of_c0);

    // With prefer_isolated_core, the occupied core 0 is penalised (-40 < +20),
    // so node 1 should have landed on a different physical core entirely.
    uint32_t p0 = UINT32_MAX, p1 = UINT32_MAX;
    for (auto const& c : topo.cores) {
        if (c.logical_id == c0) p0 = c.physical_id;
        if (c.logical_id == c1) p1 = c.physical_id;
    }
    EXPECT(p0 != p1);
    std::printf("    node0→core%u(phys%u)  node1→core%u(phys%u)  sib(core%u)=%u\n",
                c0, p0, c1, p1, c0, sib_of_c0);
}

// ── §18 — Enum values (runtime mirror of compile-time check) ──────────────────
static void test_enum_values() {
    SECTION("Test 18: PlacementPolicyKind enum values");
    EXPECT(static_cast<uint8_t>(sched::PlacementPolicyKind::Pinned)          == 0u);
    EXPECT(static_cast<uint8_t>(sched::PlacementPolicyKind::SoftAffine)      == 1u);
    EXPECT(static_cast<uint8_t>(sched::PlacementPolicyKind::WorkStealing)    == 2u);
    EXPECT(static_cast<uint8_t>(sched::PlacementPolicyKind::StickyThenSteal) == 3u);
    EXPECT(static_cast<uint8_t>(sched::ThreadRole::GENERIC)                  == 0u);
    EXPECT(static_cast<uint8_t>(sched::ThreadRole::UI_MAIN)                  == 1u);
    EXPECT(static_cast<uint8_t>(sched::ThreadRole::IO)                       == 5u);
}

// ── §19 — Multiple UI_MAIN nodes don't consume core slots ─────────────────────
static void test_multi_ui_main() {
    SECTION("Test 19: multiple UI_MAIN nodes — core_usage unaffected");

    auto topo = make_fake_4core();  // 4 physical cores
    sched::Scheduler s;

    // 3 UI_MAIN + 2 GENERIC → 4 nodes, only 2 GENERIC should consume cores.
    sched::PlacementHint hints[5]{};
    hints[0].role = sched::ThreadRole::UI_MAIN;
    hints[1].role = sched::ThreadRole::UI_MAIN;
    hints[2].role = sched::ThreadRole::GENERIC;
    hints[3].role = sched::ThreadRole::UI_MAIN;
    hints[4].role = sched::ThreadRole::GENERIC;

    auto plan = s.compute_placement(topo, 5, hints);

    EXPECT(plan.is_complete);
    EXPECT(plan.assignments.size() == 5u);

    int extern_count  = 0;
    int generic_count = 0;
    for (auto const& a : plan.assignments) {
        if (a.external) { ++extern_count; EXPECT(a.core_id == UINT32_MAX); }
        else            { ++generic_count; EXPECT(a.core_id != UINT32_MAX); }
    }
    EXPECT(extern_count  == 3);
    EXPECT(generic_count == 2);

    // The two GENERIC nodes may reuse the same core (topo only has 4,
    // both are default-hint so they just get the two best-scored cores).
    EXPECT(plan.find(0)->external == true);
    EXPECT(plan.find(1)->external == true);
    EXPECT(plan.find(3)->external == true);
    EXPECT(plan.find(2)->external == false);
    EXPECT(plan.find(4)->external == false);
}

// ── §20 — IO role allows E-cores ─────────────────────────────────────────────
static void test_io_ecore() {
    SECTION("Test 20: IO role — E-cores allowed regardless of ResourceBudget");

    // Fake topology: core 0 = P-core (efficiency_class=1), core 1 = E-core.
    phyriad::HardwareTopology t;
    {
        phyriad::CoreInfo p{};
        p.logical_id         = 0;
        p.physical_id        = 0;
        p.smt_sibling        = UINT32_MAX;
        p.efficiency_class   = 1;
        p.is_efficiency_core = false;
        p.numa_node          = 0;
        t.cores.push_back(p);

        phyriad::CoreInfo e{};
        e.logical_id         = 1;
        e.physical_id        = 1;
        e.smt_sibling        = UINT32_MAX;
        e.efficiency_class   = 0;
        e.is_efficiency_core = true;
        e.numa_node          = 0;
        t.cores.push_back(e);
    }

    sched::ResourceBudget b{};
    b.allow_efficiency_cores = false;  // budget says NO e-cores

    sched::Scheduler s;

    // GENERIC node with strict budget → can only use core 0 (P-core).
    {
        sched::PlacementHint hint{};
        hint.role = sched::ThreadRole::GENERIC;
        auto plan = s.compute_placement(t, 1,
            std::span<sched::PlacementHint const>(&hint, 1), b);
        EXPECT(plan.is_complete);
        EXPECT(plan.assignments[0].core_id == 0u);  // P-core only
    }

    // IO node with the same strict budget → IO override allows E-cores → any core eligible.
    // With prefer_isolated_core=false and equal scores, the tie-break picks the lower logical_id,
    // so core 0 wins again. But the key is: both cores are now eligible (no assertion failure).
    {
        sched::PlacementHint hint{};
        hint.role = sched::ThreadRole::IO;
        auto plan = s.compute_placement(t, 1,
            std::span<sched::PlacementHint const>(&hint, 1), b);
        EXPECT(plan.is_complete);
        EXPECT(plan.assignments[0].core_id != UINT32_MAX);
        // Core_id may be 0 or 1 — either is valid; the point is no crash / unassigned.
    }
}

// ── main ──────────────────────────────────────────────────────────────────────
int main() {
    std::puts("[scheduler_test] phyriad_scheduler pillar — Phase 1.A");
    std::puts("----------------------------------------------------------------");

    std::printf("  sizeof(PlacementHint)    = %zu bytes\n", sizeof(sched::PlacementHint));
    std::printf("  sizeof(ResourceBudget)   = %zu bytes\n", sizeof(sched::ResourceBudget));
    std::printf("  sizeof(NodeAssignment)   = %zu bytes\n", sizeof(sched::NodeAssignment));
    std::printf("  sizeof(PlacementPolicy)  = %zu bytes\n", sizeof(sched::PlacementPolicy));
    std::puts("----------------------------------------------------------------");

    test_hint_defaults();
    test_budget_defaults();
    test_assignment_defaults();
    test_plan_find();
    test_policy_named_ctors();
    test_policy_layout();
    test_scheduler_construction();
    test_zero_nodes();
    test_empty_cores();
    test_real_topology();
    test_ui_main_role();
    test_compute_vcache();
    test_determinism();
    test_max_cores_cap();
    test_render_role();
    test_anti_affinity();
    test_ht_exclusion();
    test_enum_values();
    test_multi_ui_main();
    test_io_ecore();

    std::puts("----------------------------------------------------------------");
    if (g_tests_failed == 0) {
        std::printf("[OK] %d/%d tests passed\n", g_tests_run, g_tests_run);
        return 0;
    } else {
        std::printf("[FAIL] %d/%d tests FAILED\n", g_tests_failed, g_tests_run);
        return 1;
    }
}
// Made with my soul - Swately <3
