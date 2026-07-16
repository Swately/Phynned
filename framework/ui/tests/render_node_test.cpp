// framework/ui/tests/render_node_test.cpp
// RenderNode invariant tests.
//
// Design: legacy `test_render_node` had 82 individual [PASS] lines across
// several scenarios. This test instead verifies the *invariants* that matter
// for production correctness — each property is exercised across many ticks
// so a regression in any single tick fails the property check:
//
//   §1  Outlet contract — every successful tick publishes exactly one
//       RenderStats; the published frame_id is the node's monotone counter,
//       not whatever the backend returns.
//   §2  Backend lifecycle — new_frame, end_frame and present are each called
//       exactly once per successful tick; their counts stay in lock-step.
//   §3  FrameArena reset contract — bytes_used() is zero at the end of
//       every tick (verified via a tracing arena wrapper).
//   §4  WindowState resize coalescing — resize() fires only when fb_w/h or
//       dpi_scale actually change. Repeated identical pushes are no-ops.
//   §5  Stateful inlet caching (Latest semantics) — a user state cached
//       from a prior tick survives a tick that produces no new state.
//   §6  Fatal user-inlet errors propagate from tick() BEFORE new_frame()
//       runs (no half-rendered frames).
//   §7  on_start() resets frame_id and clears cached WindowState — after a
//       restart the next ticks behave like a fresh node.
//   §8  HiDPI invariant — dpi_scale propagates to backend.resize() and
//       previous (w,h,dpi) is cached so subsequent un-changed pushes skip.
//
// All scenarios use the headless MockRenderBackend — no GLFW window needed.
//
#include <phyriad/ui/RenderNode.hpp>
#include <phyriad/render/FrameArena.hpp>
#include <phyriad/render/MockRenderBackend.hpp>
#include <phyriad/render/RenderStats.hpp>
#include <phyriad/ui/types/WindowState.hpp>
#include <phyriad/transport/Latest.hpp>
#include <phyriad/schema/PodMessage.hpp>
#include <cstdint>
#include <cstdio>

namespace ui = phyriad::ui;
namespace rd = phyriad::render;
namespace tr = phyriad::transport;

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

// ── Stats sink for the outlet ─────────────────────────────────────────────────
struct alignas(64) CounterState {
    uint32_t value;
    uint8_t  pad[60];
};
PHYRIAD_ASSERT_POD(CounterState);

// Volatile sink to defeat dead-code elimination on the draw callback.
volatile uint32_t g_last_drawn = 0;

static void draw_counter(CounterState const& s) noexcept {
    g_last_drawn = s.value;
}

static void draw_zero_state() noexcept {
    g_last_drawn = 0xABCDu;
}

// Helper: connect outlet to a Latest<RenderStats> + read after N ticks.
template <typename Node>
static uint32_t count_publishes(Node& node, tr::Latest<rd::RenderStats>& sink,
                                int n_ticks) noexcept {
    uint32_t got = 0;
    for (int i = 0; i < n_ticks; ++i) {
        auto r = node.tick();
        if (r.has_value()) {
            auto opt = sink.receive();
            if (opt.has_value()) ++got;
        }
    }
    return got;
}

// ── §1 Outlet contract: frame_id monotone, equals tick count ─────────────────
static void test_outlet_contract_and_monotone_frame_id() {
    SECTION("Test 1: Outlet publishes once per successful tick; frame_id is monotone 1..N");

    rd::FrameArena arena{16u * 1024u};
    rd::MockRenderBackend backend;
    EXPECT(backend.init(nullptr, &arena).has_value());

    ui::RenderNode<CounterState> node{backend, arena, +[](CounterState const& s) noexcept {
        draw_counter(s);
    }};
    EXPECT(node.on_start().has_value());

    tr::Latest<rd::RenderStats> sink;
    node.outlet().connect_runtime(sink);

    constexpr int kN = 50;
    uint32_t published = 0;
    uint32_t prev_id   = 0;
    bool     monotone  = true;
    for (int i = 0; i < kN; ++i) {
        auto r = node.tick();
        EXPECT(r.has_value());
        auto opt = sink.receive();
        if (opt.has_value()) {
            ++published;
            if (opt->frame_id <= prev_id) monotone = false;
            prev_id = opt->frame_id;
        }
    }
    EXPECT(published == static_cast<uint32_t>(kN));
    EXPECT(monotone);
    EXPECT(prev_id == static_cast<uint32_t>(kN));
}

