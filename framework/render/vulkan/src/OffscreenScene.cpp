// framework/render/vulkan/src/OffscreenScene.cpp
// Implementation — see header for the contract.
//
#include <phyriad/render/vulkan/OffscreenScene.hpp>
#include <cstdio>

namespace phyriad::render::vulkan {

bool OffscreenScene::create_render_pass(VkDevice device, VkFormat format) noexcept
{
    VkAttachmentDescription color{};
    color.format         = format;
    color.samples        = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    // The caller transitions the image into COLOR_ATTACHMENT_OPTIMAL
    // BEFORE record_clear; the render pass keeps it there for the duration
    // and the caller transitions it out afterwards.
    color.initialLayout  = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference color_ref{};
    color_ref.attachment = 0u;
    color_ref.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1u;
    subpass.pColorAttachments    = &color_ref;

    // External dependency: the previous-frame's compute / transfer write
    // must complete before the render pass starts loading the attachment.
    VkSubpassDependency dep{};
    dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass    = 0u;
    dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0u;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rp_ci{};
    rp_ci.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp_ci.attachmentCount = 1u;
    rp_ci.pAttachments    = &color;
    rp_ci.subpassCount    = 1u;
    rp_ci.pSubpasses      = &subpass;
    rp_ci.dependencyCount = 1u;
    rp_ci.pDependencies   = &dep;

    if (vkCreateRenderPass(device, &rp_ci, nullptr, &render_pass_) != VK_SUCCESS) {
        std::fprintf(stderr, "[OffscreenScene] vkCreateRenderPass failed\n");
        return false;
    }
    return true;
}

bool OffscreenScene::create_framebuffer(VkDevice device,
                                        VkImageView view,
                                        uint32_t w, uint32_t h) noexcept
{
    VkFramebufferCreateInfo fb_ci{};
    fb_ci.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb_ci.renderPass      = render_pass_;
    fb_ci.attachmentCount = 1u;
    fb_ci.pAttachments    = &view;
    fb_ci.width           = w;
    fb_ci.height          = h;
    fb_ci.layers          = 1u;
    if (vkCreateFramebuffer(device, &fb_ci, nullptr, &framebuffer_) != VK_SUCCESS) {
        std::fprintf(stderr, "[OffscreenScene] vkCreateFramebuffer failed\n");
        return false;
    }
    return true;
}

bool OffscreenScene::init(VkDevice    device,
                          VkImage     target_image,
                          VkImageView target_view,
                          VkFormat    format,
                          uint32_t    width,
                          uint32_t    height) noexcept
{
    if (initialized()) return true;
    if (device == VK_NULL_HANDLE || target_image == VK_NULL_HANDLE ||
        target_view == VK_NULL_HANDLE || width == 0u || height == 0u)
    {
        std::fprintf(stderr, "[OffscreenScene] init: invalid arguments\n");
        return false;
    }
    (void)target_image;   // we only need the view for the framebuffer

    device_ = device;
    w_      = width;
    h_      = height;

    if (!create_render_pass(device, format))             { shutdown(device); return false; }
    if (!create_framebuffer(device, target_view, w_, h_)) { shutdown(device); return false; }
    return true;
}

void OffscreenScene::shutdown(VkDevice device) noexcept {
    if (device == VK_NULL_HANDLE) device = device_;
    if (device == VK_NULL_HANDLE) return;
    if (framebuffer_ != VK_NULL_HANDLE) { vkDestroyFramebuffer(device, framebuffer_, nullptr); framebuffer_ = VK_NULL_HANDLE; }
    if (render_pass_ != VK_NULL_HANDLE) { vkDestroyRenderPass (device, render_pass_, nullptr); render_pass_ = VK_NULL_HANDLE; }
    w_ = h_ = 0u;
    device_ = VK_NULL_HANDLE;
}

void OffscreenScene::record_clear(VkCommandBuffer cmd, ClearColor c) noexcept
{
    if (!initialized() || cmd == VK_NULL_HANDLE) return;

    VkClearValue clear{};
    clear.color.float32[0] = c.r;
    clear.color.float32[1] = c.g;
    clear.color.float32[2] = c.b;
    clear.color.float32[3] = c.a;

    VkRenderPassBeginInfo bi{};
    bi.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    bi.renderPass        = render_pass_;
    bi.framebuffer       = framebuffer_;
    bi.renderArea.offset = {0, 0};
    bi.renderArea.extent = {w_, h_};
    bi.clearValueCount   = 1u;
    bi.pClearValues      = &clear;

    vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
    // No additional draws — the loadOp=CLEAR already paints the
    // attachment to `c` before the (empty) subpass body runs.
    vkCmdEndRenderPass(cmd);
}

void OffscreenScene::record_checker(VkCommandBuffer cmd,
                                    ClearColor bg, ClearColor fg) noexcept
{
    if (!initialized() || cmd == VK_NULL_HANDLE) return;

    // Stage 1: clear the whole image to bg.
    record_clear(cmd, bg);

    // Stage 2: scissored clears of the fg-colored squares of an 8×8 grid.
    //
    // We use vkCmdClearAttachments INSIDE a freshly-begun render pass so
    // that the operation is bound to the same framebuffer. Each "fg"
    // square is one rectangle in the rects array.
    constexpr uint32_t kGrid = 8u;
    const uint32_t cell_w = w_ / kGrid;
    const uint32_t cell_h = h_ / kGrid;
    if (cell_w == 0u || cell_h == 0u) return;   // image too small for a checker

    VkClearAttachment att{};
    att.aspectMask                  = VK_IMAGE_ASPECT_COLOR_BIT;
    att.colorAttachment             = 0u;
    att.clearValue.color.float32[0] = fg.r;
    att.clearValue.color.float32[1] = fg.g;
    att.clearValue.color.float32[2] = fg.b;
    att.clearValue.color.float32[3] = fg.a;

    // Build the list of fg-cell rectangles (alternating chess pattern).
    VkClearRect rects[kGrid * kGrid / 2u + kGrid];   // ≤32 fg cells, headroom
    uint32_t n_rects = 0u;
    for (uint32_t gy = 0u; gy < kGrid; ++gy) {
        for (uint32_t gx = 0u; gx < kGrid; ++gx) {
            if (((gx + gy) & 1u) == 0u) continue;  // bg cell — skip
            VkClearRect& r = rects[n_rects++];
            r.rect.offset = { static_cast<int32_t>(gx * cell_w),
                              static_cast<int32_t>(gy * cell_h) };
            r.rect.extent = { cell_w, cell_h };
            r.baseArrayLayer = 0u;
            r.layerCount     = 1u;
        }
    }

    // Begin a second render-pass instance to host the clear-attachments.
    // Since loadOp=CLEAR would wipe our bg clear, use loadOp=LOAD via a
    // second render pass — but creating that here is overkill. Instead
    // we begin the same render pass with DONT_CARE behavior on load by
    // using INLINE clear-attachments after begin.
    //
    // Simplest correct approach: begin the same render pass with a
    // dummy clear value (the bg again, so it's idempotent), then
    // vkCmdClearAttachments to override the fg cells.
    VkClearValue dummy{};
    dummy.color.float32[0] = bg.r;
    dummy.color.float32[1] = bg.g;
    dummy.color.float32[2] = bg.b;
    dummy.color.float32[3] = bg.a;

    VkRenderPassBeginInfo bi{};
    bi.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    bi.renderPass        = render_pass_;
    bi.framebuffer       = framebuffer_;
    bi.renderArea.offset = {0, 0};
    bi.renderArea.extent = {w_, h_};
    bi.clearValueCount   = 1u;
    bi.pClearValues      = &dummy;
    vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdClearAttachments(cmd, 1u, &att, n_rects, rects);
    vkCmdEndRenderPass(cmd);
}

} // namespace phyriad::render::vulkan
// Made with my soul - Swately <3
