// framework/runtime/tests/runtime_test.cpp
// GraphRuntime pillar test — Phase 1.B
//
// Sections:
//   §1   PerformanceProfile defaults
//   §2   make_auto_profile() — node count matches
//   §3   apply_profile_hints() — V-Cache core tightens budget
//   §4   GraphRuntime::create() — invalid node name → error
//   §5   GraphRuntime::create() — valid 2-node graph
//   §6   GraphRuntime node_count / wire_count
//   §7   GraphRuntime start() → stop() → shutdown()
//   §8   GraphRuntime::run() — 1 tick with CS→CT pipeline
//   §9   GraphRuntime::run() — back-pressure (no ring → stops cleanly)
//   §10  GraphRuntime move semantics
//   §11  FR-7: pause_node / resume_node API
//   §12  FR-7: pause_node() while run() is active
//   §13  FR-7: restart_node() — soft stop+start while paused
//   §14  FR-7: hot-restart ops on default-constructed (empty) runtime
//

#include <phyriad/runtime/RuntimeAll.hpp>
#include <phyriad/api/GraphDSL.hpp>
#include <phyriad/api/NodeRegistry.hpp>
#include <phyriad/api/WireRegistry.hpp>
#include <phyriad/api/NodeHandle.hpp>
#include <phyriad/node/canonical/CanonicalSource.hpp>
#include <phyriad/node/canonical/CanonicalTransform.hpp>
#include <phyriad/schema/SchemaHash.hpp>
#include <phyriad/topology/HardwareTopology.hpp>
#include <cstdint>
#include <cstdio>
#include <atomic>
#include <thread>

namespace rt  = phyriad::runtime;
namespace api = phyriad::api;
namespace sch = phyriad::schema;
// HardwareTopology lives in namespace phyriad directly (not phyriad::topology)

using CS = phyriad::node::canonical::CanonicalSource;
using CT = phyriad::node::canonical::CanonicalTransform;

// ── Minimal test harness ──────────────────────────────────────────────────────
static int g_pass{0};
static int g_fail{0};

#define SECTION(msg) std::printf("  § %s\n", (msg))
#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (cond) { ++g_pass; }                                                \
        else {                                                                 \
            ++g_fail;                                                          \
            std::printf("  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
        }                                                                      \
    } while (false)

// ── Helper: build a 2-node CS→CT BuiltGraph ──────────────────────────────────
static api::BuiltGraph make_two_node_graph() noexcept {
    auto r = api::build_graph()
        .node<CS>("source")
        .node<CT>("sink")
        .wire("source").to("sink")
        .build();
    if (!r) std::abort();
    return std::move(*r);
}

// ── Helper: populate a NodeRegistry with CS and CT ───────────────────────────
static void populate_registries(api::NodeRegistry& nr, api::WireRegistry& wr) {
    nr.register_node<CS>("source");
    nr.register_node<CT>("sink");
    wr.register_type<sch::SampleTick>();
}

