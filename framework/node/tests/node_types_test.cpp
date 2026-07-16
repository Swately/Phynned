// framework/node/tests/node_types_test.cpp
// Test suite para phyriad_node — pillar node.
//
// Tests:
//   Compile-time static_asserts (most checks happen at compile time):
//   1.  Lifecycle concepts: HasOnStart, HasOnStop, HasOnPause, HasOnResume
//   2.  Checkpointable concept — state_type + state() + restore_state()
//   3.  Source<CanonicalSource> — outlet(), output_type
//   4.  Sink/Transform<CanonicalTransform> — inlet(), on_message(), inlet(), outlet()
//   5.  Stateful<CanonicalTransform> — Checkpointable
//   6.  Node<CanonicalSource> and Node<CanonicalTransform>
//   7.  Runnable<CanonicalSource> — tick() + default_constructible
//   8.  WrappableNode<CanonicalTransform>
//   9.  Outlet<T> — not connected by default, publish returns InvalidHandle
//   10. Inlet<T>  — not connected by default, receive returns InvalidHandle
//   11. CoroFrameAllocator — allocate/deallocate round-trip
//   12. AwaitableTask<int> — co_return 42, value() == 42
//   13. AwaitableTask<void> — co_return void, handle done
//

#include <phyriad/node/NodeAll.hpp>
#include <phyriad/node/canonical/CanonicalSource.hpp>
#include <phyriad/node/canonical/CanonicalTransform.hpp>
#include <phyriad/schema/Schema.hpp>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <coroutine>
#include <type_traits>

namespace nd = phyriad::node;

// ─────────────────────────────────────────────────────────────────────────────
// Micro-test framework
// ─────────────────────────────────────────────────────────────────────────────
static int g_tests_run    = 0;
static int g_tests_failed = 0;

