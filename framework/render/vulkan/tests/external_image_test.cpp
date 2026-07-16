// framework/render/vulkan/tests/external_image_test.cpp
// Headless verification that ExternalImage can:
//   1. Be created with the platform's external-memory extension enabled.
//   2. Allocate a device-local VkImage + memory.
//   3. Export an OS handle (Windows HANDLE or POSIX fd) that is non-null.
//
// On Stage 3 we'll add a sister test that imports the same handle into
// an OpenGL texture and verifies the imported texture is sampleable.
// For this stage the test stops at "VkAllocateMemory + export succeeded".
//
#include <phyriad/render/vulkan/ExternalImage.hpp>

#ifdef _WIN32
#   ifndef WIN32_LEAN_AND_MEAN
#       define WIN32_LEAN_AND_MEAN
#   endif
#   ifndef NOMINMAX
#       define NOMINMAX
#   endif
#   include <windows.h>
#   include <vulkan/vulkan_win32.h>
#endif
#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int g_failed = 0;
#define CHECK(cond, msg) do {                                          \
    if (!(cond)) {                                                     \
        std::fprintf(stderr, "  [FAIL] %s\n", msg);                    \
        ++g_failed;                                                    \
    } else {                                                           \
        std::fprintf(stderr, "  [OK  ] %s\n", msg);                    \
    }                                                                  \
} while (0)

#define VK_CHECK(expr, msg) do {                                       \
    const VkResult _r = (expr);                                        \
    if (_r != VK_SUCCESS) {                                            \
        std::fprintf(stderr, "  [FAIL] %s (VkResult=%d)\n", msg,       \
                     static_cast<int>(_r));                            \
        ++g_failed;                                                    \
        return 1;                                                      \
    }                                                                  \
} while (0)

} // anonymous

int main() {
    std::printf("external_image_test — headless VK external-memory export\n");
    std::printf("───────────────────────────────────────────────────────────\n");

    // ── 1. Vulkan instance ────────────────────────────────────────────────
    VkInstance instance = VK_NULL_HANDLE;
    {
        VkApplicationInfo ai{};
        ai.sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        ai.pApplicationName = "external_image_test";
        ai.apiVersion       = VK_API_VERSION_1_2;

        VkInstanceCreateInfo ci{};
        ci.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ci.pApplicationInfo = &ai;

        // No instance-level extensions needed — external memory is
        // device-level (and was core-promoted in Vulkan 1.1; the
        // platform handle variants stay extensions).

        const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
        uint32_t n_layers = 0u;
        vkEnumerateInstanceLayerProperties(&n_layers, nullptr);
        std::vector<VkLayerProperties> layers(n_layers);
        vkEnumerateInstanceLayerProperties(&n_layers, layers.data());
        bool have_validation = false;
        for (const auto& l : layers) {
            if (std::strcmp(l.layerName, kValidationLayer) == 0) {
                have_validation = true; break;
            }
        }
        if (have_validation) {
            ci.enabledLayerCount   = 1u;
            ci.ppEnabledLayerNames = &kValidationLayer;
            std::printf("  validation layer: enabled\n");
        } else {
            std::printf("  validation layer: not installed\n");
        }

        VK_CHECK(vkCreateInstance(&ci, nullptr, &instance), "vkCreateInstance");
    }

    // ── 2. Physical device (prefer discrete) ───────────────────────────────
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    {
        uint32_t n = 0u;
        vkEnumeratePhysicalDevices(instance, &n, nullptr);
        if (n == 0u) {
            std::fprintf(stderr, "  [FAIL] no Vulkan physical devices\n");
            vkDestroyInstance(instance, nullptr);
            return 1;
        }
        std::vector<VkPhysicalDevice> devs(n);
        vkEnumeratePhysicalDevices(instance, &n, devs.data());

        for (auto pd : devs) {
            VkPhysicalDeviceProperties p{};
            vkGetPhysicalDeviceProperties(pd, &p);
            if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                phys = pd;
                std::printf("  selected GPU: %s\n", p.deviceName);
                break;
            }
        }
        if (phys == VK_NULL_HANDLE) {
            phys = devs[0];
            VkPhysicalDeviceProperties p{};
            vkGetPhysicalDeviceProperties(phys, &p);
            std::printf("  selected GPU (non-discrete): %s\n", p.deviceName);
        }
    }

    // ── 3. Check that the platform's external-memory extension is supported
    const char* kExtMemExt =
#ifdef _WIN32
        VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME;
