// framework/render/vulkan/src/VulkanBackend.cpp
// VulkanBackend — implementación del ciclo de frame Vulkan + ImGui.
//
// Flujo de frame (espejo del OpenGL3Backend):
//   new_frame()  → acquire_next_image, begin render pass, ImGui::NewFrame
//   end_frame()  → ImGui::Render, record draw data, end render pass, end cmd buf
//   present()    → submit_and_present, update FPS EMA
//
// Init de ImGui Vulkan:
//   ImGui_ImplGlfw_InitForVulkan + ImGui_ImplVulkan_Init
//   Font upload via single-shot command buffer
//
#include <phyriad/render/vulkan/VulkanBackend.hpp>
#include <phyriad/render/vulkan/VulkanContext.hpp>
#include <phyriad/render/vulkan/Swapchain.hpp>
#include <phyriad/hal/Timestamp.hpp>
#include <vulkan/vulkan.h>

// ImGui backends (después de los headers locales para evitar colisiones de macros)
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <GLFW/glfw3.h>

#include <cstdio>
#include <cstring>

namespace phyriad::render::vulkan {

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static bool check_vk(VkResult r, const char* op) noexcept {
    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "[VulkanBackend] %s → VkResult %d\n", op, (int)r);
        return false;
    }
    return true;
}

// upload_fonts_old_api — usado solo con ImGui < 1.90 donde CreateFontsTexture
// requiere un command buffer explícito de uso único.
#if IMGUI_VERSION_NUM < 19000
static bool upload_fonts_old_api(VkDevice device, VkCommandPool pool,
                                  VkQueue queue) noexcept
{
    VkCommandBufferAllocateInfo alloc_ci{};
    alloc_ci.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_ci.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_ci.commandPool        = pool;
    alloc_ci.commandBufferCount = 1u;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (!check_vk(vkAllocateCommandBuffers(device, &alloc_ci, &cmd),
                  "vkAllocateCommandBuffers (fonts)"))
        return false;

    VkCommandBufferBeginInfo begin_ci{};
    begin_ci.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_ci.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk(vkBeginCommandBuffer(cmd, &begin_ci), "vkBeginCommandBuffer (fonts)");
    ImGui_ImplVulkan_CreateFontsTexture(cmd);
    check_vk(vkEndCommandBuffer(cmd), "vkEndCommandBuffer (fonts)");

    VkSubmitInfo submit{};
    submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1u;
    submit.pCommandBuffers    = &cmd;
    check_vk(vkQueueSubmit(queue, 1u, &submit, VK_NULL_HANDLE), "vkQueueSubmit (fonts)");
    check_vk(vkQueueWaitIdle(queue), "vkQueueWaitIdle (fonts)");

    vkFreeCommandBuffers(device, pool, 1u, &cmd);
    ImGui_ImplVulkan_DestroyFontUploadObjects();
    return true;
}
#endif // IMGUI_VERSION_NUM < 19000

// ─────────────────────────────────────────────────────────────────────────────
// VulkanBackend — constructor / destructor
// ─────────────────────────────────────────────────────────────────────────────
VulkanBackend::VulkanBackend()  noexcept = default;
VulkanBackend::~VulkanBackend() noexcept { shutdown(); }

