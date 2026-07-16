// framework/ui/tests/frame_interp_test.cpp
// FrameInterpolatorNode + PresentNode + RenderedFrame headless tests.
//
// Tests:
//   Test 1  — RenderedFrame: 40 bytes, alignas(8), trivially copyable.
//   Test 2  — RenderedFrame: handle_id defaults to UINT32_MAX (invalid).
//   Test 3  — RenderedFrame: is_interpolated defaults to 0.
//   Test 4  — FrameInterpolatorNode: default algorithm is LinearBlend.
//   Test 5  — FrameInterpolatorNode: on_start() succeeds (no Vulkan needed for PassThrough).
//   Test 6  — FrameInterpolatorNode: tick() on empty inlet is a no-op.
//   Test 7  — FrameInterpolatorNode: PassThrough forwards frame unchanged.
//   Test 8  — PresentNode: tick() on empty inlet is a no-op.
//   Test 9  — FramePool: acquire() returns UINT32_MAX when all slots in_use.
//   Test 10 — FramePool: release() then acquire() succeeds.
//
#include <phyriad/ui/types/RenderedFrame.hpp>
#include <phyriad/ui/FrameInterpolatorNode.hpp>
#include <phyriad/ui/PresentNode.hpp>
#ifdef PHYRIAD_BUILD_VULKAN
#   include <phyriad/render/vulkan/FramePool.hpp>
#endif
#include <cstdio>
#include <type_traits>

// ─────────────────────────────────────────────────────────────────────────────
// Compile-time assertions
// ─────────────────────────────────────────────────────────────────────────────
static_assert(sizeof(phyriad::ui::RenderedFrame)  == 40u,
    "RenderedFrame must be 40 bytes");
static_assert(alignof(phyriad::ui::RenderedFrame) == 8u,
    "RenderedFrame must be 8-byte aligned");
static_assert(std::is_trivially_copyable_v<phyriad::ui::RenderedFrame>,
    "RenderedFrame must be trivially copyable");
static_assert(std::is_standard_layout_v<phyriad::ui::RenderedFrame>,
    "RenderedFrame must be standard layout");

// ─────────────────────────────────────────────────────────────────────────────
// Test harness
// ─────────────────────────────────────────────────────────────────────────────
static int g_run    = 0;
static int g_passed = 0;
static int g_failed = 0;

#define TEST_BEGIN(name) \
    do { ++g_run; std::printf("[TEST] %-60s ", name); std::fflush(stdout); } while(0)
#define TEST_OK() \
    do { ++g_passed; std::printf("PASS\n"); } while(0)
#define TEST_FAIL(msg) \
    do { ++g_failed;  std::printf("FAIL — %s\n", msg); } while(0)

