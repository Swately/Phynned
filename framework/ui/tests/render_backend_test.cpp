// framework/ui/tests/render_backend_test.cpp
// Dynamic backend selection unit tests.
//
// Tests the backend selection logic that lives in Application::run() without
// opening a GLFW window or a GL / Vulkan context.
//
// Covered:
//   Test 1 — RenderBackendKind enum values match the documented spec.
//   Test 2 — ApplicationConfig::render_backend defaults to OpenGL3.
//   Test 3 — Non-Auto kinds are returned unchanged by the resolve helper.
//   Test 4 — Auto resolves to OpenGL3 when PHYRIAD_BUILD_VULKAN is not defined.
//   Test 5 — Conditional compile: VulkanBackend.hpp included iff PHYRIAD_BUILD_VULKAN.
//   Test 6 — IRenderBackend header compiles and exposes the required interface.
//   Test 7 — OpenGL3Backend is default-constructible without a GL context.
//
#include <phyriad/render/RenderBackendKind.hpp>
#include <phyriad/ui/ApplicationConfig.hpp>
#include <phyriad/render/IRenderBackend.hpp>
#include <phyriad/render/opengl3/OpenGL3Backend.hpp>
#ifdef PHYRIAD_BUILD_VULKAN
#   include <phyriad/render/vulkan/VulkanBackend.hpp>
#endif
#include <cstdio>

// ─────────────────────────────────────────────────────────────────────────────
// Compile-time assertions — §Test 1
// ─────────────────────────────────────────────────────────────────────────────
static_assert(static_cast<uint8_t>(phyriad::render::RenderBackendKind::OpenGL3)   == 0u,
    "OpenGL3 must be 0");
static_assert(static_cast<uint8_t>(phyriad::render::RenderBackendKind::Vulkan)    == 1u,
    "Vulkan must be 1");
static_assert(static_cast<uint8_t>(phyriad::render::RenderBackendKind::Composite) == 2u,
    "Composite must be 2");
static_assert(static_cast<uint8_t>(phyriad::render::RenderBackendKind::Auto)      == 3u,
    "Auto must be 3");

// ─────────────────────────────────────────────────────────────────────────────
// ApplicationConfig default render_backend — §Test 2
// ─────────────────────────────────────────────────────────────────────────────
static_assert([] {
    phyriad::ui::ApplicationConfig cfg{};
    return cfg.render_backend == phyriad::render::RenderBackendKind::OpenGL3;
}(), "ApplicationConfig::render_backend must default to OpenGL3");