// ─────────────────────────────────────────────────────────────────────────────
// init
// ─────────────────────────────────────────────────────────────────────────────
std::expected<void, phyriad::Error>
VulkanBackend::init(GLFWwindow* window, FrameArena* arena) noexcept
{
    if (initialized_) shutdown();

    // Verificar soporte Vulkan en el sistema antes de hacer nada más.
    if (!glfwVulkanSupported()) {
        std::fprintf(stderr, "[VulkanBackend] Vulkan not supported on this system\n");
        return std::unexpected(phyriad::Error{
            .code           = phyriad::ErrorCode::ResourceInitFailed,
            .source_node_id = 0,
            .timestamp_ns   = 0});
    }

    window_ = window;
    arena_  = arena;

    // ── VulkanContext ─────────────────────────────────────────────────────────
    ctx_ = new VulkanContext();
    auto ctx_r = ctx_->init(window_);
    if (!ctx_r) {
        std::fprintf(stderr, "[VulkanBackend] VulkanContext::init failed\n");
        goto fail;
    }

    // ── Swapchain ─────────────────────────────────────────────────────────────
    {
        int fb_w = 0, fb_h = 0;
        glfwGetFramebufferSize(window_, &fb_w, &fb_h);

        swap_ = new Swapchain();
        auto swap_r = swap_->init(ctx_,
                                  static_cast<uint32_t>(fb_w > 0 ? fb_w : 1),
                                  static_cast<uint32_t>(fb_h > 0 ? fb_h : 1));
        if (!swap_r) {
            std::fprintf(stderr, "[VulkanBackend] Swapchain::init failed\n");
            goto fail;
        }
    }

    // ── ImGui init ────────────────────────────────────────────────────────────
    if (!ImGui_ImplGlfw_InitForVulkan(window_, /*install_callbacks=*/true)) {
        std::fprintf(stderr, "[VulkanBackend] ImGui_ImplGlfw_InitForVulkan failed\n");
        goto fail;
    }

    {
        ImGui_ImplVulkan_InitInfo vk_info{};
        vk_info.Instance       = ctx_->instance();
        vk_info.PhysicalDevice = ctx_->physical_device();
        vk_info.Device         = ctx_->device();
        vk_info.QueueFamily    = ctx_->graphics_family();
        vk_info.Queue          = ctx_->graphics_queue();
        vk_info.PipelineCache  = VK_NULL_HANDLE;
        vk_info.DescriptorPool = ctx_->descriptor_pool();
        vk_info.Subpass        = 0u;
        vk_info.MinImageCount  = 2u;
        vk_info.ImageCount     = swap_->image_count();
        vk_info.MSAASamples    = VK_SAMPLE_COUNT_1_BIT;
        vk_info.Allocator      = nullptr;

        // Compatibilidad con distintas versiones de ImGui:
        //   ≥ 1.90.3 (IMGUI_VERSION_NUM >= 19003): RenderPass está en InitInfo.
        //   ≥ 1.90.4 (IMGUI_VERSION_NUM >= 19004): ImGui_ImplVulkan_Init solo
        //     toma InitInfo* (sin segundo argumento).
        //   < 1.90.3: ImGui_ImplVulkan_Init(info, VkRenderPass).
#if IMGUI_VERSION_NUM >= 19003
        vk_info.RenderPass = swap_->render_pass();
#endif
        bool vk_init_ok;
#if IMGUI_VERSION_NUM >= 19004
        vk_init_ok = ImGui_ImplVulkan_Init(&vk_info);
#else
        vk_init_ok = ImGui_ImplVulkan_Init(&vk_info, swap_->render_pass());
#endif
        if (!vk_init_ok) {
            std::fprintf(stderr, "[VulkanBackend] ImGui_ImplVulkan_Init failed\n");
            // ImGui_ImplGlfw ya fue inicializado — limpiarlo antes de salir.
            ImGui_ImplGlfw_Shutdown();
            goto fail;
        }
    }

    // ── Font upload — adaptado a la versión de ImGui ─────────────────────────
    // ImGui 1.90+ (IMGUI_VERSION_NUM >= 19000): API sin argumentos.
    // ImGui < 1.90: requiere command buffer explícito de uso único.
    {
        ctx_->device_wait_idle();

#if IMGUI_VERSION_NUM >= 19000
        ImGui_ImplVulkan_CreateFontsTexture();
#else
        upload_fonts_old_api(ctx_->device(), swap_->cmd_pool(),
                             ctx_->graphics_queue());
#endif

        ctx_->device_wait_idle();
    }

    frame_id_          = 0u;
    frame_start_tsc_   = hal::rdtsc();
    last_present_tsc_  = hal::rdtsc();
    fps_ema_           = 0.0f;
    dropped_frames_    = 0u;
    frame_acquired_    = false;
    initialized_       = true;
    return {};

fail:
    shutdown();
    return std::unexpected(phyriad::Error{
        .code           = phyriad::ErrorCode::ResourceInitFailed,
        .source_node_id = 0,
        .timestamp_ns   = 0});
}

