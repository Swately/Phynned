// framework/render/vulkan/include/phyriad/render/vulkan/FSR2Pass.hpp
// FSR2Pass — AMD FidelityFX Super Resolution 2 (Vulkan backend).
//
// Wraps the FidelityFX SDK FSR2 context and provides a single-call
// per-frame dispatch interface compatible with the Phyriad render pipeline.
//
// Dependencies:
//   - AMD FidelityFX SDK in pillars/render/vulkan/third_party/ffx/
//     (build with PHYRIAD_BUILD_FSR2=ON)
//   - VulkanContext — provides VkDevice, VkPhysicalDevice, queues
//
// Algorithm (FidelityFX FSR 2.x):
//   Temporal upscaling using motion vectors and depth buffer.
//   Requires jitter offsets (Halton sequence) for sub-pixel sampling.
//   Output quality improves over frames as the history accumulates.
//
// Usage:
//   FSR2Pass fsr(ctx);
//   fsr.init(960, 540, 1920, 1080);   // 540p → 1080p (Quality mode)
//
//   // each frame:
//   FSR2Pass::Inputs in;
//   in.color_input    = render_target_view;
//   in.depth_input    = depth_buffer_view;
//   in.motion_vectors = motion_vec_view;
//   in.color_output   = output_image;
//   in.cmdbuf         = current_command_buffer;
//   in.jitter_x       = halton_x[frame % N];
//   in.jitter_y       = halton_y[frame % N];
//   fsr.apply(in);
//
// When PHYRIAD_BUILD_FSR2=ON but the SDK is absent, init() returns
// ErrorCode::ResourceInitFailed (stubs are compiled instead of real impl).
//
#pragma once
#include <phyriad/render/vulkan/VulkanContext.hpp>
#include <phyriad/schema/Error.hpp>
#include <vulkan/vulkan.h>
#include <cstdint>
#include <expected>

namespace phyriad::render::vulkan {

// ─────────────────────────────────────────────────────────────────────────────
// FSR2Pass
// ─────────────────────────────────────────────────────────────────────────────
class FSR2Pass {
public:
    // ── Per-frame dispatch inputs ─────────────────────────────────────────────
    struct Inputs {
        VkImageView     color_input    {VK_NULL_HANDLE}; // rendered scene (input resolution)
        VkImageView     depth_input    {VK_NULL_HANDLE}; // linear depth or depth/stencil
        VkImageView     motion_vectors {VK_NULL_HANDLE}; // RG16F screen-space motion (pixels)
        VkImage         color_output   {VK_NULL_HANDLE}; // target image (output resolution)
        VkCommandBuffer cmdbuf         {VK_NULL_HANDLE}; // command buffer to record into
        float           sharpness      {0.2f};           // RCAS sharpness 0=max … 1=off
        float           jitter_x       {0.f};            // Halton jitter X in NDC pixels
        float           jitter_y       {0.f};            // Halton jitter Y in NDC pixels
        float           frame_time_ms  {16.667f};        // delta time for motion weighting
        bool            reset          {false};          // true = discard history (scene cut)
    };

    // ── Construction ─────────────────────────────────────────────────────────
    explicit FSR2Pass(VulkanContext& ctx) noexcept;
    ~FSR2Pass() noexcept;

    FSR2Pass(FSR2Pass const&)            = delete;
    FSR2Pass& operator=(FSR2Pass const&) = delete;
    FSR2Pass(FSR2Pass&&)                 = delete;
    FSR2Pass& operator=(FSR2Pass&&)      = delete;

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    // Allocate FSR2 context resources for the given render resolution.
    // in_w × in_h  = rendering resolution (e.g. 960 × 540 for Quality mode).
    // out_w × out_h = display resolution  (e.g. 1920 × 1080).
    // Returns ResourceInitFailed if:
    //   - SDK not found (PHYRIAD_HAS_FFX_SDK=0)
    //   - Dimensions are zero or out > in for either axis
    //   - Device allocation fails
    [[nodiscard]] std::expected<void, phyriad::Error>
    init(uint32_t in_w, uint32_t in_h,
         uint32_t out_w, uint32_t out_h) noexcept;

    // Release all SDK resources.  Safe to call multiple times (idempotent).
    void shutdown() noexcept;

    // Record FSR2 dispatch into cmdbuf.
    // No-op if not initialized or any required view is VK_NULL_HANDLE.
    void apply(const Inputs& in) noexcept;

    // Resize — equivalent to shutdown() + init() with new dimensions.
    void resize(uint32_t in_w, uint32_t in_h,
                uint32_t out_w, uint32_t out_h) noexcept;

    // ── Queries ───────────────────────────────────────────────────────────────
    [[nodiscard]] bool     initialized()   const noexcept { return initialized_; }
    [[nodiscard]] uint32_t input_width()   const noexcept { return in_w_;   }
    [[nodiscard]] uint32_t input_height()  const noexcept { return in_h_;   }
    [[nodiscard]] uint32_t output_width()  const noexcept { return out_w_;  }
    [[nodiscard]] uint32_t output_height() const noexcept { return out_h_;  }

private:
    VulkanContext& ctx_;

    uint32_t in_w_  {0};
    uint32_t in_h_  {0};
    uint32_t out_w_ {0};
    uint32_t out_h_ {0};
    bool     initialized_{false};

    // SDK context handle — opaque pointer, defined in FSR2Pass.cpp.
    // Declared as void* to avoid leaking SDK types into this header.
    void* sdk_ctx_{nullptr};
};

} // namespace phyriad::render::vulkan
// Made with my soul - Swately <3
