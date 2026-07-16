// framework/render/vulkan/src/FSR3Pass.cpp
// FSR3Pass — AMD FidelityFX Super Resolution 3 + Frame Generation.
//
// Same PHYRIAD_HAS_FFX_SDK dual-path as FSR2Pass.cpp.
//
// FSR3 SDK differences from FSR2:
//   - ffxFsr3ContextCreate / ffxFsr3ContextDispatch / ffxFsr3ContextDestroy
//   - FfxFrameGenContext for the separate frame-generation pass
//   - Requires VK_KHR_present_id + VK_KHR_present_wait if FG is enabled
//   - ffxFrameGenerationConfigureSwapchain for VK_KHR_swapchain integration
//
#include <phyriad/render/vulkan/FSR3Pass.hpp>
#include <phyriad/render/vulkan/VulkanContext.hpp>
#include <phyriad/schema/Error.hpp>
#include <cstdio>

#if PHYRIAD_HAS_FFX_SDK
// ─────────────────────────────────────────────────────────────────────────────
// Real implementation — requires AMD FidelityFX SDK with FSR3 support
// ─────────────────────────────────────────────────────────────────────────────
#include <FfxFsr3.h>          // ffxFsr3ContextCreate / ffxFsr3ContextDispatch
#include <FfxFrameGen.h>      // FfxFrameGenContext, ffxFrameGenDispatch
#include <vk/ffx_fsr3_vk.h>