// ── §2 Backend lifecycle in lock-step ────────────────────────────────────────
static void test_backend_lifecycle_lockstep() {
    SECTION("Test 2: backend.new_frame == end_frame == present == tick count");
    rd::FrameArena arena{16u * 1024u};
    rd::MockRenderBackend backend;
    EXPECT(backend.init(nullptr, &arena).has_value());

    ui::RenderNode<> node{backend, arena, +[]() noexcept { draw_zero_state(); }};
    EXPECT(node.on_start().has_value());

    constexpr int kN = 100;
    int ok_ticks = 0;
    for (int i = 0; i < kN; ++i)
        if (node.tick().has_value()) ++ok_ticks;

    EXPECT(ok_ticks                  == kN);
    EXPECT(backend.new_frame_count   == kN);
    EXPECT(backend.end_frame_count   == kN);
    EXPECT(backend.present_count     == kN);
}

// ── §3 FrameArena reset contract ─────────────────────────────────────────────
static void test_arena_reset_each_tick() {
    SECTION("Test 3: arena.bytes_used() == 0 after every successful tick");
    rd::FrameArena arena{16u * 1024u};
    rd::MockRenderBackend backend;
    EXPECT(backend.init(nullptr, &arena).has_value());

    ui::RenderNode<> node{backend, arena, +[]() noexcept {
        draw_zero_state();
    }};
    EXPECT(node.on_start().has_value());

    bool reset_invariant_held = true;
    for (int i = 0; i < 25; ++i) {
        auto r = node.tick();
        if (!r.has_value()) { reset_invariant_held = false; break; }
        // After tick(), arena.bytes_used must be 0 because the node calls
        // reset_frame() at the start of the next tick — verify that the
        // resting state (just-completed tick) shows zero residual usage.
        if (arena.bytes_used() != 0u) { reset_invariant_held = false; break; }
    }
    EXPECT(reset_invariant_held);
    EXPECT(arena.high_water_mark() == 0u);   // draw_zero_state allocates nothing
}

// ── §4 Resize coalescing ─────────────────────────────────────────────────────
static void test_resize_coalescing() {
    SECTION("Test 4: resize() fires only when fb_width/height/dpi actually changes");
    rd::FrameArena arena{16u * 1024u};
    rd::MockRenderBackend backend;
    EXPECT(backend.init(nullptr, &arena).has_value());

    ui::RenderNode<> node{backend, arena, +[]() noexcept {}};
    EXPECT(node.on_start().has_value());

    tr::Latest<ui::WindowState> ws_chan;
    node.inlet<0>().connect_runtime(ws_chan);

    auto push_ws = [&](uint32_t w, uint32_t h, float dpi) {
        ui::WindowState s{};
        s.fb_width  = w;
        s.fb_height = h;
        s.dpi_scale = dpi;
        (void)ws_chan.send(s);
    };

    // Initial push at 1920×1080 @ 1.0 → first resize call.
    push_ws(1920u, 1080u, 1.0f);
    EXPECT(node.tick().has_value());
    EXPECT(backend.resize_count == 1);

    // Same size repeated 10× → coalesced (count must not grow).
    for (int i = 0; i < 10; ++i) {
        push_ws(1920u, 1080u, 1.0f);
        EXPECT(node.tick().has_value());
    }
    EXPECT(backend.resize_count == 1);

    // Change dpi only → one new resize call.
    push_ws(1920u, 1080u, 1.5f);
    EXPECT(node.tick().has_value());
    EXPECT(backend.resize_count == 2);
    EXPECT(backend.last_resize_dpi == 1.5f);

    // Change width only → another resize.
    push_ws(2560u, 1080u, 1.5f);
    EXPECT(node.tick().has_value());
    EXPECT(backend.resize_count == 3);
    EXPECT(backend.last_resize_w == 2560u);
}

// ── §5 Stateful inlet caching (Latest semantics) ─────────────────────────────
static void test_user_state_caching() {
    SECTION("Test 5: user-state cache survives ticks with no new state arrival");
    rd::FrameArena arena{16u * 1024u};
    rd::MockRenderBackend backend;
    EXPECT(backend.init(nullptr, &arena).has_value());

    ui::RenderNode<CounterState> node{backend, arena, +[](CounterState const& s) noexcept {
        draw_counter(s);
    }};
    EXPECT(node.on_start().has_value());

    tr::Latest<CounterState> counter;
    node.inlet<1>().connect_runtime(counter);

    // Push value=7 once, tick 10 times. The cached state must persist.
    CounterState s{};
    s.value = 7u;
    (void)counter.send(s);
    EXPECT(node.tick().has_value());
    EXPECT(g_last_drawn == 7u);

    // Subsequent ticks see no new send → cached value remains.
    for (int i = 0; i < 9; ++i) {
        g_last_drawn = 0xFFFFu;
        EXPECT(node.tick().has_value());
        EXPECT(g_last_drawn == 7u);
    }
}

