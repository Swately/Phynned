// framework/graph/tests/graph_test.cpp
// Test suite for phyriad_graph pillar.
//
// Tests:
//   1.  Graph<G> concept — StaticGraph satisfies it; bare struct doesn't
//   2.  DynamicGraph — concept verification (compile-time assert replicated here)
//   3.  StaticGraph schema_hash — different node types → different hashes
//   4.  StaticGraph schema_hash — same types → same hash (deterministic)
//   5.  GraphBuilder wire_ring — connects Source→Transform via Ring
//   6.  GraphBuilder build() — descriptor has correct node/wire counts
//   7.  GraphValidator validate_type_compat — OK on valid wires, fail on zero hash
//   8.  GraphValidator validate_acyclicity — OK on DAG, CycleDetected on cycle
//   9.  GraphValidator validate_reachability — UnreachableNode detected
//  10.  GraphValidator validate_all — chains all three checks
//  11.  ValidationResult Code enum values
//  12.  ConfigError default values and make() factory
//  13.  ConfigErrorCode enum values
//  14.  WirePolicy enum values
//  15.  DslGraphBuilder — build_graph() → node → wire → build() OK
//  16.  DslGraphBuilder — duplicate node name → DuplicateNodeName error
//  17.  DslGraphBuilder — unknown wire source → UnknownNodeName error
//  18.  DslGraphBuilder — type mismatch → TypeMismatch error
//  19.  DslGraphBuilder — empty graph → EmptyGraph error
//  20.  DslGraphBuilder — validate() → acyclicity OK on linear graph
//  21.  NodeHandle make<N> — valid handle, tick/start/stop
//  22.  NodeHandle wrap<N> — non-owning, destroy is no-op
//  23.  NodeHandle alignment — alignof == hal::kDestructivePad
//  24.  WireHandle make<T> — valid handle, capacity rounded to pow2
//  25.  WireHandle connect_outlet / connect_inlet round-trip
//  26.  WireRegistry register_type + create_wire — success and miss
//  27.  NodeRegistry register_node + instantiate — success and miss
//  28.  placement:: factory functions — correct fields
//  29.  DslGraphBuilder hints — placement hint propagated to BuiltGraph
//  30.  BuiltGraph shm_size() — correct byte count
//  31.  BuiltGraph write_shm() → DynamicGraph attach() round-trip
//

#include <phyriad/graph/GraphAll.hpp>
#include <phyriad/node/canonical/CanonicalSource.hpp>
#include <phyriad/node/canonical/CanonicalTransform.hpp>
#include <phyriad/schema/Schema.hpp>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <type_traits>
#include <vector>

namespace gra  = phyriad::graph;
namespace api  = phyriad::api;
namespace nd   = phyriad::node;
namespace sch  = phyriad::schema;

using CS = nd::canonical::CanonicalSource;
using CT = nd::canonical::CanonicalTransform;

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
static_assert(gra::Graph<gra::DynamicGraph>,
    "DynamicGraph must satisfy Graph<DynamicGraph>");
static_assert(gra::Graph<gra::StaticGraph<CS, CT>>,
    "StaticGraph<CS,CT> must satisfy Graph");
static_assert(!gra::Graph<int>,
    "int must NOT satisfy Graph");

// §1 — Graph<G> concept
static void test_graph_concept() {
    SECTION("Test 1: Graph<G> concept — StaticGraph yes, plain struct no");
    using SG2 = gra::StaticGraph<CS, CT>;
    EXPECT(gra::Graph<SG2>);
    EXPECT(gra::Graph<gra::DynamicGraph>);
    EXPECT(!gra::Graph<int>);
    EXPECT(!gra::Graph<sch::Hash128>);
}

// §2 — DynamicGraph concept
static void test_dynamic_graph_concept() {
    SECTION("Test 2: DynamicGraph default-constructed is empty / invalid");
    gra::DynamicGraph g;
    EXPECT(g.node_count() == 0u);
    EXPECT(g.wire_count() == 0u);
    EXPECT(g.node_descriptors() == nullptr);
    EXPECT(g.wire_descriptors() == nullptr);
}

// §3 — StaticGraph schema_hash different types
static void test_schema_hash_differs() {
    SECTION("Test 3: StaticGraph schema_hash — different node types → different hashes");
    constexpr auto h1 = gra::StaticGraph<CS>::schema_hash();
    constexpr auto h2 = gra::StaticGraph<CT>::schema_hash();
    constexpr auto h3 = gra::StaticGraph<CS, CT>::schema_hash();
    EXPECT(h1 != h2);
    EXPECT(h1 != h3);
    EXPECT(h2 != h3);
}

