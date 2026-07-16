// framework/render/vulkan/src/ExternalImage.cpp
// Implementation — see header for the contract.
//
#include <phyriad/render/vulkan/ExternalImage.hpp>

// Platform-specific Vulkan extension headers.
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

#include <cstdio>

namespace phyriad::render::vulkan {

namespace {

void log_vk_err(const char* op, VkResult r) noexcept {
    std::fprintf(stderr, "[ExternalImage] %s -> VkResult=%d\n",
                 op, static_cast<int>(r));
}

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

// Platform-specific handle type constant. Kept TU-local since callers
// don't need to know which type we use.
#ifdef _WIN32
constexpr VkExternalMemoryHandleTypeFlagBits kHandleType =
    VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
constexpr VkExternalMemoryHandleTypeFlagBits kHandleType =
    VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif

} // anonymous

// ─────────────────────────────────────────────────────────────────────────────
// init
// ─────────────────────────────────────────────────────────────────────────────
bool ExternalImage::init(VkPhysicalDevice phys_dev,
                         VkDevice         device,
                         Config const&    cfg) noexcept
{
    if (valid()) {
        std::fprintf(stderr, "[ExternalImage] already initialized\n");
        return true;
    }
    if (phys_dev == VK_NULL_HANDLE || device == VK_NULL_HANDLE ||
        cfg.width == 0u || cfg.height == 0u)
    {
        std::fprintf(stderr, "[ExternalImage] init: invalid arguments\n");
        return false;
    }

    w_   = cfg.width;
    h_   = cfg.height;
    fmt_ = cfg.format;

    // ── 1. Create image with VkExternalMemoryImageCreateInfo in pNext.
    //       This tells the driver the image's memory may be exported.
    VkExternalMemoryImageCreateInfo ext_image_info{};
    ext_image_info.sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    ext_image_info.handleTypes = kHandleType;

    VkImageCreateInfo image_info{};
    image_info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.pNext         = &ext_image_info;
    image_info.imageType     = VK_IMAGE_TYPE_2D;
    image_info.format        = cfg.format;
    image_info.extent        = { cfg.width, cfg.height, 1u };
    image_info.mipLevels     = 1u;
    image_info.arrayLayers   = 1u;
    image_info.samples       = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling        = cfg.tiling;
    image_info.usage         = cfg.usage;
    image_info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    {
        const VkResult r = vkCreateImage(device, &image_info, nullptr, &image_);
        if (r != VK_SUCCESS) {
            log_vk_err("vkCreateImage(external)", r);
            shutdown(device);
            return false;
        }
    }

    // ── 2. Allocate memory with VkExportMemoryAllocateInfoKHR in pNext.
    //       This makes the underlying allocation exportable as an OS handle.
    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(device, image_, &mr);

    const uint32_t mem_type = find_memory_type(phys_dev, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mem_type == UINT32_MAX) {
        std::fprintf(stderr,
            "[ExternalImage] no DEVICE_LOCAL memory type for image\n");
        shutdown(device);
        return false;
    }

    VkExportMemoryAllocateInfo export_info{};
    export_info.sType       = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    export_info.handleTypes = kHandleType;

#ifdef _WIN32
    // Optional: VkExportMemoryWin32HandleInfoKHR lets us set
    // SECURITY_ATTRIBUTES + a name on the exported handle. We don't need
    // either — the default null-attributes + unnamed handle is exactly
    // what GL_EXT_external_objects_win32 expects to import.
    //
    // Chain order: VkMemoryAllocateInfo.pNext → VkExportMemoryAllocateInfo
    //              → VkExportMemoryWin32HandleInfoKHR (NOT needed; skipped).
#endif

    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.pNext           = &export_info;
    alloc_info.allocationSize  = mr.size;
    alloc_info.memoryTypeIndex = mem_type;

    {
        const VkResult r = vkAllocateMemory(device, &alloc_info, nullptr, &memory_);
        if (r != VK_SUCCESS) {
            log_vk_err("vkAllocateMemory(export)", r);
            shutdown(device);
            return false;
        }
    }
    size_ = mr.size;

    if (vkBindImageMemory(device, image_, memory_, 0u) != VK_SUCCESS) {
        std::fprintf(stderr, "[ExternalImage] vkBindImageMemory failed\n");
        shutdown(device);
        return false;
    }

    // ── 3. Create the image view (so callers can sample / render with it).
    VkImageViewCreateInfo view_info{};
    view_info.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image                           = image_;
    view_info.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format                          = cfg.format;
    view_info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount     = 1u;
    view_info.subresourceRange.layerCount     = 1u;
    if (vkCreateImageView(device, &view_info, nullptr, &view_) != VK_SUCCESS) {
        std::fprintf(stderr, "[ExternalImage] vkCreateImageView failed\n");
        shutdown(device);
        return false;
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// shutdown
// ─────────────────────────────────────────────────────────────────────────────
void ExternalImage::shutdown(VkDevice device) noexcept {
    if (device == VK_NULL_HANDLE) return;
    if (view_   != VK_NULL_HANDLE) { vkDestroyImageView(device, view_, nullptr);   view_   = VK_NULL_HANDLE; }
    if (image_  != VK_NULL_HANDLE) { vkDestroyImage    (device, image_, nullptr);  image_  = VK_NULL_HANDLE; }
    if (memory_ != VK_NULL_HANDLE) { vkFreeMemory      (device, memory_, nullptr); memory_ = VK_NULL_HANDLE; }
    size_ = 0u; w_ = 0u; h_ = 0u; fmt_ = VK_FORMAT_UNDEFINED;
}

// ─────────────────────────────────────────────────────────────────────────────
// export_handle_* — platform-specific OS handle export
// ─────────────────────────────────────────────────────────────────────────────
#ifdef _WIN32
bool ExternalImage::export_handle_win32(VkDevice device,
                                        void**   out_handle) const noexcept
{
    if (!valid() || !out_handle || device == VK_NULL_HANDLE) return false;
    *out_handle = nullptr;

    // Resolve the extension function pointer. We DON'T cache it on the
    // class because (a) different VkDevices may have different proc
    // tables and (b) the lookup is one-shot per image — cost is
    // negligible compared to the export itself.
    auto pfn_get_handle = reinterpret_cast<PFN_vkGetMemoryWin32HandleKHR>(
        vkGetDeviceProcAddr(device, "vkGetMemoryWin32HandleKHR"));
    if (!pfn_get_handle) {
        std::fprintf(stderr,
            "[ExternalImage] vkGetMemoryWin32HandleKHR not loaded — "
            "VK_KHR_external_memory_win32 may not be enabled on the device\n");
        return false;
    }

    VkMemoryGetWin32HandleInfoKHR info{};
    info.sType      = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR;
    info.memory     = memory_;
    info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    HANDLE h = nullptr;
    const VkResult r = pfn_get_handle(device, &info, &h);
    if (r != VK_SUCCESS) {
        log_vk_err("vkGetMemoryWin32HandleKHR", r);
        return false;
    }
    if (h == nullptr || h == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr,
            "[ExternalImage] driver returned VK_SUCCESS but handle is null\n");
        return false;
    }
    *out_handle = static_cast<void*>(h);
    return true;
}
#else
bool ExternalImage::export_handle_fd(VkDevice device,
                                     int*     out_fd) const noexcept
{
    if (!valid() || !out_fd || device == VK_NULL_HANDLE) return false;
    *out_fd = -1;

    auto pfn_get_fd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(
        vkGetDeviceProcAddr(device, "vkGetMemoryFdKHR"));
    if (!pfn_get_fd) {
        std::fprintf(stderr,
            "[ExternalImage] vkGetMemoryFdKHR not loaded — "
            "VK_KHR_external_memory_fd may not be enabled on the device\n");
        return false;
    }

    VkMemoryGetFdInfoKHR info{};
    info.sType      = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    info.memory     = memory_;
    info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

    int fd = -1;
    const VkResult r = pfn_get_fd(device, &info, &fd);
    if (r != VK_SUCCESS) {
        log_vk_err("vkGetMemoryFdKHR", r);
        return false;
    }
    if (fd < 0) {
        std::fprintf(stderr,
            "[ExternalImage] driver returned VK_SUCCESS but fd is negative\n");
        return false;
    }
    *out_fd = fd;
    return true;
}
#endif

} // namespace phyriad::render::vulkan
// Made with my soul - Swately <3
