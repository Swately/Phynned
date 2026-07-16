// framework/render/vulkan/include/phyriad/render/vulkan/FSR3Pass.hpp
// FSR3Pass — AMD FidelityFX Super Resolution 3 (Vulkan backend).
//
// Extends FSR2Pass with Frame Generation: interpolates intermediate frames
// between rendered frames to double effective frame rate.
//
// FSR3 = FSR2 (temporal upscaling) + FG (frame generation via optical flow).
// Both passes share the same FidelityFX SDK dispatch mechanism.
//
// Additional requirements vs FSR2Pass:
//   - VK_KHR_present_id + VK_KHR_present_wait for frame pacing.
//   - Optical flow image (automatically managed by the SDK).
//   - frame_generation field in Inputs enables/disables FG per-frame.
//
// When frame_generation=false, FSR3Pass is functionally identical to FSR2Pass
// (temporal upscaling only, no interpolated frames).
//
#pragma once
#include <phyriad/render/vulkan/VulkanContext.hpp>
#include <phyriad/schema/Error.hpp>
#include <vulkan/vulkan.h>
#include <cstdint>
#include <expected>

namespace phyriad::render::vulkan {

// ─────────────────────────────────────────────────────────────────────────────
// FSR3Pass
// ─────────────────────────────────────────────────────────────────────────────
class FSR3Pass {
public:
    // ── Per-frame dispatch inputs ─────────────────────────────────────────────
    struct Inputs {
        VkImageView     color_input        {VK_NULL_HANDLE};
        VkImageView     depth_input        {VK_NULL_HANDLE};
        VkImageView     motion_vectors     {VK_NULL_HANDLE};
        VkImage         color_output       {VK_NULL_HANDLE};
        VkCommandBuffer cmdbuf             {VK_NULL_HANDLE};
        float           sharpness          {0.2f};
        float           jitter_x           {0.f};
        float           jitter_y           {0.f};
        float           frame_time_ms      {16.667f};
        bool            reset              {false};
        bool            frame_generation   {true};  // false = FSR2-only (no FG)
    };

    // ── Construction ─────────────────────────────────────────────────────────
    explicit FSR3Pass(VulkanContext& ctx) noexcept;
    ~FSR3Pass() noexcept;

    FSR3Pass(FSR3Pass const&)            = delete;
    FSR3Pass& operator=(FSR3Pass const&) = delete;
    FSR3Pass(FSR3Pass&&)                 = delete;
    FSR3Pass& operator=(FSR3Pass&&)      = delete;

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    [[nodiscard]] std::expected<void, phyriad::Error>
    init(uint32_t in_w, uint32_t in_h,
         uint32_t out_w, uint32_t out_h) noexcept;

    void shutdown() noexcept;
    void apply(const Inputs& in) noexcept;
    void resize(uint32_t in_w, uint32_t in_h,
                uint32_t out_w, uint32_t out_h) noexcept;

    // ── Queries ───────────────────────────────────────────────────────────────
    [[nodiscard]] bool     initialized()         const noexcept { return initialized_; }
    [[nodiscard]] bool     frame_gen_available()  const noexcept { return fg_available_; }
    [[nodiscard]] uint32_t input_width()          const noexcept { return in_w_;  }
    [[nodiscard]] uint32_t input_height()         const noexcept { return in_h_;  }
    [[nodiscard]] uint32_t output_width()         const noexcept { return out_w_; }
    [[nodiscard]] uint32_t output_height()        const noexcept { return out_h_; }

private:
    VulkanContext& ctx_;

    uint32_t in_w_  {0};
    uint32_t in_h_  {0};
    uint32_t out_w_ {0};
    uint32_t out_h_ {0};
    bool     initialized_ {false};
    bool     fg_available_{false};  // true when FG extensions are supported

    void* sdk_ctx_   {nullptr};  // FfxFsr3Context* — opaque
    void* sdk_fg_ctx_{nullptr};  // FfxFrameGenContext* — opaque
};

} // namespace phyriad::render::vulkan
// Made with my soul - Swately <3