// §4 — StaticGraph schema_hash deterministic
static void test_schema_hash_deterministic() {
    SECTION("Test 4: StaticGraph schema_hash — same types → same hash");
    constexpr auto h1 = gra::StaticGraph<CS, CT>::schema_hash();
    constexpr auto h2 = gra::StaticGraph<CS, CT>::schema_hash();
    EXPECT(h1 == h2);
    EXPECT(h1.low  == h2.low);
    EXPECT(h1.high == h2.high);
}

// §5 — GraphBuilder wire_ring
static void test_graph_builder_wire_ring() {
    SECTION("Test 5: GraphBuilder wire_ring — connects Source→Transform");
    gra::StaticGraph<CS, CT> graph{};
    gra::GraphBuilder builder{graph};

    phyriad::transport::Ring<sch::SampleTick, 64> ring{};
    builder.wire_ring<0, 1>(ring);

    EXPECT(graph.wire_count() == 1u);
    EXPECT(builder.wires().size() == 1u);
    EXPECT(builder.wires()[0].src_node_id == 0u);
    EXPECT(builder.wires()[0].dst_node_id == 1u);
    EXPECT(builder.wires()[0].capacity    == 64u);
    EXPECT(!builder.wires()[0].message_hash.is_zero());
}

// §6 — GraphBuilder build() descriptor
static void test_graph_builder_descriptor() {
    SECTION("Test 6: GraphBuilder build() — descriptor has correct counts");
    gra::StaticGraph<CS, CT> graph{};
    gra::GraphBuilder builder{graph};

    phyriad::transport::Ring<sch::SampleTick, 256> r1{};
    builder.wire_ring<0, 1>(r1);

    auto desc = builder.build();
    EXPECT(desc.num_nodes == 2u);
    EXPECT(desc.num_wires == 1u);
    EXPECT(!desc.graph_hash.is_zero());
    EXPECT(sch::validate_schema_descriptor(desc));
}

// §7 — GraphValidator validate_type_compat
static void test_validator_type_compat() {
    SECTION("Test 7: GraphValidator validate_type_compat — OK and TypeMismatch");

    // Good wires: non-zero message_hash
    std::vector<sch::WireDescriptor> good;
    {
        sch::WireDescriptor wd{};
        wd.message_hash = sch::schema_hash<sch::SampleTick>();
        wd.src_node_id = 0; wd.dst_node_id = 1; wd.wire_id = 0;
        good.push_back(wd);
    }
    auto r = gra::GraphValidator::validate_type_compat(good);
    EXPECT(r.ok());

    // Bad wire: zero message_hash
    std::vector<sch::WireDescriptor> bad;
    {
        sch::WireDescriptor wd{};
        wd.message_hash = {};  // zero — invalid
        wd.src_node_id = 0; wd.dst_node_id = 1; wd.wire_id = 42;
        bad.push_back(wd);
    }
    auto rb = gra::GraphValidator::validate_type_compat(bad);
    EXPECT(!rb.ok());
    EXPECT(rb.code == gra::ValidationResult::Code::TypeMismatch);
    EXPECT(rb.wire_id == 42u);
}

// §8 — GraphValidator validate_acyclicity
static void test_validator_acyclicity() {
    SECTION("Test 8: GraphValidator validate_acyclicity — DAG ok, cycle detected");

    // Linear DAG: 0→1→2
    auto mk = [](uint32_t src, uint32_t dst, uint32_t wid) {
        sch::WireDescriptor wd{};
        wd.src_node_id = src; wd.dst_node_id = dst; wd.wire_id = wid;
        wd.message_hash = sch::schema_hash<sch::SampleTick>();
        return wd;
    };

    std::vector<sch::WireDescriptor> dag = { mk(0,1,0), mk(1,2,1) };
    auto r1 = gra::GraphValidator::validate_acyclicity(dag, 3);
    EXPECT(r1.ok());

    // Cycle: 0→1→2→0
    std::vector<sch::WireDescriptor> cyc = { mk(0,1,0), mk(1,2,1), mk(2,0,2) };
    auto r2 = gra::GraphValidator::validate_acyclicity(cyc, 3);
    EXPECT(!r2.ok());
    EXPECT(r2.code == gra::ValidationResult::Code::CycleDetected);

    // Empty graph — OK
    auto r3 = gra::GraphValidator::validate_acyclicity({}, 0);
    EXPECT(r3.ok());
}