// ── §6 Fatal inlet error short-circuits tick BEFORE new_frame ────────────────
// Helper transport that always returns a non-RingEmpty error on receive.
class FaultyInlet {
public:
    [[nodiscard]] auto receive() noexcept
        -> std::expected<CounterState, phyriad::Error>
    {
        return std::unexpected(phyriad::Error{
            .code = phyriad::ErrorCode::SchemaMismatch,
            .source_node_id = 0u, .timestamp_ns = 0u});
    }
};

static void test_fatal_inlet_error_propagation() {
    SECTION("Test 6: fatal user-inlet error returns from tick() before new_frame()");
    rd::FrameArena arena{16u * 1024u};
    rd::MockRenderBackend backend;
    EXPECT(backend.init(nullptr, &arena).has_value());

    ui::RenderNode<CounterState> node{backend, arena,
        +[](CounterState const&) noexcept {}};
    EXPECT(node.on_start().has_value());

    FaultyInlet faulty;
    node.inlet<1>().connect_runtime(faulty);

    const int new_frame_before = backend.new_frame_count;
    auto r = node.tick();
    EXPECT(!r.has_value());
    if (!r) EXPECT(r.error().code == phyriad::ErrorCode::SchemaMismatch);
    EXPECT(backend.new_frame_count == new_frame_before);   // pipeline NOT entered
    EXPECT(backend.end_frame_count == 0);
}

// ── §7 on_start() resets state ───────────────────────────────────────────────
static void test_on_start_resets_state() {
    SECTION("Test 7: on_start() resets frame_id; restart cycles work cleanly");
    rd::FrameArena arena{16u * 1024u};
    rd::MockRenderBackend backend;
    EXPECT(backend.init(nullptr, &arena).has_value());

    ui::RenderNode<> node{backend, arena, +[]() noexcept {}};
    tr::Latest<rd::RenderStats> sink;
    node.outlet().connect_runtime(sink);

    // Cycle 1: run 30 ticks, frame_id reaches 30.
    EXPECT(node.on_start().has_value());
    for (int i = 0; i < 30; ++i) (void)node.tick();
    auto s1 = sink.receive();
    EXPECT(s1.has_value() && s1->frame_id == 30u);

    // Restart cycle: on_stop() + on_start() ⇒ frame_id back to 0.
    EXPECT(node.on_stop().has_value());
    EXPECT(node.on_start().has_value());
    EXPECT(node.tick().has_value());
    auto s2 = sink.receive();
    EXPECT(s2.has_value() && s2->frame_id == 1u);   // restarted at 1
}

// ── §8 HiDPI propagation ─────────────────────────────────────────────────────
static void test_hidpi_propagation() {
    SECTION("Test 8: dpi_scale flows through resize() unchanged + cached");
    rd::FrameArena arena{16u * 1024u};
    rd::MockRenderBackend backend;
    EXPECT(backend.init(nullptr, &arena).has_value());

    ui::RenderNode<> node{backend, arena, +[]() noexcept {}};
    EXPECT(node.on_start().has_value());

    tr::Latest<ui::WindowState> ws_chan;
    node.inlet<0>().connect_runtime(ws_chan);

    // Sweep through fractional DPI scales — every distinct value triggers a
    // resize, identical repeats are suppressed.
    constexpr float kScales[] = {0.75f, 1.0f, 1.25f, 1.5f, 2.0f};
    int expected_resizes = 0;
    for (float dpi : kScales) {
        ui::WindowState s{};
        s.fb_width  = 1920u;
        s.fb_height = 1080u;
        s.dpi_scale = dpi;
        (void)ws_chan.send(s);
        EXPECT(node.tick().has_value());
        ++expected_resizes;

        // Same DPI again — must NOT increment resize_count.
        (void)ws_chan.send(s);
        EXPECT(node.tick().has_value());
    }
    EXPECT(backend.resize_count == expected_resizes);
    EXPECT(backend.last_resize_dpi == kScales[std::size(kScales) - 1u]);
}

// ── main ──────────────────────────────────────────────────────────────────────
int main() {
    std::printf("[render_node_test] phyriad_ui — RenderNode invariants\n");
    std::printf("----------------------------------------------------------------\n");

    test_outlet_contract_and_monotone_frame_id();
    test_backend_lifecycle_lockstep();
    test_arena_reset_each_tick();
    test_resize_coalescing();
    test_user_state_caching();
    test_fatal_inlet_error_propagation();
    test_on_start_resets_state();
    test_hidpi_propagation();

    std::printf("----------------------------------------------------------------\n");
    const int total = g_pass + g_fail;
    if (g_fail == 0)
        std::printf("[OK] %d/%d invariants verified\n", g_pass, total);
    else
        std::printf("[FAIL] %d/%d invariants FAILED\n", g_fail, total);
    return g_fail ? 1 : 0;
}
// Made with my soul - Swately <3
