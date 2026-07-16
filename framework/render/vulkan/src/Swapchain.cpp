// framework/render/vulkan/src/Swapchain.cpp
// Swapchain — implementación de init/recreate/shutdown y frame API.
//
// Flujo de init:
//   1. Query surface capabilities, formats, present modes
//   2. Create VkSwapchainKHR (B8G8R8A8_UNORM, FIFO)
//   3. Get swapchain images, create image views
//   4. Create render pass (single color attachment, PRESENT_SRC_KHR final layout)
//   5. Create framebuffers (uno por imagen del swapchain)
//   6. Create command pool + command buffers (kMaxFramesInFlight)
//   7. Create sync objects: semaphores (image_available, render_finished)
//      + fences (in_flight) × kMaxFramesInFlight
//
#include <phyriad/render/vulkan/Swapchain.hpp>
#include <phyriad/render/vulkan/VulkanContext.hpp>
#include <cstdio>
#include <algorithm>
#include <climits>

namespace phyriad::render::vulkan {

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static bool check_vk(VkResult r, const char* op) noexcept {
    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "[Swapchain] %s → VkResult %d\n", op, (int)r);
        return false;
    }
    return true;
}

// Seleccionar el mejor formato de imagen del swapchain.
// Preferimos VK_FORMAT_B8G8R8A8_UNORM con SRGB color space.
static VkSurfaceFormatKHR choose_format(
    const VkSurfaceFormatKHR* fmts, uint32_t count) noexcept
{
    for (uint32_t i = 0u; i < count; ++i) {
        if (fmts[i].format     == VK_FORMAT_B8G8R8A8_UNORM &&
            fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            return fmts[i];
    }
    // Fallback: primer formato disponible.
    return (count > 0u) ? fmts[0] : VkSurfaceFormatKHR{};
}

// Seleccionar modo de presentación.
// FIFO = vsync estricto, siempre disponible según la spec.
static VkPresentModeKHR choose_present_mode(
    const VkPresentModeKHR* modes, uint32_t count) noexcept
{
    // Intentar MAILBOX (triple buffering sin espera de vsync completa).
    for (uint32_t i = 0u; i < count; ++i) {
        if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR)
            return VK_PRESENT_MODE_MAILBOX_KHR;
    }
    // Fallback garantizado por spec.
    return VK_PRESENT_MODE_FIFO_KHR;
}

// Calcular la extent del swapchain respetando las capacidades de la superficie.
static VkExtent2D choose_extent(
    const VkSurfaceCapabilitiesKHR& caps,
    uint32_t desired_w, uint32_t desired_h) noexcept
{
    if (caps.currentExtent.width != UINT32_MAX) {
        return caps.currentExtent;
    }
    VkExtent2D ext{};
    ext.width  = std::clamp(desired_w,
                            caps.minImageExtent.width,
                            caps.maxImageExtent.width);
    ext.height = std::clamp(desired_h,
                            caps.minImageExtent.height,
                            caps.maxImageExtent.height);
    return ext;
}

// ─────────────────────────────────────────────────────────────────────────────
// Swapchain — constructor / destructor
// ─────────────────────────────────────────────────────────────────────────────
Swapchain::Swapchain()  noexcept = default;
Swapchain::~Swapchain() noexcept { shutdown(); }

// ─────────────────────────────────────────────────────────────────────────────
// init
// ─────────────────────────────────────────────────────────────────────────────
std::expected<void, phyriad::Error>
Swapchain::init(VulkanContext* ctx, uint32_t width, uint32_t height) noexcept
{
    ctx_ = ctx;

    if (!create_swapchain(width, height))          goto fail;
    if (!create_image_views())                     goto fail;
    if (!create_render_pass())                     goto fail;
    if (!create_framebuffers())                    goto fail;
    if (!create_command_pool_and_buffers())        goto fail;
    if (!create_sync_objects())                    goto fail;

    initialized_ = true;
    return {};

fail:
    shutdown();
    return std::unexpected(phyriad::Error{
        .code           = phyriad::ErrorCode::ResourceInitFailed,
        .source_node_id = 0,
        .timestamp_ns   = 0});
}

