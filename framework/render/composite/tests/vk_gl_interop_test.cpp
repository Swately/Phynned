// framework/render/composite/tests/vk_gl_interop_test.cpp
// End-to-end mathematical verification of VK→GL shared-memory interop.
//
// Pipeline:
//   1. Set up Vulkan instance + device with external-memory extensions.
//   2. Set up a hidden GLFW window + GL 4.5 context (the OpenGL side).
//   3. Allocate a Vulkan ExternalImage; export its memory handle.
//   4. Import the handle into an OpenGL ExternalTexture.
//   5. From the Vulkan side: vkCmdClearColorImage paints the image
//      with a known colour (0.2, 0.4, 0.6, 1.0). Submit + wait fence.
//   6. From the OpenGL side: glGetTextureImage reads the texture back
//      to host memory.
//   7. Assert every pixel matches (51, 102, 153, 255) within ±2.
//
// If this passes, the entire VK↔GL shared-memory path is verified end
// to end — the same memory is being written by Vulkan and read by
// OpenGL with no CPU round-trip.
//
#include <phyriad/render/vulkan/ExternalImage.hpp>
#include <phyriad/render/opengl3/ExternalTexture.hpp>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

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

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

// ─────────────────────────────────────────────────────────────────────────────
// Minimal GL types + entries needed by the test (mirrors what
// opengl3/ExternalTexture.cpp uses; we duplicate here to avoid an extra
// API surface on the public ExternalTexture header).
// ─────────────────────────────────────────────────────────────────────────────
#ifndef APIENTRY
#   ifdef _WIN32
#       define APIENTRY __stdcall
#   else
#       define APIENTRY
#   endif
#endif
#define APIENTRYP APIENTRY *

typedef unsigned int  GLenum;
typedef unsigned int  GLuint;
typedef int           GLint;
typedef int           GLsizei;
typedef int           GLintptr;
typedef int           GLsizeiptr;
typedef unsigned char GLubyte;

#define GL_RGBA                                  0x1908
#define GL_RGBA8                                 0x8058
#define GL_UNSIGNED_BYTE                         0x1401
#define GL_NO_ERROR                              0
#define GL_TEXTURE_2D                            0x0DE1

typedef GLenum (APIENTRYP PFNGLGETERRORPROC)(void);
typedef void (APIENTRYP PFNGLGETTEXTUREIMAGEPROC)(
    GLuint texture, GLint level, GLenum format, GLenum type,
    GLsizei bufSize, void* pixels);
typedef void (APIENTRYP PFNGLFINISHPROC)(void);

constexpr uint32_t kWidth  = 32u;
constexpr uint32_t kHeight = 32u;
constexpr VkFormat kFormat = VK_FORMAT_R8G8B8A8_UNORM;

[[nodiscard]] uint32_t find_memory_type(VkPhysicalDevice phys,
                                        uint32_t          type_bits,
                                        VkMemoryPropertyFlags want) noexcept
{
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0u; i < mp.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    }
    return UINT32_MAX;
}

} // anonymous

int main() {
    std::printf("vk_gl_interop_test — end-to-end VK↔GL shared memory verification\n");
    std::printf("─────────────────────────────────────────────────────────────────\n");

    // ── 1. Vulkan instance ────────────────────────────────────────────────
    VkInstance instance = VK_NULL_HANDLE;
    {
        VkApplicationInfo ai{};
        ai.sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        ai.pApplicationName = "vk_gl_interop_test";
        ai.apiVersion       = VK_API_VERSION_1_2;

        VkInstanceCreateInfo ci{};
        ci.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ci.pApplicationInfo = &ai;

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
        }
        VK_CHECK(vkCreateInstance(&ci, nullptr, &instance), "vkCreateInstance");
    }

    // ── 2. Physical device + queue family ─────────────────────────────────
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    uint32_t qf = UINT32_MAX;
    {
        uint32_t n = 0u;
        vkEnumeratePhysicalDevices(instance, &n, nullptr);
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
        if (phys == VK_NULL_HANDLE && !devs.empty()) phys = devs[0];

        uint32_t qn = 0u;
        vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qfs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, qfs.data());
        for (uint32_t i = 0u; i < qn; ++i) {
            if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qf = i; break; }
        }
    }

    // ── 3. Vulkan device with external-memory extension enabled ──────────
    VkDevice device = VK_NULL_HANDLE;
    VkQueue  queue  = VK_NULL_HANDLE;
    {
        const float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{};
        qci.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = qf;
        qci.queueCount       = 1u;
        qci.pQueuePriorities = &prio;

        const char* exts[] = {
#ifdef _WIN32
            VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME
#else
            VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME
#endif
        };
        VkDeviceCreateInfo dci{};
        dci.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount    = 1u;
        dci.pQueueCreateInfos       = &qci;
        dci.enabledExtensionCount   = 1u;
        dci.ppEnabledExtensionNames = exts;

        VK_CHECK(vkCreateDevice(phys, &dci, nullptr, &device), "vkCreateDevice");
        vkGetDeviceQueue(device, qf, 0u, &queue);
    }

    // ── 4. Allocate the ExternalImage on the Vulkan side ─────────────────
    phyriad::render::vulkan::ExternalImage vk_img;
    {
        phyriad::render::vulkan::ExternalImage::Config cfg{};
        cfg.width  = kWidth;
        cfg.height = kHeight;
        cfg.format = kFormat;
        CHECK(vk_img.init(phys, device, cfg),
              "VK ExternalImage::init succeeded");
    }

    // ── 5. Export the VK memory handle ───────────────────────────────────