// §9 — GraphValidator validate_reachability
static void test_validator_reachability() {
    SECTION("Test 9: GraphValidator validate_reachability — unreachable node");

    auto mk = [](uint32_t src, uint32_t dst) {
        sch::WireDescriptor wd{};
        wd.src_node_id = src; wd.dst_node_id = dst;
        wd.message_hash = sch::schema_hash<sch::SampleTick>();
        return wd;
    };

    // 3-node graph: 0→1 (node 2 has no incoming or outgoing — unreachable)
    std::vector<sch::WireDescriptor> w = { mk(0, 1) };
    auto r = gra::GraphValidator::validate_reachability(w, 3);
    EXPECT(!r.ok());
    EXPECT(r.code == gra::ValidationResult::Code::UnreachableNode);

    // All reachable: 0→1→2
    std::vector<sch::WireDescriptor> ok = { mk(0,1), mk(1,2) };
    auto r2 = gra::GraphValidator::validate_reachability(ok, 3);
    EXPECT(r2.ok());

    // Single node, no wires → trivially reachable
    auto r3 = gra::GraphValidator::validate_reachability({}, 1);
    EXPECT(r3.ok());
}

// §10 — GraphValidator validate_all
static void test_validator_all() {
    SECTION("Test 10: GraphValidator validate_all chains all checks");

    auto mk = [](uint32_t src, uint32_t dst, uint32_t wid) {
        sch::WireDescriptor wd{};
        wd.src_node_id = src; wd.dst_node_id = dst; wd.wire_id = wid;
        wd.message_hash = sch::schema_hash<sch::SampleTick>();
        return wd;
    };

    // Perfect DAG: 0→1
    std::vector<sch::WireDescriptor> perfect = { mk(0, 1, 0) };
    auto r1 = gra::GraphValidator::validate_all(perfect, 2);
    EXPECT(r1.ok());

    // Cycle halts at acyclicity check
    std::vector<sch::WireDescriptor> cyc = { mk(0,1,0), mk(1,0,1) };
    auto r2 = gra::GraphValidator::validate_all(cyc, 2);
    EXPECT(!r2.ok());
    EXPECT(r2.code == gra::ValidationResult::Code::CycleDetected);
}

// §11 — ValidationResult enum values
static void test_validation_result_enum() {
    SECTION("Test 11: ValidationResult::Code enum values");
    using C = gra::ValidationResult::Code;
    EXPECT(static_cast<uint8_t>(C::OK)              == 0u);
    EXPECT(static_cast<uint8_t>(C::TypeMismatch)     == 1u);
    EXPECT(static_cast<uint8_t>(C::CycleDetected)    == 2u);
    EXPECT(static_cast<uint8_t>(C::UnreachableNode)  == 3u);
    EXPECT(static_cast<uint8_t>(C::EmptyGraph)       == 4u);

    gra::ValidationResult ok_r = gra::ValidationResult::ok_result();
    EXPECT(ok_r.ok());
    EXPECT(ok_r.code == C::OK);
}

// §12 — ConfigError
static void test_config_error() {
    SECTION("Test 12: ConfigError default values and make() factory");
    api::ConfigError e{};
    EXPECT(e.ok());
    EXPECT(e.code == api::ConfigErrorCode::OK);
    EXPECT(e.offending_node_id == UINT32_MAX);

    auto bad = api::ConfigError::make(
        api::ConfigErrorCode::TypeMismatch,
        "wire type mismatch", 3, 7);
    EXPECT(!bad.ok());
    EXPECT(bad.code == api::ConfigErrorCode::TypeMismatch);
    EXPECT(bad.offending_node_id == 3u);
    EXPECT(bad.offending_wire_id == 7u);
    EXPECT(bad.message_view() == "wire type mismatch");
}

