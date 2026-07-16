// framework/render/vulkan/tests/gpu_telemetry_test.cpp
// GpuTelemetry + GpuMetrics headless tests — no GPU / Vulkan device required.
//
// Tests:
//   Test 1  — GpuMetrics: 32 bytes, alignas(8), trivially copyable.
//   Test 2  — GpuMetrics: all fields zero-initialised by default.
//   Test 3  — GpuMetrics: standard layout.
//   Test 4  — GpuTelemetry: initialized() defaults to false.
//   Test 5  — GpuTelemetry: timestamp_period_ns() defaults to 1.0f.
//   Test 6  — GpuTelemetry: not copy-constructible.
//   Test 7  — GpuTelemetry: shutdown(VK_NULL_HANDLE) uninitialised — no crash.
//   Test 8  — GpuTelemetry: begin_frame(VK_NULL_HANDLE) uninitialised — no crash.
//   Test 9  — GpuTelemetry: end_frame(VK_NULL_HANDLE) uninitialised — no crash.
//   Test 10 — GpuTelemetry: read_results uninitialised returns zero metrics.
//   Test 11 — GpuMetrics: pending_queue_depth is at offset 16.
//   Test 12 — GpuMetrics: vrr_active is at offset 24.
//
#include <phyriad/render/vulkan/GpuTelemetry.hpp>
#include <phyriad/render/GpuMetrics.hpp>
#include <phyriad/ui/types/GpuMetrics.hpp>  // phyriad::ui::GpuMetrics alias
#include <cstdio>
#include <cstddef>
#include <type_traits>

// ─────────────────────────────────────────────────────────────────────────────
// Compile-time assertions
// ─────────────────────────────────────────────────────────────────────────────
static_assert(sizeof(phyriad::ui::GpuMetrics)  == 32u,
    "GpuMetrics must be 32 bytes");
static_assert(alignof(phyriad::ui::GpuMetrics) == 8u,
    "GpuMetrics must be 8-byte aligned");
static_assert(std::is_trivially_copyable_v<phyriad::ui::GpuMetrics>,
    "GpuMetrics must be trivially copyable");
static_assert(std::is_standard_layout_v<phyriad::ui::GpuMetrics>,
    "GpuMetrics must be standard layout");

// Field offsets
static_assert(offsetof(phyriad::ui::GpuMetrics, gpu_frame_time_ns)         ==  0u);
static_assert(offsetof(phyriad::ui::GpuMetrics, queue_submit_ns)           ==  8u);
static_assert(offsetof(phyriad::ui::GpuMetrics, pending_queue_depth)       == 16u);
static_assert(offsetof(phyriad::ui::GpuMetrics, async_compute_overlap_pct) == 20u);
static_assert(offsetof(phyriad::ui::GpuMetrics, vrr_active)                == 24u);

static_assert(!std::is_copy_constructible_v<phyriad::render::vulkan::GpuTelemetry>,
    "GpuTelemetry must not be copy-constructible");
static_assert(!std::is_move_constructible_v<phyriad::render::vulkan::GpuTelemetry>,
    "GpuTelemetry must not be move-constructible");

// ─────────────────────────────────────────────────────────────────────────────
// Harness
// ─────────────────────────────────────────────────────────────────────────────
static int g_run    = 0;
static int g_passed = 0;
static int g_failed = 0;

#define TEST_BEGIN(name) \
    do { ++g_run; std::printf("[TEST] %-60s ", name); std::fflush(stdout); } while(0)
#define TEST_OK() \
    do { ++g_passed; std::printf("PASS\n"); } while(0)
#define TEST_FAIL(msg) \
    do { ++g_failed; std::printf("FAIL — %s\n", msg); } while(0)

using GM  = phyriad::render::GpuMetrics;
using GT  = phyriad::render::vulkan::GpuTelemetry;

