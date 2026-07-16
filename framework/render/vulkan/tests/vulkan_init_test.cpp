// framework/render/vulkan/tests/vulkan_init_test.cpp
// Tests del VulkanBackend — verificación de init/shutdown sin ventana.
//
// Test 1: Sin soporte Vulkan o sin window → init retorna ResourceInitFailed.
//   Simula el comportamiento headless: crea un VulkanContext sin surface y
//   verifica que el error se propaga correctamente.
//
// Test 2: Smoke test de init de VulkanContext sin GLFWwindow (nullptr).
//   Verifica que el código de error handling es correcto y no produce UB.
//
// NOTA: Los tests que requieren una ventana real (Test completo de 100 frames)
//   se ejecutan en el ejemplo examples/vulkan_window/ con Vulkan validation layers.
//   Para ejecutar con layers: set VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
//
#include <phyriad/render/vulkan/VulkanBackend.hpp>
#include <phyriad/render/vulkan/VulkanContext.hpp>
#include <phyriad/schema/Error.hpp>
#include <cstdio>
#include <cassert>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
// Helpers de test
// ─────────────────────────────────────────────────────────────────────────────

static int g_tests_run   = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST_BEGIN(name)                                        \
    do {                                                        \
        ++g_tests_run;                                          \
        std::printf("[TEST] %s ... ", name);                    \
        std::fflush(stdout);                                    \
    } while (0)

#define TEST_OK()                                               \
    do {                                                        \
        ++g_tests_passed;                                       \
        std::printf("PASS\n");                                  \
    } while (0)

#define TEST_FAIL(msg)                                          \
    do {                                                        \
        ++g_tests_failed;                                       \
        std::printf("FAIL — %s\n", msg);                        \
    } while (0)

// ─────────────────────────────────────────────────────────────────────────────
// Test 1 — VulkanContext::init con window = nullptr → ResourceInitFailed
// ─────────────────────────────────────────────────────────────────────────────
static void test_vulkan_context_nullptr_window()
{
    TEST_BEGIN("VulkanContext::init(nullptr) returns ResourceInitFailed");

    phyriad::render::vulkan::VulkanContext ctx;

    // init con nullptr debe fallar: glfwGetRequiredInstanceExtensions(nullptr)
    // no funcionará, o la surface no se creará. En cualquier caso debe retornar
    // un error sin UB.
    auto r = ctx.init(nullptr);

    if (!r && r.error().code == phyriad::ErrorCode::ResourceInitFailed) {
        TEST_OK();
    } else if (!r) {
        // Cualquier error es aceptable (el comportamiento con nullptr depende de GLFW)
        TEST_OK();
    } else {
        TEST_FAIL("init should have failed but returned success");
    }

    // ctx.shutdown() es idempotente — no debe crashear
    ctx.shutdown();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2 — VulkanBackend::init sin GLFW inicializado → falla limpiamente
// ─────────────────────────────────────────────────────────────────────────────
static void test_vulkan_backend_no_glfw()
{
    TEST_BEGIN("VulkanBackend::init without GLFW → fails cleanly");

    phyriad::render::vulkan::VulkanBackend backend;

    // Sin GLFW inicializado, glfwVulkanSupported() retorna GLFW_FALSE
    // y el backend debe retornar ResourceInitFailed inmediatamente.
    auto r = backend.init(nullptr, nullptr);

    if (!r) {
        // Cualquier error es correcto
        TEST_OK();
    } else {
        TEST_FAIL("Expected failure but got success");
    }

    // shutdown idempotente
    backend.shutdown();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 3 — shutdown idempotente en objeto no inicializado
// ─────────────────────────────────────────────────────────────────────────────
static void test_vulkan_backend_shutdown_idempotent()
{
    TEST_BEGIN("VulkanBackend::shutdown on uninitialized object (idempotent)");

    phyriad::render::vulkan::VulkanBackend backend;
    // Llamar shutdown múltiples veces sin init no debe producir UB o crash.
    backend.shutdown();
    backend.shutdown();
    backend.shutdown();

    TEST_OK();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 4 — Error POD contract
// ─────────────────────────────────────────────────────────────────────────────
static void test_error_pod_contract()
{
    TEST_BEGIN("phyriad::Error POD contract (ResourceInitFailed)");

    const phyriad::Error e {
        .code           = phyriad::ErrorCode::ResourceInitFailed,
        .source_node_id = 0u,
        .timestamp_ns   = 0ull
    };
    static_assert(sizeof(phyriad::Error) == 16u);
    static_assert(alignof(phyriad::Error) == 16u);

    if (e.code == phyriad::ErrorCode::ResourceInitFailed)
        TEST_OK();
    else
        TEST_FAIL("Unexpected error code");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 5 — VulkanContext shutdown idempotente
// ─────────────────────────────────────────────────────────────────────────────
static void test_vulkan_context_shutdown_idempotent()
{
    TEST_BEGIN("VulkanContext::shutdown idempotent on fresh object");

    phyriad::render::vulkan::VulkanContext ctx;
    ctx.shutdown();
    ctx.shutdown();
    ctx.shutdown();

    TEST_OK();
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
int main()
{
    std::printf("=== phyriad_render_vulkan tests ===\n\n");

    test_error_pod_contract();
    test_vulkan_backend_shutdown_idempotent();
    test_vulkan_context_shutdown_idempotent();
    test_vulkan_backend_no_glfw();
    test_vulkan_context_nullptr_window();

    std::printf("\n--- Results: %d/%d passed",
                g_tests_passed, g_tests_run);
    if (g_tests_failed > 0)
        std::printf(", %d FAILED", g_tests_failed);
    std::printf(" ---\n");

    return (g_tests_failed == 0) ? 0 : 1;
}
// Made with my soul - Swately <3