// ─────────────────────────────────────────────────────────────────────────────
// new_frame
// ─────────────────────────────────────────────────────────────────────────────
void VulkanBackend::new_frame() noexcept
{
    if (!initialized_) [[unlikely]] return;

    frame_start_tsc_ = hal::rdtsc();

    // Adquirir imagen del swapchain.
    auto acquire_r = swap_->acquire_next_image();
    if (!acquire_r) {
        // Swapchain expirado (resize). Recrear con tamaño actual.
        int fb_w = 0, fb_h = 0;
        glfwGetFramebufferSize(window_, &fb_w, &fb_h);
        if (fb_w > 0 && fb_h > 0) {
            swap_->recreate(static_cast<uint32_t>(fb_w),
                            static_cast<uint32_t>(fb_h));
        }
        frame_acquired_ = false;
        return;
    }
    image_index_    = *acquire_r;
    frame_acquired_ = true;

    // ── Grabar el comienzo del command buffer ─────────────────────────────────
    VkCommandBuffer cmd = swap_->current_command_buffer();

    VkCommandBufferBeginInfo begin_ci{};
    begin_ci.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_ci.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin_ci);

    // ── Begin render pass ─────────────────────────────────────────────────────
    const VkClearValue clear_val = {{{ 0.10f, 0.10f, 0.10f, 1.0f }}};

    VkRenderPassBeginInfo rp_begin{};
    rp_begin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp_begin.renderPass        = swap_->render_pass();
    rp_begin.framebuffer       = swap_->framebuffer_at(image_index_);
    rp_begin.renderArea.offset = {0, 0};
    rp_begin.renderArea.extent = { swap_->width(), swap_->height() };
    rp_begin.clearValueCount   = 1u;
    rp_begin.pClearValues      = &clear_val;
    vkCmdBeginRenderPass(cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

    // ── ImGui new frame ───────────────────────────────────────────────────────
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

// ─────────────────────────────────────────────────────────────────────────────
// end_frame
// ─────────────────────────────────────────────────────────────────────────────
RenderStats VulkanBackend::end_frame() noexcept
{
    RenderStats stats{};
    if (!initialized_ || !frame_acquired_) [[unlikely]] return stats;

    // ── Render ImGui ──────────────────────────────────────────────────────────
    ImGui::Render();
    ImDrawData* draw_data = ImGui::GetDrawData();

    VkCommandBuffer cmd = swap_->current_command_buffer();
    if (draw_data) {
        ImGui_ImplVulkan_RenderDrawData(draw_data, cmd);
        stats.draw_calls = static_cast<uint32_t>(draw_data->CmdListsCount);
        stats.vertices   = static_cast<uint32_t>(draw_data->TotalVtxCount);
        stats.indices    = static_cast<uint32_t>(draw_data->TotalIdxCount);
    }

    // ── End render pass y command buffer ──────────────────────────────────────
    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);

    // ── Frame timing ──────────────────────────────────────────────────────────
    const hal::tsc_t end_tsc = hal::rdtsc();
    const uint64_t   freq    = hal::tsc_freq_hz();
    const uint64_t   cpu_ns  = (freq > 0u)
        ? static_cast<uint64_t>(end_tsc - frame_start_tsc_) * 1'000'000'000ULL / freq
        : 0u;

    const float wall_ms = (freq > 0u)
        ? static_cast<float>(end_tsc - last_present_tsc_)
          * 1000.0f / static_cast<float>(freq)
        : 0.0f;

    constexpr float kAlpha  = 0.05f;
    const float inst_fps    = (wall_ms > 0.0f) ? (1000.0f / wall_ms) : 0.0f;
    fps_ema_ = (fps_ema_ < 0.5f) ? inst_fps
                                  : fps_ema_ + kAlpha * (inst_fps - fps_ema_);

    if (wall_ms > 33.0f) ++dropped_frames_;

    stats.frame_id       = ++frame_id_;
    stats.present_tsc    = last_present_tsc_;
    stats.cpu_time_ns    = cpu_ns;
    stats.gpu_time_ns    = 0u;   // GPU timing queries: Fase futura (TimestampQuery)
    stats.frame_time_ms  = wall_ms;
    stats.fps_ema        = fps_ema_;
    stats.dropped_frames = dropped_frames_;

    return stats;
}

// ─────────────────────────────────────────────────────────────────────────────
// present
// ─────────────────────────────────────────────────────────────────────────────
void VulkanBackend::present() noexcept
{
    if (!initialized_ || !frame_acquired_) [[unlikely]] return;

    swap_->submit_and_present(image_index_);
    last_present_tsc_ = hal::rdtsc();
    frame_acquired_   = false;
}

// ─────────────────────────────────────────────────────────────────────────────
// resize
// ─────────────────────────────────────────────────────────────────────────────
void VulkanBackend::resize(uint32_t w, uint32_t h, float /*dpi_scale*/) noexcept
{
    if (!initialized_ || w == 0u || h == 0u) [[unlikely]] return;

    ctx_->device_wait_idle();
    swap_->recreate(w, h);
}

// ─────────────────────────────────────────────────────────────────────────────
// shutdown
// ─────────────────────────────────────────────────────────────────────────────
void VulkanBackend::shutdown() noexcept
{
    if (!initialized_ && !ctx_ && !swap_) return;

    if (ctx_) ctx_->device_wait_idle();

    if (initialized_) {
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        initialized_ = false;
    }

    if (swap_) {
        swap_->shutdown();
        delete swap_;
        swap_ = nullptr;
    }
    if (ctx_) {
        ctx_->shutdown();
        delete ctx_;
        ctx_ = nullptr;
    }

    window_         = nullptr;
    arena_          = nullptr;
    frame_acquired_ = false;
}

} // namespace phyriad::render::vulkan
// Made with my soul - Swately <3
