// framework/render/vulkan/src/FSR2Pass.cpp
// FSR2Pass — AMD FidelityFX Super Resolution 2 implementation.
//
// Compiled in two modes controlled by PHYRIAD_HAS_FFX_SDK (set by CMakeLists.txt):
//
//   PHYRIAD_HAS_FFX_SDK=1  — Real implementation using the FidelityFX SDK.
//                          SDK headers at third_party/ffx/include/
//                          and backend at third_party/ffx/include/vk/
//
//   PHYRIAD_HAS_FFX_SDK=0  — Fallback (intentional, not deferred work). init()
//                           returns ResourceInitFailed with a descriptive message;
//                           all other methods are no-ops. The API contract
//                           (zero-init, idempotent shutdown, no crash on
//                           uninitialized apply) is still upheld so callers
//                           can detect SDK absence at runtime without crashing.
//
#include <phyriad/render/vulkan/FSR2Pass.hpp>
#include <phyriad/render/vulkan/VulkanContext.hpp>
#include <phyriad/schema/Error.hpp>
#include <cstdio>

#if PHYRIAD_HAS_FFX_SDK
// ─────────────────────────────────────────────────────────────────────────────
// Real implementation — requires AMD FidelityFX SDK
// ─────────────────────────────────────────────────────────────────────────────
#include <FfxFsr2.h>
#include <vk/ffx_fsr2_vk.h>