// ── §1 PerformanceProfile defaults ───────────────────────────────────────────
static void test_profile_defaults() {
    SECTION("Test 1: PerformanceProfile defaults");

    rt::PerformanceProfile p;
    EXPECT(p.node_profiles.empty());
    EXPECT(p.ring_profiles.empty());
    EXPECT(p.yield_sleep_ns == 500u);
    EXPECT(p.use_busy_wait_loop == true);
    EXPECT(p.allow_timer_coalescing == false);

    rt::NodeTickProfile np;
    EXPECT(np.tick_budget_ns == 200'000u);
    EXPECT(np.spin_count     == 128u);
    EXPECT(np.yield_after_spins == 4u);
    EXPECT(np.latency_histogram == false);

    rt::RingProfile rp;
    EXPECT(rp.capacity == 64u);
    EXPECT(rp.backpressure_watermark == 48u);
    EXPECT(rp.strict_backpressure == true);
}

// ── §2 make_auto_profile sizes correctly ─────────────────────────────────────
static void test_auto_profile_sizing() {
    SECTION("Test 2: make_auto_profile() — node/wire count matches");

    auto topo = phyriad::HardwareTopology::probe().value_or(phyriad::HardwareTopology{});
    std::vector<phyriad::scheduler::PlacementHint> hints(3);

    auto p = rt::make_auto_profile(topo, hints, 3u, 5u);
    EXPECT(p.node_profiles.size() == 3u);
    EXPECT(p.ring_profiles.size() == 5u);

    // Ring defaults.
    for (auto const& rp : p.ring_profiles) {
        EXPECT(rp.capacity == 64u);
        EXPECT(rp.backpressure_watermark == 48u);
    }
}

// ── §3 apply_profile_hints — V-Cache core tightens budget ────────────────────
static void test_apply_profile_hints() {
    SECTION("Test 3: apply_profile_hints() — V-Cache core tightens tick budget");

    auto topo = phyriad::HardwareTopology::probe().value_or(phyriad::HardwareTopology{});

    // If the system has no V-Cache cores, skip the V-Cache assertion.
    const bool has_vcache = !topo.vcache_cores.empty();

    std::vector<phyriad::scheduler::PlacementHint> hints(2);
    auto p = rt::make_auto_profile(topo, hints, 2u, 1u);

    // Initially default budgets.
    for (auto const& np : p.node_profiles)
        EXPECT(np.tick_budget_ns == 200'000u);

    // Find a core that is NOT in vcache_cores (for node 1).
    uint32_t non_vcache_id = UINT32_MAX;
    for (auto const& c : topo.cores) {
        bool is_vc = false;
        for (uint32_t v : topo.vcache_cores)
            if (c.logical_id == v) { is_vc = true; break; }
        if (!is_vc) { non_vcache_id = c.logical_id; break; }
    }
    const bool has_non_vcache = (non_vcache_id != UINT32_MAX);

    // Assign node 0 to a V-Cache core and node 1 to a non-V-Cache core.
    std::vector<uint32_t> assigned(2, 0u);
    if (has_vcache)
        assigned[0] = topo.vcache_cores[0];
    if (has_non_vcache)
        assigned[1] = non_vcache_id;

    rt::apply_profile_hints(p, hints, assigned, topo);

    if (has_vcache) {
        // Node 0 on V-Cache → budget tightened to ≤100 µs.
        EXPECT(p.node_profiles[0].tick_budget_ns <= 100'000u);
        EXPECT(p.node_profiles[0].spin_count >= 512u);
    } else {
        EXPECT(p.node_profiles[0].tick_budget_ns == 200'000u);
    }

    if (has_vcache && has_non_vcache) {
        // Node 1 on non-V-Cache core → unchanged default.
        EXPECT(p.node_profiles[1].tick_budget_ns == 200'000u);
    }
}

// ── §4 GraphRuntime::create() — unknown node name → error ────────────────────
static void test_create_unknown_node() {
    SECTION("Test 4: GraphRuntime::create() — unknown node name → error");

    auto graph = make_two_node_graph();

    api::NodeRegistry nr;
    api::WireRegistry wr;
    // Do NOT register "source" or "sink" — instantiate() will fail.
    wr.register_type<sch::SampleTick>();

    rt::PerformanceProfile profile;
    profile.node_profiles.resize(2);
    profile.ring_profiles.resize(1);

    auto result = rt::GraphRuntime::create(graph, nr, wr, profile);
    EXPECT(!result.has_value());
    if (!result)
        EXPECT(result.error().code == api::ConfigErrorCode::UnknownNodeName);
}

// ── §5 GraphRuntime::create() — valid 2-node graph ───────────────────────────
static void test_create_valid() {
    SECTION("Test 5: GraphRuntime::create() — valid 2-node graph");

    auto graph = make_two_node_graph();

    api::NodeRegistry nr;
    api::WireRegistry wr;
    populate_registries(nr, wr);

    rt::PerformanceProfile profile;
    profile.node_profiles.resize(2);
    profile.ring_profiles.resize(1);

    auto result = rt::GraphRuntime::create(graph, nr, wr, profile);
    EXPECT(result.has_value());
}

// ── §6 node_count / wire_count ────────────────────────────────────────────────
static void test_counts() {
    SECTION("Test 6: GraphRuntime node_count / wire_count");

    auto graph = make_two_node_graph();
    api::NodeRegistry nr;
    api::WireRegistry wr;
    populate_registries(nr, wr);

    rt::PerformanceProfile profile;
    profile.node_profiles.resize(2);
    profile.ring_profiles.resize(1);

    auto rt = rt::GraphRuntime::create(graph, nr, wr, profile);
    EXPECT(rt.has_value());
    if (rt) {
        EXPECT(rt->node_count() == 2u);
        EXPECT(rt->wire_count() == 1u);
    }
}

// ── §7 start → stop → shutdown ────────────────────────────────────────────────
static void test_start_stop_shutdown() {
    SECTION("Test 7: GraphRuntime start() → stop() → shutdown()");

    auto graph = make_two_node_graph();
    api::NodeRegistry nr;
    api::WireRegistry wr;
    populate_registries(nr, wr);

    rt::PerformanceProfile profile;
    profile.node_profiles.resize(2);
    profile.ring_profiles.resize(1);

    auto result = rt::GraphRuntime::create(graph, nr, wr, profile);
    EXPECT(result.has_value());
    if (!result) return;

    auto& runtime = *result;
    EXPECT(!runtime.running());

    auto s = runtime.start();
    EXPECT(s.has_value());
    EXPECT(runtime.running());

    runtime.stop();
    auto q = runtime.shutdown();
    EXPECT(q.has_value());
    EXPECT(!runtime.running());
}

// ── §8 run() — one-tick pipeline ──────────────────────────────────────────────
static void test_run_one_tick_pipeline() {
    SECTION("Test 8: GraphRuntime::run() — CS→CT pipeline, stop after 1 ms");

    auto graph = make_two_node_graph();
    api::NodeRegistry nr;
    api::WireRegistry wr;
    populate_registries(nr, wr);

    rt::PerformanceProfile profile;
    profile.node_profiles.resize(2);
    profile.ring_profiles.resize(1);

    auto result = rt::GraphRuntime::create(graph, nr, wr, profile);
    EXPECT(result.has_value());
    if (!result) return;

    auto& runtime = *result;
    (void)runtime.start();
    EXPECT(runtime.running());

    // Stop after ~1 ms from a background thread.
    std::thread stopper([&runtime]() noexcept {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        runtime.stop();
    });

    runtime.run();   // blocks until stop() is called
    stopper.join();

    EXPECT(!runtime.running());
    (void)runtime.shutdown();
}

// ── §9 back-pressure: tick() returns RingEmpty until data arrives ─────────────
static void test_run_back_pressure() {
    SECTION("Test 9: GraphRuntime back-pressure — run() exits cleanly on stop");

    // Build a CS→CT graph but don't seed any ticks into CS.
    // CS::tick() calls emit() which publishes to the ring.  CT::tick() calls
    // receive() which may get RingEmpty until CS produces.  Both are normal.
    auto graph = make_two_node_graph();
    api::NodeRegistry nr;
    api::WireRegistry wr;
    populate_registries(nr, wr);

    rt::PerformanceProfile profile;
    profile.node_profiles.resize(2);
    profile.ring_profiles.resize(1);

    auto result = rt::GraphRuntime::create(graph, nr, wr, profile);
    EXPECT(result.has_value());
    if (!result) return;

    auto& runtime = *result;
    (void)runtime.start();

    // Stop after 500 µs — should exit cleanly regardless of tick results.
    std::thread stopper([&runtime]() noexcept {
        std::this_thread::sleep_for(std::chrono::microseconds(500));
        runtime.stop();
    });
    runtime.run();
    stopper.join();

    EXPECT(!runtime.running());
    (void)runtime.shutdown();
}

// ── §11 FR-7: pause_node / resume_node ───────────────────────────────────────
static void test_pause_resume() {
    SECTION("Test 11: FR-7 pause_node() / resume_node() API");

    auto graph = make_two_node_graph();
    api::NodeRegistry nr;
    api::WireRegistry wr;
    populate_registries(nr, wr);

    rt::PerformanceProfile profile;
    profile.node_profiles.resize(2);
    profile.ring_profiles.resize(1);

    auto result = rt::GraphRuntime::create(graph, nr, wr, profile);
    EXPECT(result.has_value());
    if (!result) return;

    auto& runtime = *result;
    (void)runtime.start();

    // Initially not paused.
    EXPECT(!runtime.is_node_paused(0u));
    EXPECT(!runtime.is_node_paused(1u));

    // Pause node 0.
    auto pr = runtime.pause_node(0u);
    EXPECT(pr.has_value());
    EXPECT(runtime.is_node_paused(0u));
    EXPECT(!runtime.is_node_paused(1u));   // node 1 unaffected

    // Resume node 0.
    auto rr = runtime.resume_node(0u);
    EXPECT(rr.has_value());
    EXPECT(!runtime.is_node_paused(0u));

    // Out-of-range id → InvalidNodeId.
    auto bad_p = runtime.pause_node(99u);
    EXPECT(!bad_p.has_value());
    if (!bad_p)
        EXPECT(bad_p.error().code == phyriad::ErrorCode::InvalidNodeId);

    auto bad_r = runtime.resume_node(99u);
    EXPECT(!bad_r.has_value());
    if (!bad_r)
        EXPECT(bad_r.error().code == phyriad::ErrorCode::InvalidNodeId);

    // is_node_paused for out-of-range returns false.
    EXPECT(!runtime.is_node_paused(99u));

    (void)runtime.shutdown();
}

// ── §12 FR-7: pause_node during run() ────────────────────────────────────────
static void test_pause_during_run() {
    SECTION("Test 12: FR-7 pause_node() / resume_node() while run() is active");

    auto graph = make_two_node_graph();
    api::NodeRegistry nr;
    api::WireRegistry wr;
    populate_registries(nr, wr);

    rt::PerformanceProfile profile;
    profile.node_profiles.resize(2);
    profile.ring_profiles.resize(1);

    auto result = rt::GraphRuntime::create(graph, nr, wr, profile);
    EXPECT(result.has_value());
    if (!result) return;

    auto& runtime = *result;
    (void)runtime.start();

    // Pause node 0 and then stop from a background thread.
    std::thread worker([&runtime]() noexcept {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        (void)runtime.pause_node(0u);
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        (void)runtime.resume_node(0u);
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        runtime.stop();
    });

    runtime.run();    // blocks until stop()
    worker.join();

    EXPECT(!runtime.is_node_paused(0u));   // resumed before stop
    EXPECT(!runtime.running());
    (void)runtime.shutdown();
}

// ── §13 FR-7: restart_node ────────────────────────────────────────────────────
static void test_restart_node() {
    SECTION("Test 13: FR-7 restart_node() — stop + start while paused");

    auto graph = make_two_node_graph();
    api::NodeRegistry nr;
    api::WireRegistry wr;
    populate_registries(nr, wr);

    rt::PerformanceProfile profile;
    profile.node_profiles.resize(2);
    profile.ring_profiles.resize(1);

    auto result = rt::GraphRuntime::create(graph, nr, wr, profile);
    EXPECT(result.has_value());
    if (!result) return;

    auto& runtime = *result;
    (void)runtime.start();

    // restart_node completes and leaves the node resumed.
    auto r0 = runtime.restart_node(0u);
    EXPECT(r0.has_value());
    EXPECT(!runtime.is_node_paused(0u));   // resumed after restart

    auto r1 = runtime.restart_node(1u);
    EXPECT(r1.has_value());
    EXPECT(!runtime.is_node_paused(1u));

    // Out-of-range id → InvalidNodeId.
    auto bad = runtime.restart_node(99u);
    EXPECT(!bad.has_value());
    if (!bad)
        EXPECT(bad.error().code == phyriad::ErrorCode::InvalidNodeId);

    (void)runtime.shutdown();
}

// ── §14 FR-7: node hot-restart ops on empty / default runtime ─────────────────
static void test_hot_restart_empty_runtime() {
    SECTION("Test 14: FR-7 hot-restart ops on default-constructed runtime");

    rt::GraphRuntime empty;

    // All ops on empty runtime return InvalidNodeId (impl_ is null).
    auto bp = empty.pause_node(0u);
    EXPECT(!bp.has_value());
    if (!bp)
        EXPECT(bp.error().code == phyriad::ErrorCode::InvalidNodeId);

    auto br = empty.resume_node(0u);
    EXPECT(!br.has_value());
    if (!br)
        EXPECT(br.error().code == phyriad::ErrorCode::InvalidNodeId);

    auto brs = empty.restart_node(0u);
    EXPECT(!brs.has_value());
    if (!brs)
        EXPECT(brs.error().code == phyriad::ErrorCode::InvalidNodeId);

    EXPECT(!empty.is_node_paused(0u));
}

// ── §10 move semantics ────────────────────────────────────────────────────────
static void test_move_semantics() {
    SECTION("Test 10: GraphRuntime move semantics");

    auto graph = make_two_node_graph();
    api::NodeRegistry nr;
    api::WireRegistry wr;
    populate_registries(nr, wr);

    rt::PerformanceProfile profile;
    profile.node_profiles.resize(2);
    profile.ring_profiles.resize(1);

    auto result = rt::GraphRuntime::create(graph, nr, wr, profile);
    EXPECT(result.has_value());
    if (!result) return;

    rt::GraphRuntime a = std::move(*result);
    EXPECT(a.node_count() == 2u);

    rt::GraphRuntime b = std::move(a);
    EXPECT(b.node_count() == 2u);
    EXPECT(a.node_count() == 0u);  // moved-from is empty

    // b destructor cleanly destroys everything.
}

// ── main ──────────────────────────────────────────────────────────────────────
int main() {
    std::printf("[runtime_test] phyriad_runtime pillar — Phase 1.B\n");
    std::printf("----------------------------------------------------------------\n");

    test_profile_defaults();
    test_auto_profile_sizing();
    test_apply_profile_hints();
    test_create_unknown_node();
    test_create_valid();
    test_counts();
    test_start_stop_shutdown();
    test_run_one_tick_pipeline();
    test_run_back_pressure();
    test_pause_resume();
    test_pause_during_run();
    test_restart_node();
    test_hot_restart_empty_runtime();
    test_move_semantics();

    std::printf("----------------------------------------------------------------\n");
    const int total = g_pass + g_fail;
    if (g_fail == 0)
        std::printf("[OK] %d/%d tests passed\n", g_pass, total);
    else
        std::printf("[FAIL] %d/%d tests FAILED\n", g_fail, total);

    return g_fail ? 1 : 0;
}
// Made with my soul - Swately <3
