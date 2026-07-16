// framework/render/vulkan/include/phyriad/render/vulkan/OffscreenScene.hpp
// OffscreenScene — minimal Vulkan render target wrapping an externally
// provided VkImage. Used by CompositeBackend to draw the "scene layer"
// directly into the image whose memory is shared with OpenGL.
//
// Unlike VulkanBackend (which owns a Swapchain tied to the GLFW
// surface) OffscreenScene is presentation-agnostic — the caller passes
// in a VkImage + format + extent and OffscreenScene records draw
// commands targeting it. The caller is responsible for submitting +
// fencing.
//
// Minimum scene supported in v1.0:
//   - Clear-color (always).
//   - Optional 8×8 checker overlay (so the visual verifier can spot
//     image-orientation bugs that a uniform clear would hide).
//
// Larger scenes (3-D meshes, full ImGui-on-Vulkan, etc.) are a v1.1
// expansion — at that point this class will gain begin_pass/end_pass
// methods that expose the active command buffer to user-supplied draw
// callbacks.
//
#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>

namespace phyriad::render::vulkan {

class OffscreenScene {
public:
    struct ClearColor {
        float r{0.0f}, g{0.0f}, b{0.0f}, a{1.0f};
    };

    OffscreenScene()  noexcept = default;
    ~OffscreenScene() noexcept = default;

    OffscreenScene(OffscreenScene const&)            = delete;
    OffscreenScene& operator=(OffscreenScene const&) = delete;
    OffscreenScene(OffscreenScene&&)                 = delete;
    OffscreenScene& operator=(OffscreenScene&&)      = delete;

    // ── init ──────────────────────────────────────────────────────────
    // Creates a render pass + framebuffer targeting the supplied image.
    // The image MUST have been created with VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
    // (Phyriad's ExternalImage default usage covers this).
    [[nodiscard]] bool init(VkDevice    device,
                            VkImage     target_image,
                            VkImageView target_view,
                            VkFormat    format,
                            uint32_t    width,
                            uint32_t    height) noexcept;

    void shutdown(VkDevice device) noexcept;
    [[nodiscard]] bool initialized() const noexcept { return framebuffer_ != VK_NULL_HANDLE; }

    // ── record_clear ──────────────────────────────────────────────────
    // Records into `cmd` a render-pass begin/end pair that clears the
    // target image to `color`. The caller is responsible for:
    //   - Transitioning the image FROM whatever layout it was in to
    //     COLOR_ATTACHMENT_OPTIMAL BEFORE this call.
    //   - Transitioning the image to a downstream-friendly layout
    //     (typically SHADER_READ_ONLY_OPTIMAL or GENERAL for GL
    //     interop) AFTER this call.
    void record_clear(VkCommandBuffer cmd, ClearColor color) noexcept;

    // ── record_checker ────────────────────────────────────────────────
    // Records a clear (to `bg`) followed by an 8×8 checker overlay
    // (toggling the clear-color between bg and fg). Implemented as
    // multiple scissored clears — no graphics pipeline needed, so the
    // shader-compilation surface stays small for the v1.0 demo.
    void record_checker(VkCommandBuffer cmd,
                        ClearColor bg, ClearColor fg) noexcept;

    [[nodiscard]] VkRenderPass  render_pass()  const noexcept { return render_pass_; }
    [[nodiscard]] VkFramebuffer framebuffer()  const noexcept { return framebuffer_; }
    [[nodiscard]] uint32_t      width()        const noexcept { return w_; }
    [[nodiscard]] uint32_t      height()       const noexcept { return h_; }

private:
    [[nodiscard]] bool create_render_pass(VkDevice device, VkFormat format) noexcept;
    [[nodiscard]] bool create_framebuffer(VkDevice device,
                                          VkImageView view,
                                          uint32_t w, uint32_t h) noexcept;

    VkDevice      device_      {VK_NULL_HANDLE};
    VkRenderPass  render_pass_ {VK_NULL_HANDLE};
    VkFramebuffer framebuffer_ {VK_NULL_HANDLE};
    uint32_t      w_           {0u};
    uint32_t      h_           {0u};
};

} // namespace phyriad::render::vulkan
// Made with my soul - Swately <3