#else
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
#endif
    {
        uint32_t n = 0u;
        vkEnumerateDeviceExtensionProperties(phys, nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> props(n);
        vkEnumerateDeviceExtensionProperties(phys, nullptr, &n, props.data());
        bool found = false;
        for (const auto& p : props) {
            if (std::strcmp(p.extensionName, kExtMemExt) == 0) { found = true; break; }
        }
        CHECK(found, kExtMemExt);
        if (!found) {
            std::fprintf(stderr,
                "  driver does not support %s — cannot proceed\n", kExtMemExt);
            vkDestroyInstance(instance, nullptr);
            return 1;
        }
    }

    // ── 4. Compute queue family ────────────────────────────────────────────
    uint32_t qf = UINT32_MAX;
    {
        uint32_t n = 0u;
        vkGetPhysicalDeviceQueueFamilyProperties(phys, &n, nullptr);
        std::vector<VkQueueFamilyProperties> qfs(n);
        vkGetPhysicalDeviceQueueFamilyProperties(phys, &n, qfs.data());
        for (uint32_t i = 0u; i < n; ++i) {
            if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qf = i; break; }
        }
        CHECK(qf != UINT32_MAX, "graphics queue family found");
    }

    // ── 5. Logical device WITH the external-memory extension enabled ──────
    VkDevice device = VK_NULL_HANDLE;
    {
        const float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{};
        qci.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = qf;
        qci.queueCount       = 1u;
        qci.pQueuePriorities = &prio;

        const char* exts[] = { kExtMemExt };

        VkDeviceCreateInfo dci{};
        dci.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount    = 1u;
        dci.pQueueCreateInfos       = &qci;
        dci.enabledExtensionCount   = 1u;
        dci.ppEnabledExtensionNames = exts;

        VK_CHECK(vkCreateDevice(phys, &dci, nullptr, &device), "vkCreateDevice");
    }

    // ── 6. Create the ExternalImage — the actual subject of the test ─────
    phyriad::render::vulkan::ExternalImage ext_img;
    {
        phyriad::render::vulkan::ExternalImage::Config cfg{};
        cfg.width  = 256u;
        cfg.height = 256u;
        cfg.format = VK_FORMAT_R8G8B8A8_UNORM;
        // Default usage covers everything; leave it alone.

        CHECK(ext_img.init(phys, device, cfg), "ExternalImage::init succeeded");
        CHECK(ext_img.valid(),                  "ExternalImage reports valid");
        CHECK(ext_img.image() != VK_NULL_HANDLE, "image handle non-null");
        CHECK(ext_img.view()  != VK_NULL_HANDLE, "view handle non-null");
        CHECK(ext_img.memory_size() >= 256u * 256u * 4u,
              "memory size >= width*height*4 bytes");
        CHECK(ext_img.width()  == 256u, "width preserved");
        CHECK(ext_img.height() == 256u, "height preserved");
    }

    // ── 7. Export the OS handle — the key VK→GL interop primitive ────────
    {
#ifdef _WIN32
        void* h = nullptr;
        const bool ok = ext_img.export_handle_win32(device, &h);
        CHECK(ok,           "export_handle_win32 succeeded");
        CHECK(h != nullptr, "exported HANDLE is non-null");

        if (h != nullptr) {
            // The HANDLE is a Win32 kernel object — verify it shows up
            // as a "named object" or at least is closeable. We could
            // call GetHandleInformation but the simplest portable check
            // is CloseHandle() — if the handle is invalid the call sets
            // GetLastError, which we can observe.
            const BOOL closed = CloseHandle(static_cast<HANDLE>(h));
            CHECK(closed != 0, "CloseHandle on exported handle succeeded");
        }
#else
        int fd = -1;
        const bool ok = ext_img.export_handle_fd(device, &fd);
        CHECK(ok,       "export_handle_fd succeeded");
        CHECK(fd >= 0, "exported fd is non-negative");

        if (fd >= 0) {
            const int closed = ::close(fd);
            CHECK(closed == 0, "close() on exported fd succeeded");
        }
#endif
    }

    // ── 8. Cleanup ─────────────────────────────────────────────────────────
    ext_img.shutdown(device);
    CHECK(!ext_img.valid(), "shutdown leaves the instance invalid");

    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);

    std::printf("───────────────────────────────────────────────────────────\n");
    std::printf("RESULT: %s\n", g_failed == 0 ? "PASS" : "FAIL");
    return g_failed == 0 ? 0 : 1;
}
// Made with my soul - Swately <3
