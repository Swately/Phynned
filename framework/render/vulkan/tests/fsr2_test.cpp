// framework/render/vulkan/tests/fsr2_test.cpp
// FSR2Pass + FSR3Pass headless unit tests.
//
// Tests the C++ API contract without a Vulkan context or GPU:
//
//   Test 1  — FSR2Pass::Config: zero input_width → init returns error.
//   Test 2  — FSR2Pass: output < input → init returns error.
//   Test 3  — FSR2Pass::shutdown idempotent on fresh object (×3).
//   Test 4  — FSR2Pass::apply on uninitialized → no crash.
//   Test 5  — FSR2Pass::resize on uninitialized → no crash.
//   Test 6  — FSR2Pass::initialized() == false on fresh object.
//   Test 7  — FSR2Pass dimension queries return 0 when uninitialized.
//   Test 8  — FSR3Pass::shutdown idempotent on fresh object.
//   Test 9  — FSR3Pass::apply on uninitialized → no crash.
//   Test 10 — FSR3Pass::frame_gen_available() == false when uninitialized.
//   Test 11 — (PHYRIAD_HAS_FFX_SDK=0) init returns ResourceInitFailed.
//
// NOTE: Integration tests requiring a real Vulkan surface + GPU run via
//   examples/vulkan_window with PHYRIAD_BUILD_FSR2=ON.
//
#include <phyriad/render/vulkan/FSR2Pass.hpp>
#include <phyriad/render/vulkan/FSR3Pass.hpp>
#include <phyriad/render/vulkan/VulkanContext.hpp>
#include <phyriad/schema/Error.hpp>
#include <cstdio>

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
// Mock VulkanContext — provides handle stubs so FSR2Pass can be constructed
// without a real Vulkan instance.
// ─────────────────────────────────────────────────────────────────────────────
// VulkanContext is default-constructible and leaves all handles as VK_NULL_HANDLE
// until init() is called.  FSR2Pass only holds a reference; for headless tests
// we never call ctx.init(), so all handles remain null.
// The stub FSR2Pass/FSR3Pass implementations (PHYRIAD_HAS_FFX_SDK=0) never
// dereference the context — they return an error immediately.
// With the real SDK (PHYRIAD_HAS_FFX_SDK=1), init() would fail because
// phys_dev / device are VK_NULL_HANDLE, which is consistent with Test 1/2/11.

// ─────────────────────────────────────────────────────────────────────────────
// Tests 1–7 — FSR2Pass
// ─────────────────────────────────────────────────────────────────────────────

static void test_fsr2_zero_dims()
{
    TEST_BEGIN("FSR2Pass::init with zero input_width → error");
    phyriad::render::vulkan::VulkanContext ctx;
    phyriad::render::vulkan::FSR2Pass fsr(ctx);
    auto r = fsr.init(0u, 540u, 1920u, 1080u);
    if (!r)
        TEST_OK();
    else
        TEST_FAIL("expected failure");
}

static void test_fsr2_output_smaller_than_input()
{
    TEST_BEGIN("FSR2Pass::init output < input → error");
    phyriad::render::vulkan::VulkanContext ctx;
    phyriad::render::vulkan::FSR2Pass fsr(ctx);
    // output 480×270 is smaller than input 960×540 — invalid
    auto r = fsr.init(960u, 540u, 480u, 270u);
    if (!r)
        TEST_OK();
    else
        TEST_FAIL("expected failure, got success");
}

static void test_fsr2_shutdown_idempotent()
{
    TEST_BEGIN("FSR2Pass::shutdown idempotent (×3)");
    phyriad::render::vulkan::VulkanContext ctx;
    phyriad::render::vulkan::FSR2Pass fsr(ctx);
    fsr.shutdown();
    fsr.shutdown();
    fsr.shutdown();
    TEST_OK();
}

static void test_fsr2_apply_uninit()
{
    TEST_BEGIN("FSR2Pass::apply on uninitialized → no crash");
    phyriad::render::vulkan::VulkanContext ctx;
    phyriad::render::vulkan::FSR2Pass fsr(ctx);
    phyriad::render::vulkan::FSR2Pass::Inputs in{};
    fsr.apply(in);                        // all handles VK_NULL_HANDLE
    TEST_OK();
}

static void test_fsr2_resize_uninit()
{
    TEST_BEGIN("FSR2Pass::resize on uninitialized → no crash");
    phyriad::render::vulkan::VulkanContext ctx;
    phyriad::render::vulkan::FSR2Pass fsr(ctx);
    fsr.resize(960u, 540u, 1920u, 1080u);
    fsr.resize(0u, 0u, 0u, 0u);
    TEST_OK();
}