// ─────────────────────────────────────────────────────────────────────────────
// Test 1 — GpuMetrics layout
// ─────────────────────────────────────────────────────────────────────────────
static void test_gpu_metrics_layout()
{
    TEST_BEGIN("GpuMetrics: 32 bytes, alignas(8), trivially copyable");
    if (sizeof(GM) == 32u &&
        alignof(GM) == 8u  &&
        std::is_trivially_copyable_v<GM>)
        TEST_OK();
    else
        TEST_FAIL("layout violated");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2 — GpuMetrics zero defaults
// ─────────────────────────────────────────────────────────────────────────────
static void test_gpu_metrics_defaults()
{
    TEST_BEGIN("GpuMetrics: all fields zero-initialised by default");
    GM m{};
    if (m.gpu_frame_time_ns == 0u &&
        m.queue_submit_ns   == 0u &&
        m.pending_queue_depth == 0u &&
        m.async_compute_overlap_pct == 0u &&
        m.vrr_active == 0u)
        TEST_OK();
    else
        TEST_FAIL("non-zero default field detected");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 3 — GpuMetrics standard layout
// ─────────────────────────────────────────────────────────────────────────────
static void test_gpu_metrics_standard_layout()
{
    TEST_BEGIN("GpuMetrics: standard layout");
    if (std::is_standard_layout_v<GM>) TEST_OK();
    else TEST_FAIL("not standard layout");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 4 — GpuTelemetry defaults
// ─────────────────────────────────────────────────────────────────────────────
static void test_gpu_telemetry_defaults()
{
    TEST_BEGIN("GpuTelemetry: initialized() defaults to false");
    GT gt;
    if (!gt.initialized()) TEST_OK();
    else TEST_FAIL("should not be initialized before init()");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 5 — timestamp_period_ns defaults to 1.0f
// ─────────────────────────────────────────────────────────────────────────────
static void test_timestamp_period_default()
{
    TEST_BEGIN("GpuTelemetry: timestamp_period_ns() defaults to 1.0f");
    GT gt;
    if (gt.timestamp_period_ns() == 1.0f) TEST_OK();
    else TEST_FAIL("expected 1.0f default");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 6 — not copy-constructible
// ─────────────────────────────────────────────────────────────────────────────
static void test_gpu_telemetry_not_copyable()
{
    TEST_BEGIN("GpuTelemetry: not copy-constructible (compile-time check)");
    // Verified by static_assert above; just confirm with runtime pass.
    if (!std::is_copy_constructible_v<GT>) TEST_OK();
    else TEST_FAIL("should not be copy-constructible");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 7 — shutdown() uninitialised is a no-op
// ─────────────────────────────────────────────────────────────────────────────
static void test_shutdown_uninitialised()
{
    TEST_BEGIN("GpuTelemetry: shutdown(VK_NULL_HANDLE) uninitialised → no crash");
    GT gt;
#ifdef PHYRIAD_BUILD_VULKAN
    gt.shutdown(VK_NULL_HANDLE);
#else
    gt.shutdown();
#endif
    TEST_OK();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 8 — begin_frame() uninitialised is a no-op
// ─────────────────────────────────────────────────────────────────────────────
static void test_begin_frame_uninitialised()
{
    TEST_BEGIN("GpuTelemetry: begin_frame(VK_NULL_HANDLE) uninitialised → no crash");
    GT gt;
#ifdef PHYRIAD_BUILD_VULKAN
    gt.begin_frame(VK_NULL_HANDLE);
#else
    gt.begin_frame(nullptr);
#endif
    TEST_OK();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 9 — end_frame() uninitialised is a no-op
// ─────────────────────────────────────────────────────────────────────────────
static void test_end_frame_uninitialised()
{
    TEST_BEGIN("GpuTelemetry: end_frame(VK_NULL_HANDLE) uninitialised → no crash");
    GT gt;
#ifdef PHYRIAD_BUILD_VULKAN
    gt.end_frame(VK_NULL_HANDLE);
#else
    gt.end_frame(nullptr);
#endif
    TEST_OK();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 10 — read_results() uninitialised returns zero metrics
// ─────────────────────────────────────────────────────────────────────────────
static void test_read_results_uninitialised()
{
    TEST_BEGIN("GpuTelemetry: read_results uninitialised → zero gpu_frame_time_ns");
    GT gt;
    GM m{};
#ifdef PHYRIAD_BUILD_VULKAN
    m = gt.read_results(VK_NULL_HANDLE, 0u);
#else
    m = gt.read_results(0u);
#endif
    if (m.gpu_frame_time_ns == 0u) TEST_OK();
    else TEST_FAIL("expected zero gpu_frame_time_ns");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 11 — pending_queue_depth is at offset 16
// ─────────────────────────────────────────────────────────────────────────────
static void test_pending_queue_depth_offset()
{
    TEST_BEGIN("GpuMetrics: pending_queue_depth is at byte offset 16");
    if (offsetof(GM, pending_queue_depth) == 16u) TEST_OK();
    else TEST_FAIL("wrong offset");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 12 — vrr_active is at offset 24
// ─────────────────────────────────────────────────────────────────────────────
static void test_vrr_active_offset()
{
    TEST_BEGIN("GpuMetrics: vrr_active is at byte offset 24");
    if (offsetof(GM, vrr_active) == 24u) TEST_OK();
    else TEST_FAIL("wrong offset");
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
int main()
{
    std::printf("=== phyriad_render_vulkan GpuTelemetry / GpuMetrics tests ===\n\n");

    test_gpu_metrics_layout();
    test_gpu_metrics_defaults();
    test_gpu_metrics_standard_layout();
    test_gpu_telemetry_defaults();
    test_timestamp_period_default();
    test_gpu_telemetry_not_copyable();
    test_shutdown_uninitialised();
    test_begin_frame_uninitialised();
    test_end_frame_uninitialised();
    test_read_results_uninitialised();
    test_pending_queue_depth_offset();
    test_vrr_active_offset();

    std::printf("\n--- Results: %d/%d passed", g_passed, g_run);
    if (g_failed > 0) std::printf(", %d FAILED", g_failed);
    std::printf(" ---\n");

    return (g_failed == 0) ? 0 : 1;
}
// Made with my soul - Swately <3
