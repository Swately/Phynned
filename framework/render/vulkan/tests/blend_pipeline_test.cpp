// framework/render/vulkan/tests/blend_pipeline_test.cpp
// Headless mathematical verification of BlendPipeline.
//
// Setup:
//   - Create a Vulkan instance + device + compute queue (no surface).
//   - Allocate three R8G8B8A8_UNORM images A, B, C at 32×32.
//   - Upload A = solid red   (255,   0,   0, 255)
//   - Upload B = solid blue  (  0,   0, 255, 255)
//   - Run BlendPipeline on (A, B) -> C.
//   - Read C back to host memory.
//   - Assert every pixel ≈ (127, 0, 127, 255)  (within ±2 to absorb the
//     8-bit round-trip's half-LSB).
//
// If this test passes, the shader compiled, the pipeline created, the
// descriptors were bound correctly, and the GPU produced mathematically
// correct output — i.e. LinearBlend works end-to-end.
//
#include <phyriad/render/vulkan/BlendPipeline.hpp>

#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

// ─────────────────────────────────────────────────────────────────────────────
// Test harness (minimal — keep the file self-contained)
// ─────────────────────────────────────────────────────────────────────────────
int g_failed = 0;
#define CHECK(cond, msg) do {                                              \
    if (!(cond)) {                                                         \
        std::fprintf(stderr, "  [FAIL] %s\n", msg);                        \
        ++g_failed;                                                        \
    } else {                                                               \
        std::fprintf(stderr, "  [OK  ] %s\n", msg);                        \
    }                                                                      \
} while (0)

#define VK_CHECK(expr, msg) do {                                           \
    const VkResult _r = (expr);                                            \
    if (_r != VK_SUCCESS) {                                                \
        std::fprintf(stderr, "  [FAIL] %s (VkResult=%d)\n", msg,           \
                     static_cast<int>(_r));                                \
        ++g_failed;                                                        \
        return 1;                                                          \
    }                                                                      \
} while (0)

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
        {
            return i;
        }
    }
    return UINT32_MAX;
}

// ─────────────────────────────────────────────────────────────────────────────
// Image with backing memory + view
// ─────────────────────────────────────────────────────────────────────────────
struct TestImage {
    VkImage        image  {VK_NULL_HANDLE};
    VkDeviceMemory memory {VK_NULL_HANDLE};
    VkImageView    view   {VK_NULL_HANDLE};

    bool create(VkDevice device, VkPhysicalDevice phys,
                uint32_t w, uint32_t h,
                VkImageUsageFlags usage) noexcept
    {
        VkImageCreateInfo ci{};
        ci.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.imageType     = VK_IMAGE_TYPE_2D;
        ci.format        = kFormat;
        ci.extent        = {w, h, 1u};
        ci.mipLevels     = 1u;
        ci.arrayLayers   = 1u;
        ci.samples       = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ci.usage         = usage;
        ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(device, &ci, nullptr, &image) != VK_SUCCESS) return false;

        VkMemoryRequirements mr{};
        vkGetImageMemoryRequirements(device, image, &mr);
        const uint32_t mt = find_memory_type(phys, mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mt == UINT32_MAX) return false;

        VkMemoryAllocateInfo ai{};
        ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize  = mr.size;
        ai.memoryTypeIndex = mt;
        if (vkAllocateMemory(device, &ai, nullptr, &memory) != VK_SUCCESS) return false;
        vkBindImageMemory(device, image, memory, 0u);