// ─────────────────────────────────────────────────────────────────────────────
// recreate
// ─────────────────────────────────────────────────────────────────────────────
std::expected<void, phyriad::Error>
Swapchain::recreate(uint32_t new_width, uint32_t new_height) noexcept
{
    if (!ctx_) {
        return std::unexpected(phyriad::Error{
            .code = phyriad::ErrorCode::InvalidHandle, .source_node_id = 0, .timestamp_ns = 0});
    }
    ctx_->device_wait_idle();
    destroy_swapchain_resources();

    if (!create_swapchain(new_width, new_height))  goto fail;
    if (!create_image_views())                     goto fail;
    if (!create_framebuffers())                    goto fail;

    return {};

fail:
    return std::unexpected(phyriad::Error{
        .code           = phyriad::ErrorCode::ResourceInitFailed,
        .source_node_id = 0,
        .timestamp_ns   = 0});
}

// ─────────────────────────────────────────────────────────────────────────────
// shutdown
// ─────────────────────────────────────────────────────────────────────────────
void Swapchain::shutdown() noexcept
{
    if (!ctx_) return;

    VkDevice dev = ctx_->device();
    if (dev == VK_NULL_HANDLE) return;

    vkDeviceWaitIdle(dev);

    // Sync objects
    for (uint32_t i = 0u; i < kMaxFramesInFlight; ++i) {
        if (in_flight_[i] != VK_NULL_HANDLE) {
            vkDestroyFence(dev, in_flight_[i], nullptr);
            in_flight_[i] = VK_NULL_HANDLE;
        }
        if (render_finished_[i] != VK_NULL_HANDLE) {
            vkDestroySemaphore(dev, render_finished_[i], nullptr);
            render_finished_[i] = VK_NULL_HANDLE;
        }
        if (image_available_[i] != VK_NULL_HANDLE) {
            vkDestroySemaphore(dev, image_available_[i], nullptr);
            image_available_[i] = VK_NULL_HANDLE;
        }
    }

    if (cmd_pool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(dev, cmd_pool_, nullptr);
        cmd_pool_ = VK_NULL_HANDLE;
    }

    destroy_swapchain_resources();

    if (render_pass_ != VK_NULL_HANDLE) {
        vkDestroyRenderPass(dev, render_pass_, nullptr);
        render_pass_ = VK_NULL_HANDLE;
    }

    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(dev, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }

    initialized_   = false;
    current_frame_ = 0u;
    image_count_   = 0u;
    ctx_           = nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// destroy_swapchain_resources — framebuffers + image views (no render pass)
// ─────────────────────────────────────────────────────────────────────────────
void Swapchain::destroy_swapchain_resources() noexcept
{
    if (!ctx_) return;
    VkDevice dev = ctx_->device();
    if (dev == VK_NULL_HANDLE) return;

    for (uint32_t i = 0u; i < image_count_; ++i) {
        if (framebuffers_[i] != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(dev, framebuffers_[i], nullptr);
            framebuffers_[i] = VK_NULL_HANDLE;
        }
        if (image_views_[i] != VK_NULL_HANDLE) {
            vkDestroyImageView(dev, image_views_[i], nullptr);
            image_views_[i] = VK_NULL_HANDLE;
        }
        images_[i] = VK_NULL_HANDLE; // propiedad del runtime — NO destruir
    }
    image_count_ = 0u;
}

// ─────────────────────────────────────────────────────────────────────────────
// create_swapchain
// ─────────────────────────────────────────────────────────────────────────────
bool Swapchain::create_swapchain(uint32_t w, uint32_t h) noexcept
{
    VkPhysicalDevice phys = ctx_->physical_device();
    VkDevice         dev  = ctx_->device();
    VkSurfaceKHR     surf = ctx_->surface();

    // ── Surface capabilities ───────────────────────────────────────────────
    VkSurfaceCapabilitiesKHR caps{};
    if (!check_vk(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surf, &caps),
                  "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"))
        return false;

    // ── Surface formats ────────────────────────────────────────────────────
    uint32_t fmt_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surf, &fmt_count, nullptr);
    VkSurfaceFormatKHR fmts[64];
    if (fmt_count > 64u) fmt_count = 64u;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surf, &fmt_count, fmts);
    const VkSurfaceFormatKHR surface_fmt = choose_format(fmts, fmt_count);
    image_format_ = surface_fmt.format;

    // ── Present modes ──────────────────────────────────────────────────────
    uint32_t pm_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surf, &pm_count, nullptr);
    VkPresentModeKHR pms[8];
    if (pm_count > 8u) pm_count = 8u;
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surf, &pm_count, pms);
    const VkPresentModeKHR present_mode = choose_present_mode(pms, pm_count);

    // ── Extent ─────────────────────────────────────────────────────────────
    const VkExtent2D extent = choose_extent(caps, w, h);
    width_  = extent.width;
    height_ = extent.height;

    // Solicitar una imagen más que el mínimo para permitir triple buffering.
    uint32_t image_count = caps.minImageCount + 1u;
    if (caps.maxImageCount > 0u && image_count > caps.maxImageCount)
        image_count = caps.maxImageCount;
    if (image_count > kMaxImages) image_count = kMaxImages;

    // ── Create swapchain ───────────────────────────────────────────────────
    VkSwapchainCreateInfoKHR ci{};
    ci.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface          = surf;
    ci.minImageCount    = image_count;
    ci.imageFormat      = surface_fmt.format;
    ci.imageColorSpace  = surface_fmt.colorSpace;
    ci.imageExtent      = extent;
    ci.imageArrayLayers = 1u;
    ci.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.preTransform     = caps.currentTransform;
    ci.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode      = present_mode;
    ci.clipped          = VK_TRUE;
    ci.oldSwapchain     = swapchain_; // pass old swapchain for efficient recreate

    const uint32_t queue_families[] = { ctx_->graphics_family(), ctx_->present_family() };
    if (ctx_->graphics_family() != ctx_->present_family()) {
        ci.imageSharingMode      = VK_SHARING_MODE_CONCURRENT;
        ci.queueFamilyIndexCount = 2u;
        ci.pQueueFamilyIndices   = queue_families;
    } else {
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    VkSwapchainKHR new_swapchain = VK_NULL_HANDLE;
    if (!check_vk(vkCreateSwapchainKHR(dev, &ci, nullptr, &new_swapchain),
                  "vkCreateSwapchainKHR"))
        return false;

    // Destruir el swapchain viejo DESPUÉS de crear el nuevo.
    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(dev, swapchain_, nullptr);
    }
    swapchain_ = new_swapchain;

    // ── Get swapchain images ───────────────────────────────────────────────
    image_count_ = 0u;
    if (!check_vk(vkGetSwapchainImagesKHR(dev, swapchain_, &image_count_, nullptr),
                  "vkGetSwapchainImagesKHR (count)"))
        return false;
    if (image_count_ > kMaxImages) image_count_ = kMaxImages;

    return check_vk(vkGetSwapchainImagesKHR(dev, swapchain_, &image_count_, images_),
                    "vkGetSwapchainImagesKHR (images)");
}