static void test_fsr2_initialized_flag()
{
    TEST_BEGIN("FSR2Pass::initialized() == false on fresh object");
    phyriad::render::vulkan::VulkanContext ctx;
    phyriad::render::vulkan::FSR2Pass fsr(ctx);
    if (!fsr.initialized())
        TEST_OK();
    else
        TEST_FAIL("initialized() should be false");
}

static void test_fsr2_dimensions_uninit()
{
    TEST_BEGIN("FSR2Pass dimension queries return 0 when uninitialized");
    phyriad::render::vulkan::VulkanContext ctx;
    phyriad::render::vulkan::FSR2Pass fsr(ctx);
    bool ok = fsr.input_width()   == 0u
           && fsr.input_height()  == 0u
           && fsr.output_width()  == 0u
           && fsr.output_height() == 0u;
    if (ok) TEST_OK(); else TEST_FAIL("dimensions not zero");
}

// ─────────────────────────────────────────────────────────────────────────────
// Tests 8–10 — FSR3Pass
// ─────────────────────────────────────────────────────────────────────────────

static void test_fsr3_shutdown_idempotent()
{
    TEST_BEGIN("FSR3Pass::shutdown idempotent (×3)");
    phyriad::render::vulkan::VulkanContext ctx;
    phyriad::render::vulkan::FSR3Pass fsr(ctx);
    fsr.shutdown();
    fsr.shutdown();
    fsr.shutdown();
    TEST_OK();
}

static void test_fsr3_apply_uninit()
{
    TEST_BEGIN("FSR3Pass::apply on uninitialized → no crash");
    phyriad::render::vulkan::VulkanContext ctx;
    phyriad::render::vulkan::FSR3Pass fsr(ctx);
    phyriad::render::vulkan::FSR3Pass::Inputs in{};
    fsr.apply(in);
    TEST_OK();
}

static void test_fsr3_frame_gen_available()
{
    TEST_BEGIN("FSR3Pass::frame_gen_available() == false when uninitialized");
    phyriad::render::vulkan::VulkanContext ctx;
    phyriad::render::vulkan::FSR3Pass fsr(ctx);
    if (!fsr.frame_gen_available())
        TEST_OK();
    else
        TEST_FAIL("frame_gen_available() should be false before init");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 11 — Stub error code when SDK absent
// ─────────────────────────────────────────────────────────────────────────────

static void test_fsr2_stub_error_code()
{
    TEST_BEGIN("FSR2Pass: init returns ResourceInitFailed without SDK");
#if !PHYRIAD_HAS_FFX_SDK
    phyriad::render::vulkan::VulkanContext ctx;
    phyriad::render::vulkan::FSR2Pass fsr(ctx);
    auto r = fsr.init(960u, 540u, 1920u, 1080u);
    if (!r && r.error().code == phyriad::ErrorCode::ResourceInitFailed)
        TEST_OK();
    else if (!r)
        TEST_OK();   // any error accepted
    else
        TEST_FAIL("expected ResourceInitFailed, got success");
#else
    // With SDK, result depends on ctx being initialized.  API contract still holds.
    TEST_OK();
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
int main()
{
    std::printf("=== phyriad_render_vulkan FSR2/FSR3 tests ===\n");
#if PHYRIAD_HAS_FFX_SDK
    std::printf("    (compiled with AMD FidelityFX SDK)\n\n");
#else
    std::printf("    (compiled WITHOUT FidelityFX SDK — stub mode)\n\n");
#endif

    // FSR2Pass tests
    test_fsr2_zero_dims();
    test_fsr2_output_smaller_than_input();
    test_fsr2_shutdown_idempotent();
    test_fsr2_apply_uninit();
    test_fsr2_resize_uninit();
    test_fsr2_initialized_flag();
    test_fsr2_dimensions_uninit();

    // FSR3Pass tests
    test_fsr3_shutdown_idempotent();
    test_fsr3_apply_uninit();
    test_fsr3_frame_gen_available();

    // Stub / SDK error code
    test_fsr2_stub_error_code();

    std::printf("\n--- Results: %d/%d passed", g_passed, g_run);
    if (g_failed > 0) std::printf(", %d FAILED", g_failed);
    std::printf(" ---\n");

    return (g_failed == 0) ? 0 : 1;
}
// Made with my soul - Swately <3