        VkImageViewCreateInfo vi{};
        vi.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image                           = image;
        vi.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        vi.format                          = kFormat;
        vi.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount     = 1u;
        vi.subresourceRange.layerCount     = 1u;
        return vkCreateImageView(device, &vi, nullptr, &view) == VK_SUCCESS;
    }

    void destroy(VkDevice device) noexcept {
        if (view)   vkDestroyImageView(device, view, nullptr);
        if (image)  vkDestroyImage(device, image, nullptr);
        if (memory) vkFreeMemory(device, memory, nullptr);
        view = VK_NULL_HANDLE; image = VK_NULL_HANDLE; memory = VK_NULL_HANDLE;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Host-visible buffer (for staging upload + readback)
// ─────────────────────────────────────────────────────────────────────────────
struct HostBuffer {
    VkBuffer       buffer{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    VkDeviceSize   size  {0u};
    void*          mapped{nullptr};

    bool create(VkDevice device, VkPhysicalDevice phys,
                VkDeviceSize sz, VkBufferUsageFlags usage) noexcept
    {
        VkBufferCreateInfo ci{};
        ci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        ci.size        = sz;
        ci.usage       = usage;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device, &ci, nullptr, &buffer) != VK_SUCCESS) return false;

        VkMemoryRequirements mr{};
        vkGetBufferMemoryRequirements(device, buffer, &mr);
        const uint32_t mt = find_memory_type(phys, mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (mt == UINT32_MAX) return false;

        VkMemoryAllocateInfo ai{};
        ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize  = mr.size;
        ai.memoryTypeIndex = mt;
        if (vkAllocateMemory(device, &ai, nullptr, &memory) != VK_SUCCESS) return false;
        vkBindBufferMemory(device, buffer, memory, 0u);
        size = sz;
        return vkMapMemory(device, memory, 0u, sz, 0u, &mapped) == VK_SUCCESS;
    }

    void destroy(VkDevice device) noexcept {
        if (mapped && memory) vkUnmapMemory(device, memory);
        if (buffer)  vkDestroyBuffer(device, buffer, nullptr);
        if (memory)  vkFreeMemory(device, memory, nullptr);
        buffer = VK_NULL_HANDLE; memory = VK_NULL_HANDLE; mapped = nullptr; size = 0u;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Image layout transition + buffer/image copy helpers
// ─────────────────────────────────────────────────────────────────────────────
void barrier(VkCommandBuffer cmd, VkImage img,
             VkImageLayout old_l, VkImageLayout new_l,
             VkAccessFlags src_acc, VkAccessFlags dst_acc,
             VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) noexcept
{
    VkImageMemoryBarrier b{};
    b.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout                       = old_l;
    b.newLayout                       = new_l;
    b.srcAccessMask                   = src_acc;
    b.dstAccessMask                   = dst_acc;
    b.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    b.image                           = img;
    b.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount     = 1u;
    b.subresourceRange.layerCount     = 1u;
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0u, 0u, nullptr, 0u, nullptr, 1u, &b);
}

void copy_buffer_to_image(VkCommandBuffer cmd, VkBuffer buf, VkImage img,
                          uint32_t w, uint32_t h) noexcept
{
    VkBufferImageCopy r{};
    r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    r.imageSubresource.layerCount = 1u;
    r.imageExtent = {w, h, 1u};
    vkCmdCopyBufferToImage(cmd, buf, img,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &r);
}

void copy_image_to_buffer(VkCommandBuffer cmd, VkImage img, VkBuffer buf,
                          uint32_t w, uint32_t h) noexcept
{
    VkBufferImageCopy r{};
    r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    r.imageSubresource.layerCount = 1u;
    r.imageExtent = {w, h, 1u};
    vkCmdCopyImageToBuffer(cmd, img,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1u, &r);
}

} // anonymous

int main() {
    std::printf("blend_pipeline_test — headless mathematical verification\n");
    std::printf("──────────────────────────────────────────────────────────\n");

    // ── 1. Instance (with validation layers if available) ─────────────────
    VkInstance instance = VK_NULL_HANDLE;
    {
        VkApplicationInfo ai{};
        ai.sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        ai.pApplicationName = "blend_pipeline_test";
        ai.apiVersion       = VK_API_VERSION_1_2;

        VkInstanceCreateInfo ci{};
        ci.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ci.pApplicationInfo = &ai;

        // Try to enable the standard validation layer; non-fatal if absent.
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
            std::printf("  validation layer: not installed (test still meaningful)\n");
        }

        VK_CHECK(vkCreateInstance(&ci, nullptr, &instance), "vkCreateInstance");
    }

    // ── 2. Pick physical device + compute queue family ────────────────────
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    uint32_t compute_family = UINT32_MAX;
    {
        uint32_t n_phys = 0u;
        vkEnumeratePhysicalDevices(instance, &n_phys, nullptr);
        if (n_phys == 0u) {
            std::fprintf(stderr, "  [FAIL] no Vulkan physical devices\n");
            vkDestroyInstance(instance, nullptr);
            return 1;
        }
        std::vector<VkPhysicalDevice> phys_devs(n_phys);
        vkEnumeratePhysicalDevices(instance, &n_phys, phys_devs.data());

        // Prefer discrete GPU.
        for (auto pd : phys_devs) {
            VkPhysicalDeviceProperties pp{};
            vkGetPhysicalDeviceProperties(pd, &pp);
            if (pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                phys = pd;
                std::printf("  selected GPU: %s\n", pp.deviceName);
                break;
            }
        }
        if (phys == VK_NULL_HANDLE) {
            phys = phys_devs[0];
            VkPhysicalDeviceProperties pp{};
            vkGetPhysicalDeviceProperties(phys, &pp);
            std::printf("  selected GPU (non-discrete): %s\n", pp.deviceName);
        }

        uint32_t n_qf = 0u;
        vkGetPhysicalDeviceQueueFamilyProperties(phys, &n_qf, nullptr);
        std::vector<VkQueueFamilyProperties> qfs(n_qf);
        vkGetPhysicalDeviceQueueFamilyProperties(phys, &n_qf, qfs.data());
        for (uint32_t i = 0u; i < n_qf; ++i) {
            if (qfs[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                compute_family = i; break;
            }
        }
        if (compute_family == UINT32_MAX) {
            std::fprintf(stderr, "  [FAIL] no compute queue family\n");
            vkDestroyInstance(instance, nullptr);
            return 1;
        }
    }

    // ── 3. Logical device ──────────────────────────────────────────────────
    VkDevice device = VK_NULL_HANDLE;
    VkQueue  queue  = VK_NULL_HANDLE;
    {
        const float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{};
        qci.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = compute_family;
        qci.queueCount       = 1u;
        qci.pQueuePriorities = &prio;

        VkDeviceCreateInfo dci{};
        dci.sType                = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount = 1u;
        dci.pQueueCreateInfos    = &qci;

        VK_CHECK(vkCreateDevice(phys, &dci, nullptr, &device), "vkCreateDevice");
        vkGetDeviceQueue(device, compute_family, 0u, &queue);
    }

    // ── 4. Allocate images A, B, C ─────────────────────────────────────────
    TestImage a_img, b_img, c_img;
    const VkImageUsageFlags sampled_usage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const VkImageUsageFlags storage_usage =
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    CHECK(a_img.create(device, phys, kWidth, kHeight, sampled_usage), "A image created");
    CHECK(b_img.create(device, phys, kWidth, kHeight, sampled_usage), "B image created");
    CHECK(c_img.create(device, phys, kWidth, kHeight, storage_usage), "C image created");

    // ── 5. Staging buffer with red + blue, plus readback buffer for C ─────
    const VkDeviceSize img_bytes = static_cast<VkDeviceSize>(kWidth) * kHeight * 4u;
    HostBuffer stage_a, stage_b, readback;
    CHECK(stage_a.create(device, phys, img_bytes,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT), "staging A buffer");
    CHECK(stage_b.create(device, phys, img_bytes,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT), "staging B buffer");
    CHECK(readback.create(device, phys, img_bytes,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT), "readback buffer");

    // Fill staging buffers: A = solid red, B = solid blue.
    {
        auto* pa = static_cast<uint8_t*>(stage_a.mapped);
        auto* pb = static_cast<uint8_t*>(stage_b.mapped);
        for (uint32_t i = 0u; i < kWidth * kHeight; ++i) {
            pa[i*4 + 0] = 255; pa[i*4 + 1] = 0;   pa[i*4 + 2] = 0;   pa[i*4 + 3] = 255;
            pb[i*4 + 0] = 0;   pb[i*4 + 1] = 0;   pb[i*4 + 2] = 255; pb[i*4 + 3] = 255;
        }
    }

    // ── 6. Initialize BlendPipeline ────────────────────────────────────────
    phyriad::render::vulkan::BlendPipeline pipe;
    CHECK(pipe.init(device, phys, /* max_in_flight */ 4u),
          "BlendPipeline::init succeeded");
    if (!pipe.initialized()) {
        std::fprintf(stderr, "  init failed — aborting\n");
        return 1;
    }

    // ── 7. Command pool + buffer ───────────────────────────────────────────
    VkCommandPool cmd_pool = VK_NULL_HANDLE;
    {
        VkCommandPoolCreateInfo ci{};
        ci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        ci.queueFamilyIndex = compute_family;
        ci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VK_CHECK(vkCreateCommandPool(device, &ci, nullptr, &cmd_pool), "vkCreateCommandPool");
    }
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    {
        VkCommandBufferAllocateInfo ai{};
        ai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool        = cmd_pool;
        ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1u;
        VK_CHECK(vkAllocateCommandBuffers(device, &ai, &cmd), "vkAllocateCommandBuffers");
    }

    // ── 8. Record everything: uploads → barriers → dispatch → readback ────
    {
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        VK_CHECK(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer");

        // 8a. Transition A,B to TRANSFER_DST.
        barrier(cmd, a_img.image,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0u, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        barrier(cmd, b_img.image,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0u, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

        // 8b. Upload red + blue.
        copy_buffer_to_image(cmd, stage_a.buffer, a_img.image, kWidth, kHeight);
        copy_buffer_to_image(cmd, stage_b.buffer, b_img.image, kWidth, kHeight);

        // 8c. Transition A,B to SHADER_READ_ONLY; C to GENERAL.
        barrier(cmd, a_img.image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        barrier(cmd, b_img.image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        barrier(cmd, c_img.image,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
            0u, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

        // 8d. The dispatch — this is what we're testing.
        CHECK(pipe.record_blend(cmd, a_img.view, b_img.view, c_img.view,
                                kWidth, kHeight),
              "BlendPipeline::record_blend recorded");

        // 8e. Transition C to TRANSFER_SRC + readback.
        barrier(cmd, c_img.image,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
        copy_image_to_buffer(cmd, c_img.image, readback.buffer, kWidth, kHeight);

        VK_CHECK(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
    }

    // ── 9. Submit + wait via fence ─────────────────────────────────────────
    VkFence fence = VK_NULL_HANDLE;
    {
        VkFenceCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VK_CHECK(vkCreateFence(device, &fi, nullptr, &fence), "vkCreateFence");

        VkSubmitInfo si{};
        si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1u;
        si.pCommandBuffers    = &cmd;
        VK_CHECK(vkQueueSubmit(queue, 1u, &si, fence), "vkQueueSubmit");

        VK_CHECK(vkWaitForFences(device, 1u, &fence, VK_TRUE, 5'000'000'000ull),
                 "vkWaitForFences (5s timeout)");
    }

    // ── 10. Verify the readback pixels — the actual math test ─────────────
    {
        const auto* px = static_cast<const uint8_t*>(readback.mapped);
        // Expected midpoint: 0.5 · (255,0,0,255) + 0.5 · (0,0,255,255)
        //                  = (127.5, 0, 127.5, 255) — rounds to 127 or 128.
        uint32_t bad_pixels = 0u;
        uint32_t worst_dr = 0u, worst_dg = 0u, worst_db = 0u, worst_da = 0u;
        for (uint32_t i = 0u; i < kWidth * kHeight; ++i) {
            const uint8_t r = px[i*4 + 0];
            const uint8_t g = px[i*4 + 1];
            const uint8_t b = px[i*4 + 2];
            const uint8_t a = px[i*4 + 3];
            const uint32_t dr = static_cast<uint32_t>(std::abs(int{r} - 127));
            const uint32_t dg = static_cast<uint32_t>(std::abs(int{g} - 0));
            const uint32_t db = static_cast<uint32_t>(std::abs(int{b} - 127));
            const uint32_t da = static_cast<uint32_t>(std::abs(int{a} - 255));
            if (dr > 2u || dg > 2u || db > 2u || da > 2u) ++bad_pixels;
            worst_dr = std::max(worst_dr, dr);
            worst_dg = std::max(worst_dg, dg);
            worst_db = std::max(worst_db, db);
            worst_da = std::max(worst_da, da);
        }
        std::printf(
            "  readback worst delta from expected (127,0,127,255):\n"
            "      R=%u  G=%u  B=%u  A=%u   (tolerance: 2)\n"
            "  sample center pixel: (%u, %u, %u, %u)\n",
            worst_dr, worst_dg, worst_db, worst_da,
            px[(kHeight/2 * kWidth + kWidth/2) * 4 + 0],
            px[(kHeight/2 * kWidth + kWidth/2) * 4 + 1],
            px[(kHeight/2 * kWidth + kWidth/2) * 4 + 2],
            px[(kHeight/2 * kWidth + kWidth/2) * 4 + 3]);
        CHECK(bad_pixels == 0u, "all 1024 pixels within ±2 of expected midpoint");
    }

    // ── 11. Cleanup ────────────────────────────────────────────────────────
    vkDestroyFence(device, fence, nullptr);
    vkDestroyCommandPool(device, cmd_pool, nullptr);
    pipe.shutdown(device);
    readback.destroy(device);
    stage_b.destroy(device);
    stage_a.destroy(device);
    c_img.destroy(device);
    b_img.destroy(device);
    a_img.destroy(device);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);

    std::printf("──────────────────────────────────────────────────────────\n");
    std::printf("RESULT: %s\n", g_failed == 0 ? "PASS" : "FAIL");
    return g_failed == 0 ? 0 : 1;
}
// Made with my soul - Swately <3