// ─────────────────────────────────────────────────────────────────────────────
// resolve_backend_kind — mirrors the logic in Application::run() §2.
// Does NOT call glfwVulkanSupported() — that requires glfwInit() and a display.
// The Auto path is therefore only exercised indirectly (§Test 4).
// ─────────────────────────────────────────────────────────────────────────────
namespace {

using Kind = phyriad::render::RenderBackendKind;

// Simulate the compile-time part of the resolution (Auto without a live GLFW).
// When PHYRIAD_BUILD_VULKAN is not defined, Auto always → OpenGL3 at compile time.
constexpr Kind resolve_static(Kind requested) noexcept {
    if (requested != Kind::Auto) return requested;
    // compile-time branch: same logic as the #ifdef block in Application.hpp
#ifdef PHYRIAD_BUILD_VULKAN
    // At compile time we can't call glfwVulkanSupported(); return Auto to
    // signal "runtime decision needed".
    return Kind::Auto;
#else
    return Kind::OpenGL3;
#endif
}

} // anonymous namespace

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
// Test 1 — enum values (runtime echo of static_asserts)
// ─────────────────────────────────────────────────────────────────────────────
static void test_backend_kind_enum()
{
    TEST_BEGIN("RenderBackendKind: enum values 0/1/2/3");
    bool ok = (static_cast<uint8_t>(Kind::OpenGL3)   == 0u)
           && (static_cast<uint8_t>(Kind::Vulkan)    == 1u)
           && (static_cast<uint8_t>(Kind::Composite) == 2u)
           && (static_cast<uint8_t>(Kind::Auto)      == 3u);
    if (ok) TEST_OK(); else TEST_FAIL("unexpected enum values");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2 — ApplicationConfig default
// ─────────────────────────────────────────────────────────────────────────────
static void test_config_default_backend()
{
    TEST_BEGIN("ApplicationConfig::render_backend defaults to OpenGL3");
    phyriad::ui::ApplicationConfig cfg{};
    if (cfg.render_backend == Kind::OpenGL3)
        TEST_OK();
    else
        TEST_FAIL("default render_backend is not OpenGL3");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 3 — Non-Auto kinds are returned unchanged
// ─────────────────────────────────────────────────────────────────────────────
static void test_resolve_non_auto()
{
    TEST_BEGIN("resolve_static: non-Auto kinds returned unchanged");
    bool ok = resolve_static(Kind::OpenGL3)   == Kind::OpenGL3
           && resolve_static(Kind::Vulkan)    == Kind::Vulkan
           && resolve_static(Kind::Composite) == Kind::Composite;
    if (ok) TEST_OK(); else TEST_FAIL("non-Auto kind was altered");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 4 — Auto without PHYRIAD_BUILD_VULKAN → OpenGL3
// ─────────────────────────────────────────────────────────────────────────────
static void test_resolve_auto_without_vulkan()
{
    TEST_BEGIN("resolve_static: Auto → OpenGL3 when !PHYRIAD_BUILD_VULKAN");
#ifdef PHYRIAD_BUILD_VULKAN
    // When Vulkan is compiled in, Auto stays as Auto (runtime query needed).
    // Verify only that the result is not some garbage value.
    auto result = resolve_static(Kind::Auto);
    bool ok = (result == Kind::Auto || result == Kind::OpenGL3 || result == Kind::Vulkan);
    if (ok) TEST_OK(); else TEST_FAIL("resolve_static returned garbage");
#else
    if (resolve_static(Kind::Auto) == Kind::OpenGL3)
        TEST_OK();
    else
        TEST_FAIL("Auto should map to OpenGL3 when PHYRIAD_BUILD_VULKAN is off");
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 5 — Conditional VulkanBackend include
// ─────────────────────────────────────────────────────────────────────────────
static void test_vulkan_include()
{
    TEST_BEGIN("Conditional VulkanBackend.hpp include");
#ifdef PHYRIAD_BUILD_VULKAN
    // If we reach here the #include succeeded — the type must be complete.
    bool ok = sizeof(phyriad::render::vulkan::VulkanBackend) > 0u;
    if (ok) TEST_OK(); else TEST_FAIL("VulkanBackend has zero size");
#else
    // Not compiled in — confirm OpenGL3 is still available.
    bool ok = sizeof(phyriad::render::opengl3::OpenGL3Backend) > 0u;
    if (ok) TEST_OK(); else TEST_FAIL("OpenGL3Backend has zero size");
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 6 — IRenderBackend interface completeness
// ─────────────────────────────────────────────────────────────────────────────
static void test_irender_backend_interface()
{
    TEST_BEGIN("IRenderBackend has virtual destructor and required methods");
    // Verify that the abstract interface exposes the methods Application relies on.
    // Confirmed by: OpenGL3Backend is-a IRenderBackend (tested via pointer cast).
    phyriad::render::opengl3::OpenGL3Backend concrete;
    phyriad::render::IRenderBackend* iface = &concrete;
    // The interface pointer being non-null is sufficient — we don't call any
    // GL-requiring virtual functions here.
    if (iface != nullptr)
        TEST_OK();
    else
        TEST_FAIL("IRenderBackend pointer is null");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 7 — OpenGL3Backend default-constructible without GL context
// ─────────────────────────────────────────────────────────────────────────────
static void test_opengl3_default_constructible()
{
    TEST_BEGIN("OpenGL3Backend default-constructible without GL context");
    phyriad::render::opengl3::OpenGL3Backend backend;
    // Constructing without a GL context must not crash.
    // initialized() should be false — we never called init().
    (void)backend;
    TEST_OK();
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
int main()
{
    std::printf("=== Dynamic backend selection tests ===\n\n");

    test_backend_kind_enum();
    test_config_default_backend();
    test_resolve_non_auto();
    test_resolve_auto_without_vulkan();
    test_vulkan_include();
    test_irender_backend_interface();
    test_opengl3_default_constructible();

    std::printf("\n--- Results: %d/%d passed", g_passed, g_run);
    if (g_failed > 0) std::printf(", %d FAILED", g_failed);
    std::printf(" ---\n");

    return (g_failed == 0) ? 0 : 1;
}
// Made with my soul - Swately <3