#ifdef _WIN32
    void* vk_handle = nullptr;
    CHECK(vk_img.export_handle_win32(device, &vk_handle),
          "VK exported HANDLE");
#else
    int vk_handle = -1;
    CHECK(vk_img.export_handle_fd(device, &vk_handle),
          "VK exported fd");
#endif

    // ── 6. GLFW + hidden GL 4.5 context (the OpenGL side) ────────────────
    if (!glfwInit()) {
        std::fprintf(stderr, "  [FAIL] glfwInit\n");
        return 1;
    }
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow* gl_window = glfwCreateWindow(64, 64, "interop_test", nullptr, nullptr);
    if (!gl_window) {
        std::fprintf(stderr, "  [FAIL] glfwCreateWindow (GL 4.5 core)\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(gl_window);
    CHECK(gl_window != nullptr, "GLFW hidden window + GL 4.5 context created");

    // Probe extensions before we try to import.
    const bool gl_ext_ok = phyriad::render::opengl3::ExternalTexture::extensions_available();
    CHECK(gl_ext_ok, "GL_EXT_memory_object{,_win32/_fd} available");
    if (!gl_ext_ok) {
        glfwDestroyWindow(gl_window);
        glfwTerminate();
        vk_img.shutdown(device);
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);
        return 1;
    }

    // ── 7. Import the VK handle into a GL texture ────────────────────────
    phyriad::render::opengl3::ExternalTexture gl_tex;
    {
#ifdef _WIN32
        const bool ok = gl_tex.init(vk_handle,
                                     static_cast<uint64_t>(vk_img.memory_size()),
                                     kWidth, kHeight, GL_RGBA8);
#else
        const bool ok = gl_tex.init(vk_handle,
                                     static_cast<uint64_t>(vk_img.memory_size()),
                                     kWidth, kHeight, GL_RGBA8);
#endif
        CHECK(ok,             "GL ExternalTexture::init succeeded");
        CHECK(gl_tex.valid(), "GL texture is valid");
        CHECK(gl_tex.texture() != 0u, "GL texture id non-zero");
    }

    // ── 8. Vulkan clears the image to a known colour ─────────────────────
    //   (0.2, 0.4, 0.6, 1.0) in normalized → bytes (51, 102, 153, 255).
    VkCommandPool cmd_pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    {
        VkCommandPoolCreateInfo pi{};
        pi.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pi.queueFamilyIndex = qf;
        pi.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VK_CHECK(vkCreateCommandPool(device, &pi, nullptr, &cmd_pool),
                 "vkCreateCommandPool");

        VkCommandBufferAllocateInfo ai{};
        ai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool        = cmd_pool;
        ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1u;
        VK_CHECK(vkAllocateCommandBuffers(device, &ai, &cmd),
                 "vkAllocateCommandBuffers");

        VkFenceCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VK_CHECK(vkCreateFence(device, &fi, nullptr, &fence), "vkCreateFence");

        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        VK_CHECK(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer");

        // Transition: UNDEFINED → TRANSFER_DST.
        VkImageMemoryBarrier b{};
        b.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout                   = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout                   = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcAccessMask               = 0u;
        b.dstAccessMask               = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        b.image                       = vk_img.image();
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.levelCount = 1u;
        b.subresourceRange.layerCount = 1u;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0u, 0u, nullptr, 0u, nullptr, 1u, &b);

        VkClearColorValue clear{};
        clear.float32[0] = 0.2f;   // → byte 51
        clear.float32[1] = 0.4f;   // → byte 102
        clear.float32[2] = 0.6f;   // → byte 153
        clear.float32[3] = 1.0f;   // → byte 255

        VkImageSubresourceRange range{};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.levelCount = 1u;
        range.layerCount = 1u;
        vkCmdClearColorImage(cmd, vk_img.image(),
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1u, &range);

        // Transition to GENERAL. The "consumer" is OpenGL — outside the
        // Vulkan pipeline — so the canonical dstStage/Access pair for
        // "release ownership, no in-VK consumer" is BOTTOM_OF_PIPE +
        // zero access. The fence + glFinish() pair establish the cross-
        // API synchronization that an extension semaphore would replace.
        VkImageMemoryBarrier b2 = b;
        b2.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b2.newLayout     = VK_IMAGE_LAYOUT_GENERAL;
        b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b2.dstAccessMask = 0u;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0u, 0u, nullptr, 0u, nullptr, 1u, &b2);

        VK_CHECK(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");

        VkSubmitInfo si{};
        si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1u;
        si.pCommandBuffers    = &cmd;
        VK_CHECK(vkQueueSubmit(queue, 1u, &si, fence), "vkQueueSubmit(clear)");

        // Synchronisation note: GL_EXT_semaphore would give us proper
        // VK↔GL sync. For this Stage-3 verification we use coarse-grained
        // vkWaitForFences + glFinish() on the GL side instead. That's
        // strictly slower than a semaphore but correctness-equivalent.
        VK_CHECK(vkWaitForFences(device, 1u, &fence, VK_TRUE, 5'000'000'000ull),
                 "vkWaitForFences (Vulkan clear done)");
    }

    // ── 9. Read back the same memory through the OpenGL texture ──────────
    {
        auto glFinish_fn = reinterpret_cast<PFNGLFINISHPROC>(
            glfwGetProcAddress("glFinish"));
        auto glGetTextureImage_fn =
            reinterpret_cast<PFNGLGETTEXTUREIMAGEPROC>(
                glfwGetProcAddress("glGetTextureImage"));
        auto glGetError_fn = reinterpret_cast<PFNGLGETERRORPROC>(
            glfwGetProcAddress("glGetError"));
        CHECK(glGetTextureImage_fn != nullptr,
              "glGetTextureImage loaded");

        // Wait for any pending GL ops (the import was synchronous, but
        // be explicit so the readback observes the import's effects).
        if (glFinish_fn) glFinish_fn();

        std::vector<uint8_t> pixels(kWidth * kHeight * 4u, 0u);
        glGetTextureImage_fn(gl_tex.texture(), 0,
            GL_RGBA, GL_UNSIGNED_BYTE,
            static_cast<GLsizei>(pixels.size()), pixels.data());
        const GLenum err = glGetError_fn ? glGetError_fn() : 0u;
        CHECK(err == GL_NO_ERROR, "glGetTextureImage returned GL_NO_ERROR");

        // Verify the pixels equal what Vulkan cleared the image to.
        uint32_t bad_pixels = 0u;
        uint32_t worst_dr = 0u, worst_dg = 0u, worst_db = 0u, worst_da = 0u;
        for (uint32_t i = 0u; i < kWidth * kHeight; ++i) {
            const uint8_t r = pixels[i*4 + 0];
            const uint8_t g = pixels[i*4 + 1];
            const uint8_t b = pixels[i*4 + 2];
            const uint8_t a = pixels[i*4 + 3];
            const uint32_t dr = static_cast<uint32_t>(std::abs(int{r} - 51));
            const uint32_t dg = static_cast<uint32_t>(std::abs(int{g} - 102));
            const uint32_t db = static_cast<uint32_t>(std::abs(int{b} - 153));
            const uint32_t da = static_cast<uint32_t>(std::abs(int{a} - 255));
            if (dr > 2u || dg > 2u || db > 2u || da > 2u) ++bad_pixels;
            worst_dr = std::max(worst_dr, dr);
            worst_dg = std::max(worst_dg, dg);
            worst_db = std::max(worst_db, db);
            worst_da = std::max(worst_da, da);
        }
        std::printf(
            "  readback worst delta vs (51,102,153,255):  R=%u G=%u B=%u A=%u\n"
            "  sample center pixel: (%u, %u, %u, %u)\n",
            worst_dr, worst_dg, worst_db, worst_da,
            pixels[(kHeight/2 * kWidth + kWidth/2) * 4 + 0],
            pixels[(kHeight/2 * kWidth + kWidth/2) * 4 + 1],
            pixels[(kHeight/2 * kWidth + kWidth/2) * 4 + 2],
            pixels[(kHeight/2 * kWidth + kWidth/2) * 4 + 3]);
        CHECK(bad_pixels == 0u,
              "all 1024 pixels match VK-cleared colour through GL texture");
    }

    // ── 10. Cleanup ───────────────────────────────────────────────────────
    gl_tex.shutdown();
    vkDestroyFence(device, fence, nullptr);
    vkDestroyCommandPool(device, cmd_pool, nullptr);
    vk_img.shutdown(device);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);

    glfwDestroyWindow(gl_window);
    glfwTerminate();

    std::printf("─────────────────────────────────────────────────────────────────\n");
    std::printf("RESULT: %s\n", g_failed == 0 ? "PASS" : "FAIL");
    return g_failed == 0 ? 0 : 1;
}
// Made with my soul - Swately <3