// ─────────────────────────────────────────────────────────────────────────────
// Test 1 — RenderedFrame layout
// ─────────────────────────────────────────────────────────────────────────────
static void test_rendered_frame_layout()
{
    TEST_BEGIN("RenderedFrame: 40 bytes, alignas(8), trivially copyable");
    if (sizeof(phyriad::ui::RenderedFrame)  == 40u &&
        alignof(phyriad::ui::RenderedFrame) == 8u  &&
        std::is_trivially_copyable_v<phyriad::ui::RenderedFrame>)
        TEST_OK();
    else
        TEST_FAIL("layout violated");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2 — RenderedFrame defaults
// ─────────────────────────────────────────────────────────────────────────────
static void test_rendered_frame_defaults()
{
    TEST_BEGIN("RenderedFrame: handle_id defaults to UINT32_MAX");
    phyriad::ui::RenderedFrame rf{};
    if (rf.handle_id == UINT32_MAX) TEST_OK();
    else TEST_FAIL("handle_id should default to UINT32_MAX");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 3 — is_interpolated defaults to 0
// ─────────────────────────────────────────────────────────────────────────────
static void test_is_interpolated_default()
{
    TEST_BEGIN("RenderedFrame: is_interpolated defaults to 0");
    phyriad::ui::RenderedFrame rf{};
    if (rf.is_interpolated == 0u) TEST_OK();
    else TEST_FAIL("is_interpolated should default to 0");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 4 — FrameInterpolatorNode default algorithm
// ─────────────────────────────────────────────────────────────────────────────
static void test_frame_interp_default_algo()
{
    TEST_BEGIN("FrameInterpolatorNode: default algorithm is LinearBlend");
    phyriad::ui::FrameInterpolatorNode node;
    if (node.algorithm() == phyriad::ui::FrameInterpolatorNode::Algorithm::LinearBlend)
        TEST_OK();
    else
        TEST_FAIL("default algorithm should be LinearBlend");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 5 — on_start() succeeds in PassThrough mode
// ─────────────────────────────────────────────────────────────────────────────
static void test_frame_interp_on_start()
{
    TEST_BEGIN("FrameInterpolatorNode: on_start() succeeds (PassThrough mode)");
    phyriad::ui::FrameInterpolatorNode node(
        phyriad::ui::FrameInterpolatorNode::Algorithm::PassThrough);
    auto r = node.on_start();
    if (r.has_value()) TEST_OK();
    else TEST_FAIL("on_start() returned error");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 6 — tick() on empty inlet is a no-op
// ─────────────────────────────────────────────────────────────────────────────
static void test_frame_interp_tick_empty()
{
    TEST_BEGIN("FrameInterpolatorNode: tick() on empty inlet → no crash");
    phyriad::ui::FrameInterpolatorNode node(
        phyriad::ui::FrameInterpolatorNode::Algorithm::PassThrough);
    (void)node.on_start();
    auto r = node.tick();
    if (r.has_value()) TEST_OK();
    else TEST_FAIL("tick() returned error on empty inlet");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 7 — PassThrough: set_algorithm changes algorithm
// ─────────────────────────────────────────────────────────────────────────────
static void test_frame_interp_set_algorithm()
{
    TEST_BEGIN("FrameInterpolatorNode: set_algorithm changes algorithm");
    phyriad::ui::FrameInterpolatorNode node;
    node.set_algorithm(phyriad::ui::FrameInterpolatorNode::Algorithm::PassThrough);
    if (node.algorithm() == phyriad::ui::FrameInterpolatorNode::Algorithm::PassThrough)
        TEST_OK();
    else
        TEST_FAIL("set_algorithm had no effect");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 8 — PresentNode: tick() on empty inlet is a no-op
// ─────────────────────────────────────────────────────────────────────────────
static void test_present_node_tick_empty()
{
    TEST_BEGIN("PresentNode: tick() on empty inlet → no crash");
    phyriad::ui::PresentNode node;
    (void)node.on_start();
    auto r = node.tick();
    if (r.has_value()) TEST_OK();
    else TEST_FAIL("PresentNode::tick() returned error");
}

#ifdef PHYRIAD_BUILD_VULKAN
// ─────────────────────────────────────────────────────────────────────────────
// Test 9 — FramePool: acquire() returns UINT32_MAX when pool exhausted
// ─────────────────────────────────────────────────────────────────────────────
static void test_frame_pool_exhaustion()
{
    TEST_BEGIN("FramePool: acquire() returns UINT32_MAX when all slots in_use");

    phyriad::render::vulkan::FramePool pool;

    // Manually mark all slots as in_use by acquiring them.
    uint32_t acquired[phyriad::render::vulkan::FramePool::kPoolSize];
    uint32_t n_acquired = 0u;
    for (uint32_t i = 0u; i < phyriad::render::vulkan::FramePool::kPoolSize; ++i) {
        uint32_t idx = pool.acquire();
        if (idx != UINT32_MAX)
            acquired[n_acquired++] = idx;
    }

    // Now pool should be exhausted.
    const uint32_t extra = pool.acquire();
    if (extra == UINT32_MAX) {
        TEST_OK();
    } else {
        TEST_FAIL("expected UINT32_MAX — pool should be exhausted");
    }

    // Release all acquired slots.
    for (uint32_t i = 0u; i < n_acquired; ++i)
        pool.release(acquired[i]);
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 10 — FramePool: release() then acquire() succeeds
// ─────────────────────────────────────────────────────────────────────────────
static void test_frame_pool_release_reacquire()
{
    TEST_BEGIN("FramePool: release() then acquire() succeeds");

    phyriad::render::vulkan::FramePool pool;

    uint32_t idx = pool.acquire();
    if (idx == UINT32_MAX) { TEST_FAIL("initial acquire() failed"); return; }

    pool.release(idx);
    uint32_t idx2 = pool.acquire();
    if (idx2 != UINT32_MAX)
        TEST_OK();
    else
        TEST_FAIL("acquire() after release() failed");

    pool.release(idx2);
}
#endif // PHYRIAD_BUILD_VULKAN

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
int main()
{
    std::printf("=== phyriad_ui FrameInterpolatorNode / PresentNode tests ===\n\n");

    test_rendered_frame_layout();
    test_rendered_frame_defaults();
    test_is_interpolated_default();
    test_frame_interp_default_algo();
    test_frame_interp_on_start();
    test_frame_interp_tick_empty();
    test_frame_interp_set_algorithm();
    test_present_node_tick_empty();
#ifdef PHYRIAD_BUILD_VULKAN
    test_frame_pool_exhaustion();
    test_frame_pool_release_reacquire();
#endif

    std::printf("\n--- Results: %d/%d passed", g_passed, g_run);
    if (g_failed > 0) std::printf(", %d FAILED", g_failed);
    std::printf(" ---\n");

    return (g_failed == 0) ? 0 : 1;
}
// Made with my soul - Swately <3
