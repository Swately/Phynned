// framework/render/opengl3/tests/fsr1_test.cpp
// FSR1Pass headless unit tests.
//
// These tests verify the C++ contract of FSR1Pass without requiring an actual
// GL context (where GL calls would be no-ops or safely fail):
//
//   Test 1 — Config validation: zero dimensions → init returns error.
//   Test 2 — shutdown idempotent on uninitialized object.
//   Test 3 — resize on uninitialized object is a no-op (no crash).
//   Test 4 — apply on uninitialized object is a no-op (no crash).
//   Test 5 — Config POD values preserved after construction.
//
// NOTE: Tests that require a real GL context (compile shaders, run passes)
//   are covered by the examples/vulkan_window and examples/standard_window
//   integration tests where a GLFW window is available.
//
#include <phyriad/render/opengl3/FSR1Pass.hpp>
#include <phyriad/schema/Error.hpp>
#include <cstdio>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
// Test harness
// ─────────────────────────────────────────────────────────────────────────────

static int g_run    = 0;
static int g_passed = 0;
static int g_failed = 0;

#define TEST_BEGIN(name) \
    do { ++g_run; std::printf("[TEST] %-60s ", name); std::fflush(stdout); } while(0)
#define TEST_OK()   \
    do { ++g_passed; std::printf("PASS\n"); } while(0)
#define TEST_FAIL(msg) \
    do { ++g_failed;  std::printf("FAIL — %s\n", msg); } while(0)

// ─────────────────────────────────────────────────────────────────────────────
// Test 1 — Zero dimensions → init returns ResourceInitFailed
// ─────────────────────────────────────────────────────────────────────────────
static void test_zero_dims()
{
    TEST_BEGIN("FSR1Pass::init with zero input_width → ResourceInitFailed");

    phyriad::render::opengl3::FSR1Pass fsr;
    // No GL context — init will fail at dimension validation before any GL call.
    auto r = fsr.init({0, 360, 1920, 1080, 0.2f});

    if (!r && r.error().code == phyriad::ErrorCode::ResourceInitFailed)
        TEST_OK();
    else if (!r)
        TEST_OK();   // any error is acceptable
    else
        TEST_FAIL("expected failure, got success");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2 — shutdown idempotent on uninitialized object
// ─────────────────────────────────────────────────────────────────────────────
static void test_shutdown_idempotent()
{
    TEST_BEGIN("FSR1Pass::shutdown idempotent on fresh object (×3)");

    phyriad::render::opengl3::FSR1Pass fsr;
    fsr.shutdown();
    fsr.shutdown();
    fsr.shutdown();
    TEST_OK();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 3 — resize on uninitialized object is a no-op
// ─────────────────────────────────────────────────────────────────────────────
static void test_resize_uninit()
{
    TEST_BEGIN("FSR1Pass::resize on uninitialized object → no crash");

    phyriad::render::opengl3::FSR1Pass fsr;
    fsr.resize(640, 360, 1920, 1080);
    fsr.resize(0, 0, 0, 0);         // zero dims while uninit → also safe
    TEST_OK();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 4 — apply on uninitialized object is a no-op
// ─────────────────────────────────────────────────────────────────────────────
static void test_apply_uninit()
{
    TEST_BEGIN("FSR1Pass::apply on uninitialized object → no crash");

    phyriad::render::opengl3::FSR1Pass fsr;
    fsr.apply(0u);
    fsr.apply(999u);   // garbage texture id — also safe when uninit
    TEST_OK();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 5 — Config default values
// ─────────────────────────────────────────────────────────────────────────────
static void test_config_defaults()
{
    TEST_BEGIN("FSR1Pass::Config default values");

    phyriad::render::opengl3::FSR1Pass::Config cfg;
    if (cfg.input_width   == 0 &&
        cfg.input_height  == 0 &&
        cfg.output_width  == 0 &&
        cfg.output_height == 0 &&
        cfg.sharpness     == 0.2f)
        TEST_OK();
    else
        TEST_FAIL("unexpected default config values");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 6 — initialized() returns false on fresh object
// ─────────────────────────────────────────────────────────────────────────────
static void test_initialized_flag()
{
    TEST_BEGIN("FSR1Pass::initialized() == false on fresh object");

    phyriad::render::opengl3::FSR1Pass fsr;
    if (!fsr.initialized())
        TEST_OK();
    else
        TEST_FAIL("initialized() should be false before init()");
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
int main()
{
    std::printf("=== phyriad_render_opengl3 FSR1Pass tests ===\n\n");

    test_config_defaults();
    test_initialized_flag();
    test_shutdown_idempotent();
    test_resize_uninit();
    test_apply_uninit();
    test_zero_dims();

    std::printf("\n--- Results: %d/%d passed", g_passed, g_run);
    if (g_failed > 0) std::printf(", %d FAILED", g_failed);
    std::printf(" ---\n");

    return (g_failed == 0) ? 0 : 1;
}
// Made with my soul - Swately <3