namespace phyriad::render::vulkan {

FSR3Pass::FSR3Pass(VulkanContext& ctx) noexcept
    : ctx_(ctx)
{}

FSR3Pass::~FSR3Pass() noexcept
{
    shutdown();
}

std::expected<void, phyriad::Error>
FSR3Pass::init(uint32_t in_w, uint32_t in_h,
               uint32_t out_w, uint32_t out_h) noexcept
{
    if (initialized_) shutdown();

    if (in_w == 0u || in_h == 0u || out_w == 0u || out_h == 0u) {
        return std::unexpected(phyriad::Error{
            phyriad::ErrorCode::ResourceInitFailed,
            "FSR3Pass::init: zero dimension"});
    }
    if (out_w < in_w || out_h < in_h) {
        return std::unexpected(phyriad::Error{
            phyriad::ErrorCode::ResourceInitFailed,
            "FSR3Pass::init: output must be >= input resolution"});
    }

    // ── FSR3 upscaling context ────────────────────────────────────────────────
    auto* fsr3_ctx = new (std::nothrow) FfxFsr3Context;
    if (!fsr3_ctx) {
        return std::unexpected(phyriad::Error{
            phyriad::ErrorCode::ResourceInitFailed,
            "FSR3Pass::init: FfxFsr3Context allocation failed"});
    }

    const size_t scratch_sz = ffxFsr3GetScratchMemorySizeVK(ctx_.physical_device());
    void* scratch = operator new(scratch_sz, std::nothrow);
    if (!scratch) { delete fsr3_ctx; return std::unexpected(phyriad::Error{phyriad::ErrorCode::ResourceInitFailed, "scratch alloc"}); }

    FfxFsr3Interface backend{};
    if (ffxFsr3GetInterfaceVK(&backend, ctx_.device(), ctx_.physical_device(),
                              scratch, scratch_sz) != FFX_OK) {
        operator delete(scratch); delete fsr3_ctx;
        return std::unexpected(phyriad::Error{phyriad::ErrorCode::ResourceInitFailed, "ffxFsr3GetInterfaceVK"});
    }

    FfxFsr3ContextDescription desc{};
    desc.flags            = FFX_FSR3_ENABLE_HIGH_DYNAMIC_RANGE
                          | FFX_FSR3_ENABLE_DEPTH_INVERTED
                          | FFX_FSR3_ENABLE_AUTO_EXPOSURE;
    desc.maxRenderSize    = {in_w,  in_h};
    desc.displaySize      = {out_w, out_h};
    desc.backendInterface = backend;

    if (ffxFsr3ContextCreate(fsr3_ctx, &desc) != FFX_OK) {
        operator delete(scratch); delete fsr3_ctx;
        return std::unexpected(phyriad::Error{phyriad::ErrorCode::ResourceInitFailed, "ffxFsr3ContextCreate"});
    }

    sdk_ctx_ = fsr3_ctx;

    // ── Frame generation context (optional) ───────────────────────────────────
    // Only available if VK_KHR_present_id / VK_KHR_present_wait are supported.
    // We attempt the FG context and silently disable if extensions are absent.
    auto* fg_ctx = new (std::nothrow) FfxFrameGenContext;
    if (fg_ctx) {
        FfxFrameGenContextDescription fg_desc{};
        fg_desc.flags       = FFX_FRAMEGEN_ENABLE_ASYNC_WORKLOAD_SUPPORT;
        fg_desc.displaySize = {out_w, out_h};
        fg_desc.backendInterface = backend;

        if (ffxFrameGenContextCreate(fg_ctx, &fg_desc) == FFX_OK) {
            sdk_fg_ctx_   = fg_ctx;
            fg_available_ = true;
        } else {
            delete fg_ctx;
            fg_available_ = false;
        }
    }

    in_w_        = in_w;
    in_h_        = in_h;
    out_w_       = out_w;
    out_h_       = out_h;
    initialized_ = true;
    return {};
}

void FSR3Pass::shutdown() noexcept
{
    if (!initialized_) return;

    if (sdk_fg_ctx_) {
        ffxFrameGenContextDestroy(static_cast<FfxFrameGenContext*>(sdk_fg_ctx_));
        delete static_cast<FfxFrameGenContext*>(sdk_fg_ctx_);
        sdk_fg_ctx_   = nullptr;
        fg_available_ = false;
    }
    if (sdk_ctx_) {
        ffxFsr3ContextDestroy(static_cast<FfxFsr3Context*>(sdk_ctx_));
        delete static_cast<FfxFsr3Context*>(sdk_ctx_);
        sdk_ctx_ = nullptr;
    }
    initialized_ = false;
    in_w_ = in_h_ = out_w_ = out_h_ = 0u;
}

void FSR3Pass::apply(const Inputs& in) noexcept
{
    if (!initialized_) return;
    if (in.cmdbuf         == VK_NULL_HANDLE) return;
    if (in.color_input    == VK_NULL_HANDLE) return;
    if (in.depth_input    == VK_NULL_HANDLE) return;
    if (in.motion_vectors == VK_NULL_HANDLE) return;
    if (in.color_output   == VK_NULL_HANDLE) return;

    // ── FSR3 upscaling dispatch ───────────────────────────────────────────────
    FfxFsr3DispatchUpscaleDescription disp{};
    disp.commandList        = ffxGetCommandListVK(in.cmdbuf);
    disp.jitterOffset       = {in.jitter_x, in.jitter_y};
    disp.motionVectorScale  = {static_cast<float>(in_w_), static_cast<float>(in_h_)};
    disp.renderSize         = {in_w_, in_h_};
    disp.enableSharpening   = (in.sharpness > 0.f);
    disp.sharpness          = in.sharpness;
    disp.frameTimeDelta     = in.frame_time_ms;
    disp.preExposure        = 1.f;
    disp.reset              = in.reset;
    disp.cameraNear         = 0.01f;
    disp.cameraFar          = 1000.f;
    disp.cameraFovAngleVertical = 1.0472f;
    // color/depth/motion resources (abbreviated — analogous to FSR2):
    disp.color         = ffxGetTextureResourceVK(
        static_cast<FfxFsr3Context*>(sdk_ctx_),
        VK_NULL_HANDLE, in.color_input,
        in_w_, in_h_, VK_FORMAT_B8G8R8A8_UNORM,
        nullptr, FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    disp.depth         = ffxGetTextureResourceVK(
        static_cast<FfxFsr3Context*>(sdk_ctx_),
        VK_NULL_HANDLE, in.depth_input,
        in_w_, in_h_, VK_FORMAT_D32_SFLOAT,
        nullptr, FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    disp.motionVectors = ffxGetTextureResourceVK(
        static_cast<FfxFsr3Context*>(sdk_ctx_),
        VK_NULL_HANDLE, in.motion_vectors,
        in_w_, in_h_, VK_FORMAT_R16G16_SFLOAT,
        nullptr, FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    disp.output        = ffxGetTextureResourceVK(
        static_cast<FfxFsr3Context*>(sdk_ctx_),
        in.color_output, VK_NULL_HANDLE,
        out_w_, out_h_, VK_FORMAT_B8G8R8A8_UNORM,
        nullptr, FFX_RESOURCE_STATE_UNORDERED_ACCESS);

    (void)ffxFsr3ContextDispatchUpscale(
        static_cast<FfxFsr3Context*>(sdk_ctx_), &disp);

    // ── Frame generation dispatch (if enabled and available) ──────────────────
    if (in.frame_generation && fg_available_ && sdk_fg_ctx_) {
        FfxFrameGenDispatchDescription fg_disp{};
        fg_disp.commandList     = ffxGetCommandListVK(in.cmdbuf);
        fg_disp.presentColor    = disp.output;
        fg_disp.reset           = in.reset;
        fg_disp.frameTimeDelta  = in.frame_time_ms;

        (void)ffxFrameGenContextDispatch(
            static_cast<FfxFrameGenContext*>(sdk_fg_ctx_), &fg_disp);
    }
}

void FSR3Pass::resize(uint32_t in_w, uint32_t in_h,
                      uint32_t out_w, uint32_t out_h) noexcept
{
    if (!initialized_) return;
    shutdown();
    (void)init(in_w, in_h, out_w, out_h);
}

} // namespace phyriad::render::vulkan

#else // PHYRIAD_HAS_FFX_SDK == 0
// ─────────────────────────────────────────────────────────────────────────────
// Fallback implementation (FFX SDK not present — same contract as FSR2Pass:
// init() returns ResourceInitFailed; all other methods are no-ops.)
// ─────────────────────────────────────────────────────────────────────────────

namespace phyriad::render::vulkan {

FSR3Pass::FSR3Pass(VulkanContext& ctx) noexcept : ctx_(ctx) {}
FSR3Pass::~FSR3Pass() noexcept { shutdown(); }

std::expected<void, phyriad::Error>
FSR3Pass::init(uint32_t, uint32_t, uint32_t, uint32_t) noexcept
{
    std::fprintf(stderr,
        "[FSR3Pass] AMD FidelityFX SDK not found.\n"
        "  Place the SDK in pillars/render/vulkan/third_party/ffx/\n"
        "  and rebuild with -DPHYRIAD_BUILD_FSR2=ON -DPHYRIAD_BUILD_VULKAN=ON\n");
    return std::unexpected(phyriad::Error{
        phyriad::ErrorCode::ResourceInitFailed,
        "FSR3Pass requires PHYRIAD_BUILD_FSR2=ON + AMD FidelityFX SDK"});
}

void FSR3Pass::shutdown() noexcept
{
    initialized_  = false;
    fg_available_ = false;
    sdk_ctx_      = nullptr;
    sdk_fg_ctx_   = nullptr;
    in_w_ = in_h_ = out_w_ = out_h_ = 0u;
}

void FSR3Pass::apply(const Inputs&) noexcept {}

void FSR3Pass::resize(uint32_t, uint32_t, uint32_t, uint32_t) noexcept {}

} // namespace phyriad::render::vulkan

#endif // PHYRIAD_HAS_FFX_SDK
// Made with my soul - Swately <3