// §13 — ConfigErrorCode enum values
static void test_config_error_code() {
    SECTION("Test 13: ConfigErrorCode enum values");
    using C = api::ConfigErrorCode;
    EXPECT(static_cast<uint8_t>(C::OK)                == 0u);
    EXPECT(static_cast<uint8_t>(C::DuplicateNodeName) == 1u);
    EXPECT(static_cast<uint8_t>(C::UnknownNodeName)   == 2u);
    EXPECT(static_cast<uint8_t>(C::TypeMismatch)      == 3u);
    EXPECT(static_cast<uint8_t>(C::EmptyGraph)        == 4u);
    EXPECT(static_cast<uint8_t>(C::CycleDetected)     == 5u);
    EXPECT(static_cast<uint8_t>(C::UnreachableNode)   == 6u);
    EXPECT(static_cast<uint8_t>(C::ValidationFailed)  == 7u);
}

// §14 — WirePolicy enum values
static void test_wire_policy_enum() {
    SECTION("Test 14: WirePolicy enum values");
    EXPECT(static_cast<uint8_t>(api::WirePolicy::StrictWaitOrEvict) == 0u);
    EXPECT(static_cast<uint8_t>(api::WirePolicy::OverwriteOnDeath)  == 1u);
    EXPECT(static_cast<uint8_t>(api::WirePolicy::BlockUntilEvicted) == 2u);
}

// §15 — DslGraphBuilder basic build
static void test_dsl_build_ok() {
    SECTION("Test 15: DslGraphBuilder — build_graph().node.wire.build() OK");

    auto result = api::build_graph()
        .node<CS>("source")
        .node<CT>("sink")
        .wire("source").to("sink")
        .build();

    EXPECT(result.has_value());
    if (result) {
        EXPECT(result->nodes.size() == 2u);
        EXPECT(result->wires.size() == 1u);
        EXPECT(!result->header.graph_hash.is_zero());
        EXPECT(sch::validate_schema_descriptor(result->header));
        std::printf("    graph_hash.low = 0x%016llx\n",
                    (unsigned long long)result->header.graph_hash.low);
    }
}

// §16 — Duplicate node name
static void test_dsl_duplicate_name() {
    SECTION("Test 16: DslGraphBuilder — duplicate node name → error");

    auto result = api::build_graph()
        .node<CS>("alpha")
        .node<CT>("alpha")   // duplicate!
        .wire("alpha").to("alpha")
        .build();

    EXPECT(!result.has_value());
    if (!result)
        EXPECT(result.error().code == api::ConfigErrorCode::DuplicateNodeName);
}

// §17 — Unknown wire source
static void test_dsl_unknown_node() {
    SECTION("Test 17: DslGraphBuilder — unknown wire source → error");

    auto result = api::build_graph()
        .node<CS>("source")
        .wire("nonexistent").to("source")
        .build();

    EXPECT(!result.has_value());
    if (!result)
        EXPECT(result.error().code == api::ConfigErrorCode::UnknownNodeName);
}

// §18 — Type mismatch
// CS produces SampleTick, CT consumes SampleTick — they match.
// Build a fake mismatch by using CS→CS (both Source, no input_type on CS).
// Actually: two Sources have no input_type so both hashes are zero → no mismatch.
// Use CT→CS mismatch instead: CT produces SampleTick, CS has no input_type.
// Simplest: provide a custom node with mismatching type.
namespace {
struct OtherTick { uint64_t ts{0}; };
static_assert(std::is_trivially_copyable_v<OtherTick>);
struct OtherSource {
    using output_type = OtherTick;
    [[nodiscard]] phyriad::node::Outlet<OtherTick>& outlet() noexcept { return out_; }
    [[nodiscard]] auto on_start() noexcept -> std::expected<void, phyriad::Error> { return {}; }
    [[nodiscard]] auto on_stop()  noexcept -> std::expected<void, phyriad::Error> { return {}; }
    [[nodiscard]] auto tick()     noexcept -> std::expected<void, phyriad::Error> { return {}; }
private:
    phyriad::node::Outlet<OtherTick> out_;
};
} // anon namespace

static void test_dsl_type_mismatch() {
    SECTION("Test 18: DslGraphBuilder — type mismatch → error");

    // OtherSource produces OtherTick, CT consumes SampleTick — mismatch.
    auto result = api::build_graph()
        .node<OtherSource>("source")
        .node<CT>("sink")
        .wire("source").to("sink")
        .build();

    EXPECT(!result.has_value());
    if (!result)
        EXPECT(result.error().code == api::ConfigErrorCode::TypeMismatch);
}