// ─────────────────────────────────────────────────────────────────────────────
// create_image_views
// ─────────────────────────────────────────────────────────────────────────────
bool Swapchain::create_image_views() noexcept
{
    VkDevice dev = ctx_->device();

    for (uint32_t i = 0u; i < image_count_; ++i) {
        VkImageViewCreateInfo ci{};
        ci.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ci.image                           = images_[i];
        ci.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        ci.format                          = image_format_;
        ci.components.r                    = VK_COMPONENT_SWIZZLE_IDENTITY;
        ci.components.g                    = VK_COMPONENT_SWIZZLE_IDENTITY;
        ci.components.b                    = VK_COMPONENT_SWIZZLE_IDENTITY;
        ci.components.a                    = VK_COMPONENT_SWIZZLE_IDENTITY;
        ci.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        ci.subresourceRange.baseMipLevel   = 0u;
        ci.subresourceRange.levelCount     = 1u;
        ci.subresourceRange.baseArrayLayer = 0u;
        ci.subresourceRange.layerCount     = 1u;

        if (!check_vk(vkCreateImageView(dev, &ci, nullptr, &image_views_[i]),
                      "vkCreateImageView"))
            return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// create_render_pass
// ─────────────────────────────────────────────────────────────────────────────
bool Swapchain::create_render_pass() noexcept
{
    // Single color attachment — layout transition UNDEFINED → PRESENT_SRC_KHR.
    VkAttachmentDescription color_att{};
    color_att.format         = image_format_;
    color_att.samples        = VK_SAMPLE_COUNT_1_BIT;
    color_att.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    color_att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color_att.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    color_att.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference color_ref{};
    color_ref.attachment = 0u;
    color_ref.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1u;
    subpass.pColorAttachments    = &color_ref;

    // Dependencia de subpass para sincronizar la adquisición de la imagen.
    VkSubpassDependency dep{};
    dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass    = 0u;
    dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0u;
    dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo ci{};
    ci.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci.attachmentCount = 1u;
    ci.pAttachments    = &color_att;
    ci.subpassCount    = 1u;
    ci.pSubpasses      = &subpass;
    ci.dependencyCount = 1u;
    ci.pDependencies   = &dep;

    return check_vk(vkCreateRenderPass(ctx_->device(), &ci, nullptr, &render_pass_),
                    "vkCreateRenderPass");
}

// ─────────────────────────────────────────────────────────────────────────────
// create_framebuffers
// ─────────────────────────────────────────────────────────────────────────────
bool Swapchain::create_framebuffers() noexcept
{
    VkDevice dev = ctx_->device();

    for (uint32_t i = 0u; i < image_count_; ++i) {
        VkFramebufferCreateInfo ci{};
        ci.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        ci.renderPass      = render_pass_;
        ci.attachmentCount = 1u;
        ci.pAttachments    = &image_views_[i];
        ci.width           = width_;
        ci.height          = height_;
        ci.layers          = 1u;

        if (!check_vk(vkCreateFramebuffer(dev, &ci, nullptr, &framebuffers_[i]),
                      "vkCreateFramebuffer"))
            return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// create_command_pool_and_buffers
// ─────────────────────────────────────────────────────────────────────────────
bool Swapchain::create_command_pool_and_buffers() noexcept
{
    VkDevice dev = ctx_->device();

    VkCommandPoolCreateInfo pool_ci{};
    pool_ci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_ci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_ci.queueFamilyIndex = ctx_->graphics_family();

    if (!check_vk(vkCreateCommandPool(dev, &pool_ci, nullptr, &cmd_pool_),
                  "vkCreateCommandPool"))
        return false;

    VkCommandBufferAllocateInfo alloc_ci{};
    alloc_ci.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_ci.commandPool        = cmd_pool_;
    alloc_ci.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_ci.commandBufferCount = kMaxFramesInFlight;

    return check_vk(vkAllocateCommandBuffers(dev, &alloc_ci, cmd_bufs_),
                    "vkAllocateCommandBuffers");
}

// ─────────────────────────────────────────────────────────────────────────────
// create_sync_objects
// ─────────────────────────────────────────────────────────────────────────────
bool Swapchain::create_sync_objects() noexcept
{
    VkDevice dev = ctx_->device();

    VkSemaphoreCreateInfo sem_ci{};
    sem_ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fence_ci{};
    fence_ci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_ci.flags = VK_FENCE_CREATE_SIGNALED_BIT; // pre-signaled para el primer frame

    for (uint32_t i = 0u; i < kMaxFramesInFlight; ++i) {
        if (!check_vk(vkCreateSemaphore(dev, &sem_ci, nullptr, &image_available_[i]),
                      "vkCreateSemaphore (image_available)"))
            return false;
        if (!check_vk(vkCreateSemaphore(dev, &sem_ci, nullptr, &render_finished_[i]),
                      "vkCreateSemaphore (render_finished)"))
            return false;
        if (!check_vk(vkCreateFence(dev, &fence_ci, nullptr, &in_flight_[i]),
                      "vkCreateFence"))
            return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// acquire_next_image
// ─────────────────────────────────────────────────────────────────────────────
std::expected<uint32_t, phyriad::Error>
Swapchain::acquire_next_image() noexcept
{
    VkDevice dev = ctx_->device();
    const uint32_t frame = current_frame_;

    // Esperar a que el frame anterior haya terminado.
    vkWaitForFences(dev, 1u, &in_flight_[frame], VK_TRUE, UINT64_MAX);
    vkResetFences(dev, 1u, &in_flight_[frame]);

    uint32_t image_index = 0u;
    const VkResult r = vkAcquireNextImageKHR(
        dev, swapchain_, UINT64_MAX,
        image_available_[frame], VK_NULL_HANDLE, &image_index);

    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        // Swapchain desactualizado — el caller debe llamar a recreate().
        return std::unexpected(phyriad::Error{
            .code           = phyriad::ErrorCode::ResourceInitFailed,
            .source_node_id = 0,
            .timestamp_ns   = 0});
    }
    if (!check_vk(r, "vkAcquireNextImageKHR")) {
        return std::unexpected(phyriad::Error{
            .code           = phyriad::ErrorCode::ResourceInitFailed,
            .source_node_id = 0,
            .timestamp_ns   = 0});
    }

    // Reset del command buffer del frame actual para reutilizarlo.
    vkResetCommandBuffer(cmd_bufs_[frame], 0u);

    return image_index;
}

// ─────────────────────────────────────────────────────────────────────────────
// submit_and_present
// ─────────────────────────────────────────────────────────────────────────────
std::expected<void, phyriad::Error>
Swapchain::submit_and_present(uint32_t image_index) noexcept
{
    const uint32_t frame = current_frame_;

    // ── Submit ────────────────────────────────────────────────────────────────
    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSubmitInfo submit{};
    submit.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount   = 1u;
    submit.pWaitSemaphores      = &image_available_[frame];
    submit.pWaitDstStageMask    = &wait_stage;
    submit.commandBufferCount   = 1u;
    submit.pCommandBuffers      = &cmd_bufs_[frame];
    submit.signalSemaphoreCount = 1u;
    submit.pSignalSemaphores    = &render_finished_[frame];

    if (!check_vk(vkQueueSubmit(ctx_->graphics_queue(), 1u, &submit, in_flight_[frame]),
                  "vkQueueSubmit"))
    {
        return std::unexpected(phyriad::Error{
            .code = phyriad::ErrorCode::ResourceInitFailed, .source_node_id = 0, .timestamp_ns = 0});
    }

    // ── Present ───────────────────────────────────────────────────────────────
    VkPresentInfoKHR present{};
    present.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1u;
    present.pWaitSemaphores    = &render_finished_[frame];
    present.swapchainCount     = 1u;
    present.pSwapchains        = &swapchain_;
    present.pImageIndices      = &image_index;

    const VkResult r = vkQueuePresentKHR(ctx_->present_queue(), &present);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        // Se notifica al caller para que recree el swapchain si lo desea.
        // No se considera error fatal aquí — es esperado en resize.
    } else if (!check_vk(r, "vkQueuePresentKHR")) {
        return std::unexpected(phyriad::Error{
            .code = phyriad::ErrorCode::ResourceInitFailed, .source_node_id = 0, .timestamp_ns = 0});
    }

    // Avanzar al siguiente frame en vuelo.
    current_frame_ = (current_frame_ + 1u) % kMaxFramesInFlight;
    return {};
}

} // namespace phyriad::render::vulkan
// Made with my soul - Swately <3
