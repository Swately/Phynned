// framework/render/composite/tests/composite_test.cpp
// CompositeBackend headless tests — no GPU / GLFW required.
//
// Tests:
//   Test 1 — CompositeBackend is-a IRenderBackend (upcast compiles).
//   Test 2 — Default state: interop_active() == false.
//   Test 3 — Default state: scene_initialized() == false.
//   Test 4 — shutdown() on uninitialised instance is a no-op (no crash).
//   Test 5 — init(nullptr, nullptr) returns an error.
//   Test 6 — resize() on uninitialised instance is a no-op.
//   Test 7 — new_frame() on uninitialised instance is a no-op.
//   Test 8 — end_frame() on uninitialised instance returns zero stats.
//   Test 9 — present() on uninitialised instance is a no-op.
//   Test 10 — Double shutdown() is idempotent (no crash, no double-free).
//
#include <phyriad/render/composite/CompositeBackend.hpp>
#include <phyriad/render/IRenderBackend.hpp>
#include <cstdio>
#include <type_traits>

// ─────────────────────────────────────────────────────────────────────────────
// Compile-time assertions
// ─────────────────────────────────────────────────────────────────────────────
static_assert(std::is_base_of_v<phyriad::render::IRenderBackend,
                                 phyriad::render::composite::CompositeBackend>,
    "CompositeBackend must derive from IRenderBackend");

static_assert(!std::is_copy_constructible_v<phyriad::render::composite::CompositeBackend>,
    "CompositeBackend must not be copy-constructible");
static_assert(!std::is_copy_assignable_v<phyriad::render::composite::CompositeBackend>,
    "CompositeBackend must not be copy-assignable");
static_assert(!std::is_move_constructible_v<phyriad::render::composite::CompositeBackend>,
    "CompositeBackend must not be move-constructible");

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
    do { ++g_failed;  std::printf("FAIL — %s\n", msg); } while(0)

using CB = phyriad::render::composite::CompositeBackend;

// ─────────────────────────────────────────────────────────────────────────────
// Test 1 — is-a IRenderBackend
// ─────────────────────────────────────────────────────────────────────────────
static void test_is_a_irender_backend()
{
    TEST_BEGIN("CompositeBackend: is-a IRenderBackend (upcast)");
    CB cb;
    phyriad::render::IRenderBackend* p = &cb;
    if (p != nullptr) TEST_OK();
    else TEST_FAIL("upcast returned nullptr");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2 — interop_active() defaults to false
// ─────────────────────────────────────────────────────────────────────────────
static void test_interop_active_default()
{
    TEST_BEGIN("CompositeBackend: interop_active() defaults to false");
    CB cb;
    if (!cb.interop_active()) TEST_OK();
    else TEST_FAIL("interop_active() should be false before init()");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 3 — scene_initialized() defaults to false
// ─────────────────────────────────────────────────────────────────────────────
static void test_scene_initialized_default()
{
    TEST_BEGIN("CompositeBackend: scene_initialized() defaults to false");
    CB cb;
    if (!cb.scene_initialized()) TEST_OK();
    else TEST_FAIL("scene_initialized() should be false before init()");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 4 — shutdown() on uninitialised instance is a no-op
// ─────────────────────────────────────────────────────────────────────────────
static void test_shutdown_uninitialised()
{
    TEST_BEGIN("CompositeBackend: shutdown() uninitialised → no crash");
    CB cb;
    cb.shutdown();  // must not crash
    TEST_OK();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 5 — init(nullptr, nullptr) returns an error
// ─────────────────────────────────────────────────────────────────────────────
static void test_init_null_window()
{
    TEST_BEGIN("CompositeBackend: init(nullptr, nullptr) returns error");
    CB cb;
    auto r = cb.init(nullptr, nullptr);
    if (!r.has_value()) TEST_OK();
    else TEST_FAIL("expected error with null window");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 6 — resize() on uninitialised instance is a no-op
// ─────────────────────────────────────────────────────────────────────────────
static void test_resize_uninitialised()
{
    TEST_BEGIN("CompositeBackend: resize() uninitialised → no crash");
    CB cb;
    cb.resize(1920u, 1080u);
    TEST_OK();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 7 — new_frame() on uninitialised instance is a no-op
// ─────────────────────────────────────────────────────────────────────────────
static void test_new_frame_uninitialised()
{
    TEST_BEGIN("CompositeBackend: new_frame() uninitialised → no crash");
    CB cb;
    cb.new_frame();
    TEST_OK();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 8 — end_frame() on uninitialised instance returns zero stats
// ─────────────────────────────────────────────────────────────────────────────
static void test_end_frame_uninitialised()
{
    TEST_BEGIN("CompositeBackend: end_frame() uninitialised → zero stats");
    CB cb;
    phyriad::render::RenderStats stats = cb.end_frame();
    // All fields should be zero (or at least the call must not crash).
    if (stats.frame_id == 0u) TEST_OK();
    else TEST_FAIL("expected zero frame_id in default stats");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 9 — present() on uninitialised instance is a no-op
// ─────────────────────────────────────────────────────────────────────────────
static void test_present_uninitialised()
{
    TEST_BEGIN("CompositeBackend: present() uninitialised → no crash");
    CB cb;
    cb.present();
    TEST_OK();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 10 — Double shutdown() is idempotent
// ─────────────────────────────────────────────────────────────────────────────
static void test_double_shutdown()
{
    TEST_BEGIN("CompositeBackend: double shutdown() is idempotent");
    CB cb;
    cb.shutdown();
    cb.shutdown();   // second call must not crash or double-free
    TEST_OK();
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
int main()
{
    std::printf("=== phyriad_render_composite CompositeBackend tests ===\n\n");

    test_is_a_irender_backend();
    test_interop_active_default();
    test_scene_initialized_default();
    test_shutdown_uninitialised();
    test_init_null_window();
    test_resize_uninitialised();
    test_new_frame_uninitialised();
    test_end_frame_uninitialised();
    test_present_uninitialised();
    test_double_shutdown();

    std::printf("\n--- Results: %d/%d passed", g_passed, g_run);
    if (g_failed > 0) std::printf(", %d FAILED", g_failed);
    std::printf(" ---\n");

    return (g_failed == 0) ? 0 : 1;
}
// Made with my soul - Swately <3