// §19 — Empty graph
static void test_dsl_empty_graph() {
    SECTION("Test 19: DslGraphBuilder — empty graph → error");

    auto result1 = api::build_graph().build();  // no nodes
    EXPECT(!result1.has_value());
    EXPECT(result1.error().code == api::ConfigErrorCode::EmptyGraph);

    auto result2 = api::build_graph()
        .node<CS>("src")
        // no wires
        .build();
    EXPECT(!result2.has_value());
    EXPECT(result2.error().code == api::ConfigErrorCode::EmptyGraph);
}

// §20 — validate() on linear graph
static void test_dsl_validate_dag() {
    SECTION("Test 20: DslGraphBuilder validate() — acyclicity OK on linear graph");

    auto result = api::build_graph()
        .node<CS>("src")
        .node<CT>("dst")
        .wire("src").to("dst")
        .validate()
        .build();

    EXPECT(result.has_value());
    if (result)
        EXPECT(result->nodes.size() == 2u);
}

// §21 — NodeHandle make<N>
static void test_node_handle_make() {
    SECTION("Test 21: NodeHandle make<N> — valid, tick/start/stop");

    auto h = api::NodeHandle::make<CS>(0u);
    EXPECT(h.valid());
    EXPECT(h.node_id == 0u);
    EXPECT(h.outlet_count == 1u);
    EXPECT(h.inlet_count  == 0u);
    EXPECT(!h.output_type_hash.is_zero());
    EXPECT(h.input_type_hash.is_zero());

    h.start();
    auto r = h.tick();
    // tick() fires through the function pointer.  The outlet is not connected
    // yet (no ring wired), so publish() returns InvalidHandle — expected.
    EXPECT(!r.has_value());
    if (!r) EXPECT(r.error().code == phyriad::ErrorCode::InvalidHandle);
    h.stop();
    h.destroy();
    EXPECT(!h.valid());
}

// §22 — NodeHandle wrap<N>
static void test_node_handle_wrap() {
    SECTION("Test 22: NodeHandle wrap<N> — non-owning, destroy is no-op");

    CS src{};
    auto h = api::NodeHandle::wrap(&src, 1u);
    EXPECT(h.valid());
    EXPECT(h.node_id == 1u);

    h.start();
    auto r = h.tick();
    // tick() fires through the function pointer.  Outlet not connected →
    // publish() returns InvalidHandle — expected before graph wiring.
    EXPECT(!r.has_value());
    if (!r) EXPECT(r.error().code == phyriad::ErrorCode::InvalidHandle);
    h.destroy();
    // Node still alive — wrap doesn't own it
    EXPECT(!h.valid());   // state cleared to nullptr
    EXPECT(src.outlet().connected() == false);  // node itself unaffected
}

// §23 — NodeHandle alignment
static void test_node_handle_alignment() {
    SECTION("Test 23: NodeHandle alignment == hal::kDestructivePad");
    EXPECT(alignof(api::NodeHandle) == phyriad::hal::kDestructivePad);
    std::printf("    sizeof(NodeHandle) = %zu bytes\n", sizeof(api::NodeHandle));
}

// §24 — WireHandle make<T>
static void test_wire_handle_make() {
    SECTION("Test 24: WireHandle make<T> — valid, capacity rounded to pow2");

    auto h = api::WireHandle::make<sch::SampleTick>(0u, 0u, 1u, 100u);
    EXPECT(h.valid());
    EXPECT(h.capacity == 128u);  // next power-of-2 >= 100
    EXPECT(!h.type_hash.is_zero());
    EXPECT(h.src_node_id == 0u);
    EXPECT(h.dst_node_id == 1u);

    // Minimum cap of 64
    auto h2 = api::WireHandle::make<sch::SampleTick>(0u, 0u, 1u, 1u);
    EXPECT(h2.capacity == 64u);

    h.destroy();
    EXPECT(!h.valid());

    h2.destroy();
}

// §25 — WireHandle connect_outlet / connect_inlet
static void test_wire_handle_connect() {
    SECTION("Test 25: WireHandle connect_outlet/connect_inlet round-trip");

    CS src{};
    CT dst{};
    auto wire = api::WireHandle::make<sch::SampleTick>(0u, 0u, 1u, 64u);

    EXPECT(!src.outlet().connected());
    EXPECT(!dst.inlet().connected());

    wire.connect_outlet(&src.outlet());
    wire.connect_inlet(&dst.inlet());

    EXPECT(src.outlet().connected());
    EXPECT(dst.inlet().connected());

    // Disconnect the inlet before destroying the wire — Inlet::~Inlet calls
    // ring->unsubscribe() and would touch freed memory after wire.destroy().
    // Outlet doesn't hold a ring subscription, only send_fn_ + ctx_ pointers,
    // so it's safe to let its destructor run after the wire is gone.
    // Mirrors GraphRuntime's "destroy nodes before wires" discipline.
    dst.inlet().disconnect();
    wire.destroy();
}

