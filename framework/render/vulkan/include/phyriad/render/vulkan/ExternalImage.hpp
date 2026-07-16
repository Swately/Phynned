// framework/render/vulkan/include/phyriad/render/vulkan/ExternalImage.hpp
// ExternalImage — Vulkan image with memory exportable to OpenGL via
// the GL_EXT_memory_object extension.
//
// Used by CompositeBackend to give the Vulkan side an offscreen render
// target whose memory can be imported into an OpenGL texture without a
// CPU round-trip. The Vulkan side draws into the image; the OpenGL side
// samples it as a fullscreen quad, optionally with an ImGui overlay
// composited on top, and presents via glfwSwapBuffers.
//
// Platform contract:
//   Windows: handle type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT
//            export via vkGetMemoryWin32HandleKHR → HANDLE (owns lifetime)
//   POSIX:   handle type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT
//            export via vkGetMemoryFdKHR → int fd (owns lifetime)
//
// Prerequisite: the VkDevice MUST have been created with the
// corresponding KHR_external_memory_{win32,fd} extension enabled.
// VulkanContext::external_memory_supported() reports whether this is
// the case. ExternalImage::init() returns false otherwise.
//
#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>

namespace phyriad::render::vulkan {

class ExternalImage {
public:
    // ── Construction parameters ───────────────────────────────────────
    struct Config {
        uint32_t          width  = 0u;
        uint32_t          height = 0u;
        // RGBA8 UNORM matches GL_RGBA8 — the natural format for VK↔GL
        // texture interop. Override only if you specifically need a
        // different format and have verified GL can import it.
        VkFormat          format = VK_FORMAT_R8G8B8A8_UNORM;
        // Default usage covers: color attachment (render-to), sampled
        // (read from compute / fragment), transfer src (readback), and
        // storage (compute write-out). The image is created with all
        // these bits so downstream callers don't need to repeat the
        // negotiation. They're cheap on a single image allocation.
        VkImageUsageFlags usage  = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                                 | VK_IMAGE_USAGE_SAMPLED_BIT
                                 | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                                 | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                                 | VK_IMAGE_USAGE_STORAGE_BIT;
        // Tiling. OPTIMAL is required for VK↔GL interop on NVIDIA: the
        // NVIDIA GL driver rejects GL_LINEAR_TILING_EXT for imported
        // memory (GL_INVALID_VALUE), so the GL side must use
        // GL_OPTIMAL_TILING_EXT, which in turn requires the VK side to
        // declare OPTIMAL tiling. Correctness for sampling — not just
        // FBO readback — depends on the OUT barrier releasing queue
        // ownership to VK_QUEUE_FAMILY_EXTERNAL so the driver
        // decompresses any framebuffer-compression metadata before GL
        // reads the memory. See composite_demo/main.cpp for the
        // canonical hand-off pattern.
        VkImageTiling     tiling = VK_IMAGE_TILING_OPTIMAL;
    };

    ExternalImage()  noexcept = default;
    ~ExternalImage() noexcept = default;

    ExternalImage(ExternalImage const&)            = delete;
    ExternalImage& operator=(ExternalImage const&) = delete;
    ExternalImage(ExternalImage&&)                 = delete;
    ExternalImage& operator=(ExternalImage&&)      = delete;

    // ── Lifecycle ─────────────────────────────────────────────────────
    // Creates the image + memory + view. The caller's device must have
    // been created with VK_KHR_external_memory_{win32,fd} enabled.
    //
    // Returns false on any failure (extension absent, allocation
    // failed, etc.) with all partial objects torn down — the instance
    // is left in its initial state.
    [[nodiscard]] bool init(VkPhysicalDevice phys_dev,
                            VkDevice         device,
                            Config const&    cfg) noexcept;

    // Destroys all Vulkan objects. Idempotent. NOTE: any platform OS
    // handle previously obtained from export_handle_*() is OWNED BY
    // THE CALLER — shutdown() does NOT close it. The caller must
    // CloseHandle / close() the OS handle when the GL texture is done
    // with it.
    void shutdown(VkDevice device) noexcept;

    [[nodiscard]] bool valid() const noexcept { return image_ != VK_NULL_HANDLE; }

    // ── Accessors ────────────────────────────────────────────────────
    [[nodiscard]] VkImage      image()       const noexcept { return image_; }
    [[nodiscard]] VkImageView  view()        const noexcept { return view_;  }
    [[nodiscard]] VkDeviceSize memory_size() const noexcept { return size_;  }
    [[nodiscard]] uint32_t     width()       const noexcept { return w_;     }
    [[nodiscard]] uint32_t     height()      const noexcept { return h_;     }
    [[nodiscard]] VkFormat     format()      const noexcept { return fmt_;   }

    // ── OS handle export (for GL import) ──────────────────────────────
    // Platform-specific because handle types differ: Windows HANDLE
    // (void*) vs POSIX int fd. On success, the returned handle is
    // OWNED BY THE CALLER — Vulkan does NOT close it on shutdown.
    //
    // Typical lifetime:
    //   img.init(...)             → image + memory allocated
    //   img.export_handle_*(...)  → caller gets the OS handle
    //   gl_import(handle)         → OpenGL takes a reference
    //   { ... use ...             → both VK + GL keep the memory alive
    //   gl_release(texture)       → GL drops its reference
    //   CloseHandle(handle)       → caller releases its reference
    //   img.shutdown(device)      → frees the Vulkan-side memory
    //
    // The OS handle does NOT have to be closed before shutdown — the
    // memory is reference-counted by the OS.
#ifdef _WIN32
    [[nodiscard]] bool export_handle_win32(VkDevice device,
                                           void** out_handle) const noexcept;
#else
    [[nodiscard]] bool export_handle_fd(VkDevice device,
                                        int* out_fd) const noexcept;
#endif

private:
    VkImage        image_ {VK_NULL_HANDLE};
    VkImageView    view_  {VK_NULL_HANDLE};
    VkDeviceMemory memory_{VK_NULL_HANDLE};
    VkDeviceSize   size_  {0u};
    uint32_t       w_     {0u};
    uint32_t       h_     {0u};
    VkFormat       fmt_   {VK_FORMAT_UNDEFINED};
};

} // namespace phyriad::render::vulkan
// Made with my soul - Swately <3