#define EXPECT(cond)                                                        \
    do {                                                                    \
        ++g_tests_run;                                                      \
        if (!(cond)) {                                                      \
            ++g_tests_failed;                                               \
            std::fprintf(stderr, "  [FAIL] %s:%d: %s\n",                  \
                         __FILE__, __LINE__, #cond);                        \
        }                                                                   \
    } while(0)

#define SECTION(name) std::puts("  § " name)

// ─────────────────────────────────────────────────────────────────────────────
// Compile-time concept checks (fail to compile if broken)
// ─────────────────────────────────────────────────────────────────────────────

using CS = nd::canonical::CanonicalSource;
using CT = nd::canonical::CanonicalTransform;

// §3 — Source / Node
static_assert(nd::Source<CS>);
static_assert(nd::Node<CS>);
static_assert(!nd::Sink<CS>);

// §4 — Sink / Transform / Node
static_assert(nd::Sink<CT>);
static_assert(nd::Source<CT>);
static_assert(nd::Transform<CT>);
static_assert(nd::Node<CT>);

// §5 — Stateful
static_assert(nd::Stateful<CT>);
static_assert(!nd::Stateful<CS>);   // CanonicalSource has no state_type

// §7 — Runnable
static_assert(phyriad::Runnable<CS>);
static_assert(phyriad::Runnable<CT>);

// §8 — WrappableNode
static_assert(phyriad::WrappableNode<CS>);
static_assert(phyriad::WrappableNode<CT>);

// ─────────────────────────────────────────────────────────────────────────────
// §1 — Lifecycle concepts
// ─────────────────────────────────────────────────────────────────────────────
static void test_lifecycle_concepts() {
    SECTION("Test 1: lifecycle concepts — HasOnStart, HasOnStop");

    EXPECT(nd::HasOnStart<CS>);
    EXPECT(nd::HasOnStop<CS>);
    EXPECT(!nd::HasOnPause<CS>);
    EXPECT(!nd::HasOnResume<CS>);

    EXPECT(nd::HasOnStart<CT>);
    EXPECT(nd::HasOnStop<CT>);

    // try_start / try_stop on a node with hooks
    CS src;
    EXPECT(nd::try_start(src).has_value());
    EXPECT(nd::try_stop(src).has_value());
    EXPECT(nd::try_pause(src).has_value());   // no-op, returns {}
    EXPECT(nd::try_resume(src).has_value());  // no-op, returns {}
}

// ─────────────────────────────────────────────────────────────────────────────
// §2 — Checkpointable
// ─────────────────────────────────────────────────────────────────────────────
static void test_checkpointable() {
    SECTION("Test 2: Checkpointable — state round-trip");

    CT transform;
    EXPECT(nd::try_start(transform).has_value());

    // State: msg_count starts at 0
    EXPECT(transform.state().msg_count == 0u);

    // Simulate some processing (restore_state with modified value)
    CT::state_type snap{.msg_count = 42u};
    transform.restore_state(snap);
    EXPECT(transform.state().msg_count == 42u);

    // Restore to 0
    transform.restore_state({.msg_count = 0u});
    EXPECT(transform.state().msg_count == 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// §9 — Outlet<T> unconnected
// ─────────────────────────────────────────────────────────────────────────────
static void test_outlet_unconnected() {
    SECTION("Test 9: Outlet<T> — not connected, publish returns InvalidHandle");

    nd::Outlet<phyriad::schema::SampleTick> out;
    EXPECT(!out.connected());

    auto r = out.publish({});
    EXPECT(!r.has_value());
    EXPECT(r.error().code == phyriad::ErrorCode::InvalidHandle);
}

// ─────────────────────────────────────────────────────────────────────────────
// §10 — Inlet<T> unconnected
// ─────────────────────────────────────────────────────────────────────────────
static void test_inlet_unconnected() {
    SECTION("Test 10: Inlet<T> — not connected, receive returns InvalidHandle");

    nd::Inlet<phyriad::schema::SampleTick> in;
    EXPECT(!in.connected());

    auto r = in.receive();
    EXPECT(!r.has_value());
    EXPECT(r.error().code == phyriad::ErrorCode::InvalidHandle);
}

// ─────────────────────────────────────────────────────────────────────────────
// §11 — CoroFrameAllocator
// ─────────────────────────────────────────────────────────────────────────────
static void test_coro_frame_allocator() {
    SECTION("Test 11: CoroFrameAllocator — allocate/deallocate round-trip");

    auto& alloc = nd::CoroFrameAllocator::instance();

    // Allocate a normal-sized frame (≤ kFrameSize).
    constexpr std::size_t kSmall = 512;
    void* p1 = alloc.allocate(kSmall);
    EXPECT(p1 != nullptr);
    // Write to it to detect memory corruption.
    std::memset(p1, 0xAB, kSmall);
    alloc.deallocate(p1, kSmall);

    // Allocate many frames to test bitmap cycling.
    constexpr std::size_t kN = 10;
    void* ptrs[kN]{};
    for (std::size_t i = 0; i < kN; ++i) {
        ptrs[i] = alloc.allocate(256);
        EXPECT(ptrs[i] != nullptr);
    }
    for (std::size_t i = 0; i < kN; ++i) {
        alloc.deallocate(ptrs[i], 256);
    }

    // Oversized allocation falls through to system allocator.
    constexpr std::size_t kOver = nd::CoroFrameAllocator::kFrameSize + 1;
    void* p_over = alloc.allocate(kOver);
    EXPECT(p_over != nullptr);
    alloc.deallocate(p_over, kOver);
}

// ─────────────────────────────────────────────────────────────────────────────
// §12 — AwaitableTask<int>
// ─────────────────────────────────────────────────────────────────────────────
static nd::AwaitableTask<int> make_int_task() {
    co_return 42;
}

static void test_awaitable_task_int() {
    SECTION("Test 12: AwaitableTask<int> — co_return 42");

    auto task = make_int_task();
    EXPECT(!task.handle().done());
    task.handle().resume();
    EXPECT(task.handle().done());
    EXPECT(task.value() == 42);
}

// ─────────────────────────────────────────────────────────────────────────────
// §13 — AwaitableTask<void>
// ─────────────────────────────────────────────────────────────────────────────
static nd::AwaitableTask<void> make_void_task() {
    co_return;
}

static void test_awaitable_task_void() {
    SECTION("Test 13: AwaitableTask<void> — co_return void");

    auto task = make_void_task();
    EXPECT(!task.handle().done());
    task.handle().resume();
    EXPECT(task.handle().done());
}

// ─────────────────────────────────────────────────────────────────────────────
// §6 — Node helpers (category query booleans)
// ─────────────────────────────────────────────────────────────────────────────
static void test_node_category_helpers() {
    SECTION("Test 6: category query helpers — is_source_v, is_sink_v, etc.");

    EXPECT(nd::is_source_v<CS>);
    EXPECT(!nd::is_sink_v<CS>);
    EXPECT(!nd::is_transform_v<CS>);
    EXPECT(!nd::is_stateful_v<CS>);
    // CanonicalSource has tick() so it satisfies Actor too (tick is the GraphRuntime entry point).
    EXPECT(nd::is_actor_v<CS>);

    EXPECT(nd::is_source_v<CT>);
    EXPECT(nd::is_sink_v<CT>);
    EXPECT(nd::is_transform_v<CT>);
    EXPECT(nd::is_stateful_v<CT>);
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
int main() {
    std::puts("[node_types_test] phyriad_node pillar — Phase 1.A");
    std::puts("----------------------------------------------------------------");
    std::printf("  sizeof(Outlet<SampleTick>) = %zu bytes\n",
                sizeof(nd::Outlet<phyriad::schema::SampleTick>));
    std::printf("  sizeof(Inlet<SampleTick>)  = %zu bytes\n",
                sizeof(nd::Inlet<phyriad::schema::SampleTick>));
    std::printf("  sizeof(CanonicalSource)    = %zu bytes\n",
                sizeof(nd::canonical::CanonicalSource));
    std::printf("  sizeof(CanonicalTransform) = %zu bytes\n",
                sizeof(nd::canonical::CanonicalTransform));
    std::printf("  sizeof(CoroFrameAllocator) = %zu bytes (≈256 KB pool)\n",
                sizeof(nd::CoroFrameAllocator));
    std::puts("----------------------------------------------------------------");

    test_lifecycle_concepts();
    test_checkpointable();
    test_node_category_helpers();
    test_outlet_unconnected();
    test_inlet_unconnected();
    test_coro_frame_allocator();
    test_awaitable_task_int();
    test_awaitable_task_void();

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