// §26 — WireRegistry
static void test_wire_registry() {
    SECTION("Test 26: WireRegistry register_type + create_wire");

    api::WireRegistry wr;
    EXPECT(wr.type_count() == 0u);

    wr.register_type<sch::SampleTick>();
    EXPECT(wr.type_count() == 1u);
    EXPECT(wr.has_type(sch::schema_hash<sch::SampleTick>()));

    auto result = wr.create_wire(sch::schema_hash<sch::SampleTick>(),
                                  0u, 0u, 1u, 64u);
    EXPECT(result.has_value());
    if (result) {
        EXPECT(result->valid());
        EXPECT(result->capacity == 64u);
        result->destroy();
    }

    // Unknown type → error
    auto miss = wr.create_wire(sch::Hash128{0xDEAD, 0xBEEF}, 0u, 0u, 1u, 64u);
    EXPECT(!miss.has_value());
    EXPECT(miss.error().code == phyriad::ErrorCode::SchemaMismatch);

    // Duplicate registration ignored
    wr.register_type<sch::SampleTick>();
    EXPECT(wr.type_count() == 1u);
}

// §27 — NodeRegistry
static void test_node_registry() {
    SECTION("Test 27: NodeRegistry register_node + instantiate");

    api::NodeRegistry reg;
    EXPECT(reg.node_count() == 0u);

    reg.register_node<CS>("source");
    reg.register_node<CT>("sink");
    EXPECT(reg.node_count() == 2u);

    EXPECT(reg.has_node("source"));
    EXPECT(reg.has_node("sink"));
    EXPECT(!reg.has_node("unknown"));

    auto r1 = reg.instantiate("source", 0u);
    EXPECT(r1.has_value());
    if (r1) {
        EXPECT(r1->valid());
        EXPECT(r1->outlet_count == 1u);
        r1->destroy();
    }

    auto miss = reg.instantiate("unknown", 99u);
    EXPECT(!miss.has_value());
    EXPECT(miss.error().code == phyriad::ErrorCode::InvalidHandle);

    // Wire registry auto-populated
    EXPECT(reg.wire_registry().has_type(sch::schema_hash<sch::SampleTick>()));
}

// §28 — placement:: factories
static void test_placement_factories() {
    SECTION("Test 28: placement:: factory functions — correct fields");
    using namespace phyriad::api::placement;

    auto g = generic();
    EXPECT(g.role == phyriad::scheduler::ThreadRole::GENERIC);

    auto u = ui_main();
    EXPECT(u.role == phyriad::scheduler::ThreadRole::UI_MAIN);

    auto r = render_pcie_gpu(1u);
    EXPECT(r.role           == phyriad::scheduler::ThreadRole::RENDER);
    EXPECT(r.prefers_pcie_gpu == true);
    EXPECT(r.pcie_gpu_index == 1u);

    auto l = logic();
    EXPECT(l.role == phyriad::scheduler::ThreadRole::LOGIC);

    auto c = compute_vcache();
    EXPECT(c.role                == phyriad::scheduler::ThreadRole::COMPUTE);
    EXPECT(c.prefer_vcache       == true);
    EXPECT(c.prefer_isolated_core == true);

    auto io = phyriad::api::placement::io();
    EXPECT(io.role == phyriad::scheduler::ThreadRole::IO);
}

// §29 — DslGraphBuilder hints propagated
static void test_dsl_hints_propagated() {
    SECTION("Test 29: DslGraphBuilder hints — propagated to BuiltGraph.hints");

    using namespace phyriad::api::placement;
    auto result = api::build_graph()
        .node<CS>("src",  compute_vcache())
        .node<CT>("sink", io())
        .wire("src").to("sink")
        .build();

    EXPECT(result.has_value());
    if (result) {
        EXPECT(result->hints.size() == 2u);
        EXPECT(result->hints[0].role == phyriad::scheduler::ThreadRole::COMPUTE);
        EXPECT(result->hints[0].prefer_vcache == true);
        EXPECT(result->hints[1].role == phyriad::scheduler::ThreadRole::IO);
    }
}

