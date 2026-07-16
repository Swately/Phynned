// framework/render/vulkan/src/FramePool.cpp
// FramePool — Vulkan image pool implementation.
//
// Allocates kPoolSize VkImage + VkImageView + VkDeviceMemory objects
// at init time.  No dynamic allocation in the hot path.
//
// Memory selection: prefers device-local (VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
// memory type for GPU-optimal performance.  Falls back to any valid type.
//
#include <phyriad/render/vulkan/FramePool.hpp>
#include <cstdio>
#include <phyriad/hal/MemoryOrder.hpp>

namespace phyriad::render::vulkan {

// ─────────────────────────────────────────────────────────────────────────────
// Helper — find a memory type index satisfying the property requirements.
// ─────────────────────────────────────────────────────────────────────────────
static uint32_t find_memory_type(VkPhysicalDevice   phys_dev,
                                  uint32_t           type_filter,
                                  VkMemoryPropertyFlags props) noexcept
{
    VkPhysicalDeviceMemoryProperties mem_props{};
    vkGetPhysicalDeviceMemoryProperties(phys_dev, &mem_props);

    for (uint32_t i = 0u; i < mem_props.memoryTypeCount; ++i) {
        if ((type_filter & (1u << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & props) == props)
        {
            return i;
        }
    }
    return UINT32_MAX;
}

// ─────────────────────────────────────────────────────────────────────────────
// init
// ─────────────────────────────────────────────────────────────────────────────
bool FramePool::init(VkDevice         device,
                     VkPhysicalDevice phys_dev,
                     uint32_t         width,
                     uint32_t         height,
                     VkFormat         format) noexcept
{
    if (initialized_) shutdown(device);

    for (uint32_t i = 0u; i < kPoolSize; ++i) {
        Slot& s = slots_[i];

        // ── Create image ──────────────────────────────────────────────────────
        VkImageCreateInfo img_info{};
        img_info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        img_info.imageType     = VK_IMAGE_TYPE_2D;
        img_info.format        = format;
        img_info.extent        = {width, height, 1u};
        img_info.mipLevels     = 1u;
        img_info.arrayLayers   = 1u;
        img_info.samples       = VK_SAMPLE_COUNT_1_BIT;
        img_info.tiling        = VK_IMAGE_TILING_OPTIMAL;
        img_info.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                               | VK_IMAGE_USAGE_SAMPLED_BIT
                               | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                               | VK_IMAGE_USAGE_STORAGE_BIT;
        img_info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        img_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(device, &img_info, nullptr, &s.image) != VK_SUCCESS) {
            std::fprintf(stderr, "[FramePool] vkCreateImage failed for slot %u\n", i);
            shutdown(device);
            return false;
        }

        // ── Allocate memory ───────────────────────────────────────────────────
        VkMemoryRequirements mem_req{};
        vkGetImageMemoryRequirements(device, s.image, &mem_req);

        uint32_t mem_type = find_memory_type(
            phys_dev, mem_req.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (mem_type == UINT32_MAX) {
            // Fallback: any valid type.
            mem_type = find_memory_type(phys_dev, mem_req.memoryTypeBits, 0u);
            if (mem_type == UINT32_MAX) {
                std::fprintf(stderr, "[FramePool] no valid memory type for slot %u\n", i);
                shutdown(device);
                return false;
            }
        }

        VkMemoryAllocateInfo alloc_info{};
        alloc_info.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc_info.allocationSize  = mem_req.size;
        alloc_info.memoryTypeIndex = mem_type;

        if (vkAllocateMemory(device, &alloc_info, nullptr, &s.memory) != VK_SUCCESS) {
            std::fprintf(stderr, "[FramePool] vkAllocateMemory failed for slot %u\n", i);
            shutdown(device);
            return false;
        }
        vkBindImageMemory(device, s.image, s.memory, 0u);

        // ── Create image view ─────────────────────────────────────────────────
        VkImageViewCreateInfo view_info{};
        view_info.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image                           = s.image;
        view_info.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format                          = format;
        view_info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.baseMipLevel   = 0u;
        view_info.subresourceRange.levelCount     = 1u;
        view_info.subresourceRange.baseArrayLayer = 0u;
        view_info.subresourceRange.layerCount     = 1u;

        if (vkCreateImageView(device, &view_info, nullptr, &s.view) != VK_SUCCESS) {
            std::fprintf(stderr, "[FramePool] vkCreateImageView failed for slot %u\n", i);
            shutdown(device);
            return false;
        }

        hal::stat_store_relaxed(s.in_use, 0u);
    }

    width_       = width;
    height_      = height;
    format_      = format;
    initialized_ = true;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// shutdown
// ─────────────────────────────────────────────────────────────────────────────
void FramePool::shutdown(VkDevice device) noexcept
{
    for (uint32_t i = 0u; i < kPoolSize; ++i) {
        Slot& s = slots_[i];
        if (s.view   != VK_NULL_HANDLE) { vkDestroyImageView(device, s.view,   nullptr); s.view   = VK_NULL_HANDLE; }
        if (s.image  != VK_NULL_HANDLE) { vkDestroyImage    (device, s.image,  nullptr); s.image  = VK_NULL_HANDLE; }
        if (s.memory != VK_NULL_HANDLE) { vkFreeMemory      (device, s.memory, nullptr); s.memory = VK_NULL_HANDLE; }
        hal::stat_store_relaxed(s.in_use, 0u);
    }
    initialized_ = false;
    width_ = height_ = 0u;
    format_ = VK_FORMAT_UNDEFINED;
}

} // namespace phyriad::render::vulkan
// Made with my soul - Swately <3