namespace phyriad::render::vulkan {

FSR2Pass::FSR2Pass(VulkanContext& ctx) noexcept
    : ctx_(ctx)
{}

FSR2Pass::~FSR2Pass() noexcept
{
    shutdown();
}

std::expected<void, phyriad::Error>
FSR2Pass::init(uint32_t in_w, uint32_t in_h,
               uint32_t out_w, uint32_t out_h) noexcept
{
    if (initialized_) shutdown();

    if (in_w == 0u || in_h == 0u || out_w == 0u || out_h == 0u) {
        return std::unexpected(phyriad::Error{
            phyriad::ErrorCode::ResourceInitFailed,
            "FSR2Pass::init: zero dimension"});
    }
    if (out_w < in_w || out_h < in_h) {
        return std::unexpected(phyriad::Error{
            phyriad::ErrorCode::ResourceInitFailed,
            "FSR2Pass::init: output must be >= input resolution"});
    }

    // ── Allocate SDK context ──────────────────────────────────────────────────
    auto* sdk = new (std::nothrow) FfxFsr2Context;
    if (!sdk) {
        return std::unexpected(phyriad::Error{
            phyriad::ErrorCode::ResourceInitFailed,
            "FSR2Pass::init: FfxFsr2Context allocation failed"});
    }

    // Create backend interface for Vulkan.
    const size_t scratch_size = ffxFsr2GetScratchMemorySizeVK(ctx_.physical_device());
    void* scratch = operator new(scratch_size, std::nothrow);
    if (!scratch) {
        delete sdk;
        return std::unexpected(phyriad::Error{
            phyriad::ErrorCode::ResourceInitFailed,
            "FSR2Pass::init: scratch memory allocation failed"});
    }

    FfxFsr2Interface backend_iface{};
    FfxErrorCode err = ffxFsr2GetInterfaceVK(
        &backend_iface,
        ctx_.device(),
        ctx_.physical_device(),
        scratch,
        scratch_size);

    if (err != FFX_OK) {
        operator delete(scratch);
        delete sdk;
        return std::unexpected(phyriad::Error{
            phyriad::ErrorCode::ResourceInitFailed,
            "FSR2Pass::init: ffxFsr2GetInterfaceVK failed"});
    }

    FfxFsr2ContextDescription desc{};
    desc.flags          = FFX_FSR2_ENABLE_HIGH_DYNAMIC_RANGE
                        | FFX_FSR2_ENABLE_DEPTH_INVERTED
                        | FFX_FSR2_ENABLE_AUTO_EXPOSURE;
    desc.maxRenderSize  = {in_w,  in_h};
    desc.displaySize    = {out_w, out_h};
    desc.backendInterface = backend_iface;

    err = ffxFsr2ContextCreate(sdk, &desc);
    if (err != FFX_OK) {
        operator delete(scratch);
        delete sdk;
        return std::unexpected(phyriad::Error{
            phyriad::ErrorCode::ResourceInitFailed,
            "FSR2Pass::init: ffxFsr2ContextCreate failed"});
    }

    sdk_ctx_     = sdk;
    in_w_        = in_w;
    in_h_        = in_h;
    out_w_       = out_w;
    out_h_       = out_h;
    initialized_ = true;
    return {};
}

void FSR2Pass::shutdown() noexcept
{
    if (!initialized_) return;
    if (sdk_ctx_) {
        ffxFsr2ContextDestroy(static_cast<FfxFsr2Context*>(sdk_ctx_));
        delete static_cast<FfxFsr2Context*>(sdk_ctx_);
        sdk_ctx_ = nullptr;
    }
    initialized_ = false;
    in_w_ = in_h_ = out_w_ = out_h_ = 0u;
}

void FSR2Pass::apply(const Inputs& in) noexcept
{
    if (!initialized_) return;
    if (in.cmdbuf         == VK_NULL_HANDLE) return;
    if (in.color_input    == VK_NULL_HANDLE) return;
    if (in.depth_input    == VK_NULL_HANDLE) return;
    if (in.motion_vectors == VK_NULL_HANDLE) return;
    if (in.color_output   == VK_NULL_HANDLE) return;

    FfxFsr2DispatchDescription disp{};
    disp.commandList                = ffxGetCommandListVK(in.cmdbuf);
    disp.color                      = ffxGetTextureResourceVK(
        static_cast<FfxFsr2Context*>(sdk_ctx_),
        VK_NULL_HANDLE, in.color_input,
        in_w_, in_h_, VK_FORMAT_B8G8R8A8_UNORM,
        nullptr, FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    disp.depth                      = ffxGetTextureResourceVK(
        static_cast<FfxFsr2Context*>(sdk_ctx_),
        VK_NULL_HANDLE, in.depth_input,
        in_w_, in_h_, VK_FORMAT_D32_SFLOAT,
        nullptr, FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    disp.motionVectors              = ffxGetTextureResourceVK(
        static_cast<FfxFsr2Context*>(sdk_ctx_),
        VK_NULL_HANDLE, in.motion_vectors,
        in_w_, in_h_, VK_FORMAT_R16G16_SFLOAT,
        nullptr, FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    disp.output                     = ffxGetTextureResourceVK(
        static_cast<FfxFsr2Context*>(sdk_ctx_),
        in.color_output, VK_NULL_HANDLE,
        out_w_, out_h_, VK_FORMAT_B8G8R8A8_UNORM,
        nullptr, FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    disp.jitterOffset               = {in.jitter_x, in.jitter_y};
    disp.motionVectorScale          = {static_cast<float>(in_w_),
                                       static_cast<float>(in_h_)};
    disp.renderSize                 = {in_w_, in_h_};
    disp.enableSharpening           = (in.sharpness > 0.f);
    disp.sharpness                  = in.sharpness;
    disp.frameTimeDelta             = in.frame_time_ms;
    disp.preExposure                = 1.f;
    disp.reset                      = in.reset;
    disp.cameraNear                 = 0.01f;
    disp.cameraFar                  = 1000.f;
    disp.cameraFovAngleVertical     = 1.0472f; // 60°

    (void)ffxFsr2ContextDispatch(
        static_cast<FfxFsr2Context*>(sdk_ctx_), &disp);
}

void FSR2Pass::resize(uint32_t in_w, uint32_t in_h,
                      uint32_t out_w, uint32_t out_h) noexcept
{
    if (!initialized_) return;
    shutdown();
    (void)init(in_w, in_h, out_w, out_h);
}

} // namespace phyriad::render::vulkan

#else // PHYRIAD_HAS_FFX_SDK == 0
// ─────────────────────────────────────────────────────────────────────────────
// Fallback implementation (FFX SDK not present)
// ─────────────────────────────────────────────────────────────────────────────

namespace phyriad::render::vulkan {

FSR2Pass::FSR2Pass(VulkanContext& ctx) noexcept
    : ctx_(ctx)
{}

FSR2Pass::~FSR2Pass() noexcept
{
    shutdown();
}

std::expected<void, phyriad::Error>
FSR2Pass::init(uint32_t /*in_w*/, uint32_t /*in_h*/,
               uint32_t /*out_w*/, uint32_t /*out_h*/) noexcept
{
    std::fprintf(stderr,
        "[FSR2Pass] AMD FidelityFX SDK not found.\n"
        "  Place the SDK in pillars/render/vulkan/third_party/ffx/\n"
        "  and rebuild with -DPHYRIAD_BUILD_FSR2=ON -DPHYRIAD_BUILD_VULKAN=ON\n");
    return std::unexpected(phyriad::Error{
        phyriad::ErrorCode::ResourceInitFailed,
        "FSR2Pass requires PHYRIAD_BUILD_FSR2=ON + AMD FidelityFX SDK"});
}

void FSR2Pass::shutdown() noexcept
{
    // No resources allocated — idempotent no-op.
    initialized_ = false;
    sdk_ctx_     = nullptr;
    in_w_ = in_h_ = out_w_ = out_h_ = 0u;
}

void FSR2Pass::apply(const Inputs& /*in*/) noexcept
{
    // No-op: not initialized.
}

void FSR2Pass::resize(uint32_t /*in_w*/, uint32_t /*in_h*/,
                      uint32_t /*out_w*/, uint32_t /*out_h*/) noexcept
{
    // No-op: not initialized.
}

} // namespace phyriad::render::vulkan

#endif // PHYRIAD_HAS_FFX_SDK
// Made with my soul - Swately <3