// §30 — BuiltGraph shm_size
static void test_built_graph_shm_size() {
    SECTION("Test 30: BuiltGraph shm_size() — correct byte count");

    auto result = api::build_graph()
        .node<CS>("a").node<CT>("b")
        .wire("a").to("b")
        .build();

    EXPECT(result.has_value());
    if (result) {
        std::size_t expected =
            sizeof(sch::GraphSchemaDescriptor)
            + 2u * sizeof(sch::NodeDescriptor)
            + 1u * sizeof(sch::WireDescriptor);
        EXPECT(result->shm_size() == expected);
        std::printf("    shm_size = %zu bytes (header=%zu, node=%zu, wire=%zu)\n",
                    result->shm_size(),
                    sizeof(sch::GraphSchemaDescriptor),
                    sizeof(sch::NodeDescriptor),
                    sizeof(sch::WireDescriptor));
    }
}

// §31 — write_shm → DynamicGraph::attach round-trip
static void test_shm_round_trip() {
    SECTION("Test 31: write_shm() → DynamicGraph::attach() round-trip");

    auto result = api::build_graph()
        .node<CS>("a").node<CT>("b")
        .wire("a").to("b")
        .build();

    EXPECT(result.has_value());
    if (!result) return;

    const auto sz = result->shm_size();
    std::vector<std::byte> buf(sz, std::byte{0});
    result->write_shm(buf.data());

    // Attach with the correct hash → must succeed
    auto dg = gra::DynamicGraph::attach(buf.data(), result->header.graph_hash);
    EXPECT(dg.has_value());
    if (dg) {
        EXPECT(dg->node_count() == 2u);
        EXPECT(dg->wire_count() == 1u);
        EXPECT(dg->node_descriptors() != nullptr);
        EXPECT(dg->wire_descriptors() != nullptr);
    }

    // Attach with wrong hash → SchemaMismatch
    auto dg2 = gra::DynamicGraph::attach(buf.data(), sch::Hash128{0xDEAD, 0xBEEF});
    EXPECT(!dg2.has_value());
    if (!dg2)
        EXPECT(dg2.error().code == phyriad::ErrorCode::SchemaMismatch);

    // Attach null → InvalidHandle
    auto dg3 = gra::DynamicGraph::attach(nullptr, result->header.graph_hash);
    EXPECT(!dg3.has_value());
    if (!dg3)
        EXPECT(dg3.error().code == phyriad::ErrorCode::InvalidHandle);
}

// ── main ──────────────────────────────────────────────────────────────────────
int main() {
    std::puts("[graph_test] phyriad_graph pillar — Phase 1.A");
    std::puts("----------------------------------------------------------------");
    std::printf("  sizeof(GraphSchemaDescriptor) = %zu bytes\n",
                sizeof(sch::GraphSchemaDescriptor));
    std::printf("  sizeof(NodeDescriptor)        = %zu bytes\n",
                sizeof(sch::NodeDescriptor));
    std::printf("  sizeof(WireDescriptor)        = %zu bytes\n",
                sizeof(sch::WireDescriptor));
    std::printf("  sizeof(StaticGraph<CS,CT>)    = %zu bytes\n",
                sizeof(gra::StaticGraph<CS, CT>));
    std::puts("----------------------------------------------------------------");

    test_graph_concept();
    test_dynamic_graph_concept();
    test_schema_hash_differs();
    test_schema_hash_deterministic();
    test_graph_builder_wire_ring();
    test_graph_builder_descriptor();
    test_validator_type_compat();
    test_validator_acyclicity();
    test_validator_reachability();
    test_validator_all();
    test_validation_result_enum();
    test_config_error();
    test_config_error_code();
    test_wire_policy_enum();
    test_dsl_build_ok();
    test_dsl_duplicate_name();
    test_dsl_unknown_node();
    test_dsl_type_mismatch();
    test_dsl_empty_graph();
    test_dsl_validate_dag();
    test_node_handle_make();
    test_node_handle_wrap();
    test_node_handle_alignment();
    test_wire_handle_make();
    test_wire_handle_connect();
    test_wire_registry();
    test_node_registry();
    test_placement_factories();
    test_dsl_hints_propagated();
    test_built_graph_shm_size();
    test_shm_round_trip();

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
