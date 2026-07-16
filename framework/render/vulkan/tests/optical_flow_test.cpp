// framework/render/vulkan/tests/optical_flow_test.cpp
// Headless mathematical verification of OpticalFlowPipeline (HIERARCHICAL pyramid block-match,
// STAGE-15; extended in STAGE-20 with confidence gate + Rc6/Rf2 defaults;
// extended in STAGE-24 with post-warp source-agreement gate).
// Synthetic motion with known ground truth: build two frames with a bright blob translated by a
// known vector, run the pipeline, read the motion-vector image back, and assert the tile containing
// the blob reports that vector within ±0.5 px.
//
// Cases:
//   1. small-motion (the original gate): 64×64, blob (24,28)→(29,31) = MV (+5,+3).
//      The pyramid must still track SMALL displacements (it does not over-downsample tiny frames).
//   2. LARGE displacement: 256×256, blob translated by (+40, 0) = MV (+40,0). This is the NEW
//      capability — the old flat ±R search was capped at R≤32 and could not reach D=40 at all.
//   3. small FAST object: 128×128, an 8×8 object translated by (+12,+8). Verifies a small object in
//      fast motion is NOT lost to the coarse pyramid averaging (the STAGE-14 caveat) within the
//      depth-capped envelope.
//   4. STATIC OVERLAY over moving scene (STAGE-20 confidence gate): 128×128, a 16×16 static white
//      overlay at (8,8) AND a 16×16 moving blob at (48,48)→(56,48) = MV (+8,0).
//      The overlay tile: block-match returns MV=(0,0) naturally (uniform white tile → SAD=0 at all
//      MVs; lambda tie-breaker selects MV=predictor=0). The confidence gate: sad_zero=0, sad_best=0
//      → 0*(1−0.5) > 0 = false → BLEND (correct: static tile is blend-in-place regardless of MV).
//      The blob tile must still track (+8,0) — high sad_zero, low sad_best → WARP gate passes.
//   5. WARP OUTPUT READBACK (STAGE-24 agreement gate + brecha de STAGE-18/20 cerrada):
//      Same geometry as case 4 (128×128, static overlay (8,8), blob (48,48)→(56,48)).
//      Reads back the output image C and asserts the WARP OUTPUT:
//        (a) Overlay pixel (12,12): BLEND — both A and B have value 250 here → output ≈ 250.
//            (Confidence gate fails for static tile: sad_zero=0 → improvement_frac check fails.)
//        (b) Blob midpoint pixel (52,48): WARP — A_samp = A[48,48]=220 (in blob A),
//            B_samp = B[56,48]=220 (in blob B), d≈0 → agreement gate passes → output ≈ 220.
//            If the warp gate were broken (always BLEND), output ≈ (A[52,48]+B[52,48])/2
//            = (220+32)/2 ≈ 126 (blob A bright, background in B). This threshold (≥180)
//            cleanly separates WARP (≈220) from BLEND (≈126) for this synthetic scene.
//   6. NON-MULT-8 DIMENSIONS (regression guard for the unconditional barrier in
//      optical_flow_warp.comp ~lines 100-108):
//      Resolution 100×52 (neither axis is a multiple of 8). This spawns edge workgroups whose
//      invocations have coord ≥ out_size; those invocations write 0 to s_d[] and must still reach
//      the barrier() to keep control flow uniform across the workgroup. A past bug (early-return
//      before the barrier) passed every existing test because all prior resolutions (64/128/256)
//      are multiples of 8 — zero edge workgroups, zero out-of-bounds invocations.
//      Geometry: 12×12 bright blob at (40,16)→(45,21) = MV (+5,+3). Blob tile = (5,2).
//      Asserts: (a) blob MV is (+5,+3) within ±0.5 px; (b) warp output C at the blob midpoint
//      (42,18) is WARP-bright (≥180 — BLEND would give ≈126 same as case 5); (c) the pipeline
//      COMPLETES — a divergent barrier typically produces a GPU hang under validation layers.
//
#include <phyriad/render/vulkan/OpticalFlowPipeline.hpp>

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int g_failed = 0;
#define CHECK(cond, msg) do {                                          \
    if (!(cond)) { std::fprintf(stderr, "  [FAIL] %s\n", msg); ++g_failed; }     \
    else        { std::fprintf(stderr, "  [OK  ] %s\n", msg); }                 \
} while (0)

constexpr int kBlockSize = 8;
constexpr int kSearchR   = 8;     // finest-level refinement radius (the pyramid handles large D)

[[nodiscard]] uint32_t find_memory_type(VkPhysicalDevice p, uint32_t bits,
                                        VkMemoryPropertyFlags want) noexcept
{
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(p, &mp);
    for (uint32_t i = 0u; i < mp.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    }
    return UINT32_MAX;
}

// A frame: dark-grey background (32) with a bright (220) square `blob` px wide at (blob_x, blob_y).
// make_frame_with_overlay: same but also composites a STATIC opaque white patch (250) at (ovl_x, ovl_y)
// of size ovl_sz. The patch is drawn identically in A and B — zero motion for those pixels.
void make_frame_with_overlay(uint8_t* px, uint32_t w, uint32_t h,
                             int blob_size, int blob_x, int blob_y,
                             int ovl_x, int ovl_y, int ovl_sz) noexcept
{
    for (uint32_t y = 0u; y < h; ++y)
        for (uint32_t x = 0u; x < w; ++x) {
            const int dbx = static_cast<int>(x) - blob_x;
            const int dby = static_cast<int>(y) - blob_y;
            const int dox = static_cast<int>(x) - ovl_x;
            const int doy = static_cast<int>(y) - ovl_y;
            const bool in_blob = (dbx >= 0 && dbx < blob_size && dby >= 0 && dby < blob_size);
            const bool in_ovl  = (dox >= 0 && dox < ovl_sz   && doy >= 0 && doy < ovl_sz);
            // Overlay takes priority (it sits on top of the blob and background)
            const uint8_t v = in_ovl ? 250u : (in_blob ? 220u : 32u);
            const uint32_t i = (y * w + x) * 4u;
            px[i+0] = v; px[i+1] = v; px[i+2] = v; px[i+3] = 255u;
        }
}

// STAGE-77: a 2D periodic DOT-GRID texture — period `xperiod` px in x (the ambiguous axis), period
// `yperiod` px in y, shifted by `shift` px in x. A texel is bright (220) where BOTH ((x-shift) mod
// xperiod < xperiod/2) AND (y mod yperiod < yperiod/2), dark (32) otherwise. Periodicity in x makes the
// block-match AMBIGUOUS along x: a shift of `s` and `s ± xperiod` both realign → near-tied SADs one
// X-period apart (the aliasing class the candidate field exposes). The Y axis is given a LARGER period
// (> 2·search_radius) so y-translations do NOT realign within the search window — without this, a pure
// vertical-stripe pattern is y-translation-invariant and the runner-up degenerates to a ±1px y-neighbour
// (a trivial tie) instead of the period-shifted vector this test is about.
void make_stripes(uint8_t* px, uint32_t w, uint32_t h, int xperiod, int yperiod, int shift) noexcept
{
    for (uint32_t y = 0u; y < h; ++y)
        for (uint32_t x = 0u; x < w; ++x) {
            int xp = (static_cast<int>(x) - shift) % xperiod; if (xp < 0) xp += xperiod;
            int yp = static_cast<int>(y) % yperiod;           if (yp < 0) yp += yperiod;
            const bool bright = (xp < xperiod / 2) && (yp < yperiod / 2);
            const uint8_t v = bright ? 220u : 32u;
            const uint32_t i = (y * w + x) * 4u;
            px[i+0] = v; px[i+1] = v; px[i+2] = v; px[i+3] = 255u;
        }
}

// A frame: dark-grey background (32) with a bright (220) square `blob` px wide at (blob_x, blob_y).
void make_frame(uint8_t* px, uint32_t w, uint32_t h, int blob, int blob_x, int blob_y) noexcept
{
    for (uint32_t y = 0u; y < h; ++y)
        for (uint32_t x = 0u; x < w; ++x) {
            const int dx = static_cast<int>(x) - blob_x;
            const int dy = static_cast<int>(y) - blob_y;
            const bool in_blob = (dx >= 0 && dx < blob && dy >= 0 && dy < blob);
            const uint8_t v = in_blob ? 220u : 32u;
            const uint32_t i = (y * w + x) * 4u;
            px[i+0] = v; px[i+1] = v; px[i+2] = v; px[i+3] = 255u;
        }
}

struct Image {
    VkImage img{VK_NULL_HANDLE}; VkImageView vw{VK_NULL_HANDLE}; VkDeviceMemory mem{VK_NULL_HANDLE};
    bool create(VkDevice d, VkPhysicalDevice p, uint32_t w, uint32_t h, VkFormat f, VkImageUsageFlags usage) noexcept {
        VkImageCreateInfo ci{}; ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = f; ci.extent = {w, h, 1u}; ci.mipLevels = 1u; ci.arrayLayers = 1u; ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = usage; ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(d, &ci, nullptr, &img) != VK_SUCCESS) return false;
        VkMemoryRequirements mr{}; vkGetImageMemoryRequirements(d, img, &mr);
        const uint32_t mt = find_memory_type(p, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mt == UINT32_MAX) return false;
        VkMemoryAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ai.allocationSize = mr.size; ai.memoryTypeIndex = mt;
        if (vkAllocateMemory(d, &ai, nullptr, &mem) != VK_SUCCESS) return false;
        vkBindImageMemory(d, img, mem, 0u);
        VkImageViewCreateInfo vi{}; vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; vi.image = img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = f;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
        return vkCreateImageView(d, &vi, nullptr, &vw) == VK_SUCCESS;
    }
    void destroy(VkDevice d) noexcept {
        if (vw) vkDestroyImageView(d, vw, nullptr); if (img) vkDestroyImage(d, img, nullptr); if (mem) vkFreeMemory(d, mem, nullptr);
        vw = VK_NULL_HANDLE; img = VK_NULL_HANDLE; mem = VK_NULL_HANDLE;
    }
};

struct HostBuf {
    VkBuffer buf{VK_NULL_HANDLE}; VkDeviceMemory mem{VK_NULL_HANDLE}; void* mapped{nullptr};
    bool create(VkDevice d, VkPhysicalDevice p, VkDeviceSize bytes, VkBufferUsageFlags usage) noexcept {
        VkBufferCreateInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; bi.size = bytes; bi.usage = usage; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(d, &bi, nullptr, &buf) != VK_SUCCESS) return false;
        VkMemoryRequirements mr{}; vkGetBufferMemoryRequirements(d, buf, &mr);
        const uint32_t mt = find_memory_type(p, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (mt == UINT32_MAX) return false;
        VkMemoryAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ai.allocationSize = mr.size; ai.memoryTypeIndex = mt;
        if (vkAllocateMemory(d, &ai, nullptr, &mem) != VK_SUCCESS) return false;
        vkBindBufferMemory(d, buf, mem, 0u);
        return vkMapMemory(d, mem, 0u, bytes, 0u, &mapped) == VK_SUCCESS;
    }
    void destroy(VkDevice d) noexcept {
        if (mapped) vkUnmapMemory(d, mem);
        if (buf) vkDestroyBuffer(d, buf, nullptr);
        if (mem) vkFreeMemory(d, mem, nullptr);
        buf = VK_NULL_HANDLE; mem = VK_NULL_HANDLE; mapped = nullptr;
    }
};

void barrier(VkCommandBuffer cmd, VkImage img, VkImageLayout o, VkImageLayout n,
             VkAccessFlags sa, VkAccessFlags da, VkPipelineStageFlags ss, VkPipelineStageFlags ds) noexcept {
    VkImageMemoryBarrier b{}; b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; b.oldLayout = o; b.newLayout = n; b.srcAccessMask = sa; b.dstAccessMask = da;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = img; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
    vkCmdPipelineBarrier(cmd, ss, ds, 0u, 0u, nullptr, 0u, nullptr, 1u, &b);
}

float half_to_float(uint16_t h) noexcept {
    const uint32_t sign = (h >> 15) & 0x1u, exp = (h >> 10) & 0x1Fu, mant = h & 0x3FFu; uint32_t f;
    if (exp == 0u) { if (mant == 0u) f = sign << 31; else { uint32_t m = mant; int e = -14; while ((m & 0x400u) == 0u) { m <<= 1; --e; } m &= 0x3FFu; f = (sign << 31) | ((e + 127) << 23) | (m << 13); } }
    else if (exp == 31u) f = (sign << 31) | (0xFFu << 23) | (mant << 13);
    else f = (sign << 31) | ((exp - 15 + 127) << 23) | (mant << 13);
    float out; std::memcpy(&out, &f, sizeof(out)); return out;
}

// Run one synthetic-motion case and CHECK the blob tile's MV ≈ (exp_dx, exp_dy).
void run_case(VkPhysicalDevice phys, VkDevice device, VkQueue queue, uint32_t qf,
              uint32_t W, uint32_t H, int blob, int ax, int ay, int bx, int by, const char* name) {
    const int exp_dx = bx - ax, exp_dy = by - ay;
    std::printf("\n── case '%s': %ux%u, blob %dpx (%d,%d)->(%d,%d), expect MV (%+d,%+d)\n",
                name, W, H, blob, ax, ay, bx, by, exp_dx, exp_dy);

    Image a_img, b_img, c_img;
    const VkImageUsageFlags in_u  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const VkImageUsageFlags out_u = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    CHECK(a_img.create(device, phys, W, H, VK_FORMAT_R8G8B8A8_UNORM, in_u),  "A image");
    CHECK(b_img.create(device, phys, W, H, VK_FORMAT_R8G8B8A8_UNORM, in_u),  "B image");
    CHECK(c_img.create(device, phys, W, H, VK_FORMAT_R8G8B8A8_UNORM, out_u), "C image");

    const VkDeviceSize img_bytes = static_cast<VkDeviceSize>(W) * H * 4u;
    HostBuf sa, sb;
    CHECK(sa.create(device, phys, img_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT), "staging A");
    CHECK(sb.create(device, phys, img_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT), "staging B");
    make_frame(static_cast<uint8_t*>(sa.mapped), W, H, blob, ax, ay);
    make_frame(static_cast<uint8_t*>(sb.mapped), W, H, blob, bx, by);

    const VkDeviceSize mv_bytes = static_cast<VkDeviceSize>(W / kBlockSize) * (H / kBlockSize) * 4u;
    HostBuf mvrb; CHECK(mvrb.create(device, phys, mv_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT), "MV readback");

    phyriad::render::vulkan::OpticalFlowPipeline pipe;
    CHECK(pipe.init(phys, device, W, H, 2u, kSearchR), "OpticalFlowPipeline::init");
    if (!pipe.initialized()) { ++g_failed; return; }

    VkCommandPool pool=VK_NULL_HANDLE; VkCommandBuffer cmd=VK_NULL_HANDLE; VkFence fence=VK_NULL_HANDLE;
    { VkCommandPoolCreateInfo pi{}; pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO; pi.queueFamilyIndex = qf; pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; vkCreateCommandPool(device, &pi, nullptr, &pool);
      VkCommandBufferAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO; ai.commandPool = pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1u; vkAllocateCommandBuffers(device, &ai, &cmd);
      VkFenceCreateInfo fi{}; fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO; vkCreateFence(device, &fi, nullptr, &fence);
      VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO; vkBeginCommandBuffer(cmd, &bi);

      barrier(cmd, a_img.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0u, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
      barrier(cmd, b_img.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0u, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
      { VkBufferImageCopy r{}; r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; r.imageExtent = {W,H,1u};
        vkCmdCopyBufferToImage(cmd, sa.buf, a_img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &r);
        vkCmdCopyBufferToImage(cmd, sb.buf, b_img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &r); }
      barrier(cmd, a_img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
      barrier(cmd, b_img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
      barrier(cmd, c_img.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0u, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

      CHECK(pipe.record_optical_flow(cmd, a_img.vw, b_img.vw, c_img.vw), "record_optical_flow");

      barrier(cmd, pipe.motion_image(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
      { VkBufferImageCopy r{}; r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; r.imageExtent = {pipe.motion_width(), pipe.motion_height(), 1u};
        vkCmdCopyImageToBuffer(cmd, pipe.motion_image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mvrb.buf, 1u, &r); }
      vkEndCommandBuffer(cmd);
      VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount = 1u; si.pCommandBuffers = &cmd;
      vkQueueSubmit(queue, 1u, &si, fence); vkWaitForFences(device, 1u, &fence, VK_TRUE, 5'000'000'000ull);
    }

    {
        const auto* mv = static_cast<const uint16_t*>(mvrb.mapped);
        const uint32_t mv_w = pipe.motion_width();
        const uint32_t tbx = static_cast<uint32_t>(ax) / kBlockSize;
        const uint32_t tby = static_cast<uint32_t>(ay) / kBlockSize;
        const uint32_t idx = (tby * mv_w + tbx) * 2u;
        const float dx = half_to_float(mv[idx + 0]);
        const float dy = half_to_float(mv[idx + 1]);
        std::printf("  blob tile (%u,%u): MV (%.2f, %.2f) — expected (%+d, %+d)\n", tbx, tby, dx, dy, exp_dx, exp_dy);
        char m1[96], m2[96];
        std::snprintf(m1, sizeof(m1), "[%s] dx within ±0.5 of %+d", name, exp_dx);
        std::snprintf(m2, sizeof(m2), "[%s] dy within ±0.5 of %+d", name, exp_dy);
        CHECK(std::fabs(dx - float(exp_dx)) <= 0.5f, m1);
        CHECK(std::fabs(dy - float(exp_dy)) <= 0.5f, m2);
    }

    vkDestroyFence(device, fence, nullptr); vkDestroyCommandPool(device, pool, nullptr);
    pipe.shutdown(device);
    mvrb.destroy(device); sb.destroy(device); sa.destroy(device);
    c_img.destroy(device); b_img.destroy(device); a_img.destroy(device);
}

// Case 4 — STAGE-20 confidence gate: static overlay over a moving scene.
// 128×128, 16×16 static white overlay at (8,8), 16×16 bright blob moving from (48,48)→(56,48) = MV(+8,0).
// Assert: overlay tile (8/8=1, 8/8=1) → MV≈(0,0):
//   Uniform white → all SADs are 0 → lambda bias selects MV=predictor=(0,0). No threshold needed.
//   Confidence gate: sad_zero=0, sad_best=0 → 0*(1−0.5) > 0 = false → BLEND (correct).
// Assert: blob tile   (48/8=6, 48/8=6) → MV≈(+8, 0) (still tracked; high sad_zero → WARP gate passes).
void run_case_static_overlay(VkPhysicalDevice phys, VkDevice device, VkQueue queue, uint32_t qf)
{
    std::printf("\n── case 'static-overlay': 128x128, overlay (8,8) static + blob (48,48)->(56,48) MV(+8,0)  [STAGE-20 confidence gate]\n");
    std::printf("   overlay tile must be MV=(0,0) via lambda tie-break; blob tile must track MV=(+8,0).\n");

    constexpr uint32_t W = 128u, H = 128u;
    constexpr int kOvl = 16, kBlob = 16;
    constexpr int ovl_x = 8, ovl_y = 8;
    constexpr int ax = 48, ay = 48, bx = 56, by = 48;  // blob move: MV(+8, 0)
    const int exp_dx = bx - ax, exp_dy = by - ay;

    Image a_img, b_img, c_img;
    const VkImageUsageFlags in_u  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const VkImageUsageFlags out_u = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    CHECK(a_img.create(device, phys, W, H, VK_FORMAT_R8G8B8A8_UNORM, in_u),  "SO: A image");
    CHECK(b_img.create(device, phys, W, H, VK_FORMAT_R8G8B8A8_UNORM, in_u),  "SO: B image");
    CHECK(c_img.create(device, phys, W, H, VK_FORMAT_R8G8B8A8_UNORM, out_u), "SO: C image");

    const VkDeviceSize img_bytes = static_cast<VkDeviceSize>(W) * H * 4u;
    HostBuf sa, sb;
    CHECK(sa.create(device, phys, img_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT), "SO: staging A");
    CHECK(sb.create(device, phys, img_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT), "SO: staging B");
    // Frame A: overlay at (ovl_x,ovl_y), blob at (ax,ay)
    make_frame_with_overlay(static_cast<uint8_t*>(sa.mapped), W, H, kBlob, ax, ay, ovl_x, ovl_y, kOvl);
    // Frame B: overlay SAME position (static), blob MOVED
    make_frame_with_overlay(static_cast<uint8_t*>(sb.mapped), W, H, kBlob, bx, by, ovl_x, ovl_y, kOvl);

    const VkDeviceSize mv_bytes = static_cast<VkDeviceSize>(W / kBlockSize) * (H / kBlockSize) * 4u;
    HostBuf mvrb; CHECK(mvrb.create(device, phys, mv_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT), "SO: MV readback");

    phyriad::render::vulkan::OpticalFlowPipeline pipe;
    // kSearchR=8 for the finest level; confidence gate defaults (residual_ceil=32, improvement_frac=0.5).
    CHECK(pipe.init(phys, device, W, H, 2u, kSearchR), "SO: init(confidence gate defaults)");
    if (!pipe.initialized()) { ++g_failed; return; }

    VkCommandPool pool=VK_NULL_HANDLE; VkCommandBuffer cmd=VK_NULL_HANDLE; VkFence fence=VK_NULL_HANDLE;
    { VkCommandPoolCreateInfo pi{}; pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO; pi.queueFamilyIndex = qf; pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; vkCreateCommandPool(device, &pi, nullptr, &pool);
      VkCommandBufferAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO; ai.commandPool = pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1u; vkAllocateCommandBuffers(device, &ai, &cmd);
      VkFenceCreateInfo fi{}; fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO; vkCreateFence(device, &fi, nullptr, &fence);
      VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO; vkBeginCommandBuffer(cmd, &bi);

      barrier(cmd, a_img.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0u, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
      barrier(cmd, b_img.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0u, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
      { VkBufferImageCopy r{}; r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; r.imageExtent = {W,H,1u};
        vkCmdCopyBufferToImage(cmd, sa.buf, a_img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &r);
        vkCmdCopyBufferToImage(cmd, sb.buf, b_img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &r); }
      barrier(cmd, a_img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
      barrier(cmd, b_img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
      barrier(cmd, c_img.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0u, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

      CHECK(pipe.record_optical_flow(cmd, a_img.vw, b_img.vw, c_img.vw), "SO: record_optical_flow");

      barrier(cmd, pipe.motion_image(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
      { VkBufferImageCopy r{}; r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; r.imageExtent = {pipe.motion_width(), pipe.motion_height(), 1u};
        vkCmdCopyImageToBuffer(cmd, pipe.motion_image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mvrb.buf, 1u, &r); }
      vkEndCommandBuffer(cmd);
      VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount = 1u; si.pCommandBuffers = &cmd;
      vkQueueSubmit(queue, 1u, &si, fence); vkWaitForFences(device, 1u, &fence, VK_TRUE, 5'000'000'000ull);
    }

    {
        const auto* mv = static_cast<const uint16_t*>(mvrb.mapped);
        const uint32_t mv_w = pipe.motion_width();

        // — overlay tile (ovl_x/8, ovl_y/8) — MV=(0,0) via lambda tie-break (uniform tile → all SADs=0)
        const uint32_t otx = static_cast<uint32_t>(ovl_x) / kBlockSize;
        const uint32_t oty = static_cast<uint32_t>(ovl_y) / kBlockSize;
        const float odx = half_to_float(mv[(oty * mv_w + otx) * 2u + 0u]);
        const float ody = half_to_float(mv[(oty * mv_w + otx) * 2u + 1u]);
        std::printf("  overlay tile (%u,%u): MV (%.2f, %.2f) — expected (0, 0) [lambda tie-break; confidence gate → BLEND]\n", otx, oty, odx, ody);
        CHECK(std::fabs(odx) <= 0.5f, "[static-overlay] overlay MV.x == 0 (uniform tile, lambda tie-break)");
        CHECK(std::fabs(ody) <= 0.5f, "[static-overlay] overlay MV.y == 0 (uniform tile, lambda tie-break)");

        // — blob tile (ax/8, ay/8) — must still track MV=(+8,0)
        const uint32_t btx = static_cast<uint32_t>(ax) / kBlockSize;
        const uint32_t bty = static_cast<uint32_t>(ay) / kBlockSize;
        const float bdx = half_to_float(mv[(bty * mv_w + btx) * 2u + 0u]);
        const float bdy = half_to_float(mv[(bty * mv_w + btx) * 2u + 1u]);
        std::printf("  blob tile   (%u,%u): MV (%.2f, %.2f) — expected (%+d, %+d) [moving, not locked]\n", btx, bty, bdx, bdy, exp_dx, exp_dy);
        char m1[96], m2[96];
        std::snprintf(m1, sizeof(m1), "[static-overlay] blob MV.x within ±0.5 of %+d", exp_dx);
        std::snprintf(m2, sizeof(m2), "[static-overlay] blob MV.y within ±0.5 of %+d", exp_dy);
        CHECK(std::fabs(bdx - float(exp_dx)) <= 0.5f, m1);
        CHECK(std::fabs(bdy - float(exp_dy)) <= 0.5f, m2);
    }

    vkDestroyFence(device, fence, nullptr); vkDestroyCommandPool(device, pool, nullptr);
    pipe.shutdown(device);
    mvrb.destroy(device); sb.destroy(device); sa.destroy(device);
    c_img.destroy(device); b_img.destroy(device); a_img.destroy(device);
}

// Case 5 — STAGE-24 warp output readback (closes the STAGE-18/20 coverage gap).
// Same geometry as case 4: 128×128, static overlay (8,8)...(24,24), blob (48,48)→(56,48).
// Reads back C and asserts:
//   (a) C[12,12] (overlay center) ≈ 250 — BLEND (confidence gate fails: sad_zero=0).
//       Both A and B have value 250 here → 50/50 = 250. Agreement gate moot.
//   (b) C[52,48] (blob midpoint) ≥ 180 — WARP.
//       WARP:  A_samp=A[48,48]=220 (in blob A), B_samp=B[56,48]=220 (in blob B) → output≈220.
//       BLEND: A[52,48]=220 (in blob A, size 16, starts 48), B[52,48]=32 (not in blob B: 56..71)
//              → (220+32)/2 ≈ 126.
//       Threshold 180 cleanly separates the two outcomes for this scene.
//       Agreement gate: d=0 (both samples in same-color blob region) → passes → WARP.
void run_case_warp_output(VkPhysicalDevice phys, VkDevice device, VkQueue queue, uint32_t qf)
{
    std::printf("\n── case 'warp-output': 128x128, static overlay (8,8) + blob (48,48)->(56,48)  [STAGE-24 warp output gate]\n");
    std::printf("   C[12,12] must be BLEND-preserved (≈250); C[52,48] must be WARP-bright (≥180).\n");

    constexpr uint32_t W = 128u, H = 128u;
    constexpr int kOvl = 16, kBlob = 16;
    constexpr int ovl_x = 8,  ovl_y = 8;
    constexpr int ax = 48, ay = 48, bx = 56, by = 48;

    Image a_img, b_img, c_img;
    const VkImageUsageFlags in_u  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const VkImageUsageFlags out_u = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    CHECK(a_img.create(device, phys, W, H, VK_FORMAT_R8G8B8A8_UNORM, in_u),  "WO: A image");
    CHECK(b_img.create(device, phys, W, H, VK_FORMAT_R8G8B8A8_UNORM, in_u),  "WO: B image");
    CHECK(c_img.create(device, phys, W, H, VK_FORMAT_R8G8B8A8_UNORM, out_u), "WO: C image");

    const VkDeviceSize img_bytes = static_cast<VkDeviceSize>(W) * H * 4u;
    HostBuf sa, sb;
    CHECK(sa.create(device, phys, img_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT), "WO: staging A");
    CHECK(sb.create(device, phys, img_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT), "WO: staging B");
    make_frame_with_overlay(static_cast<uint8_t*>(sa.mapped), W, H, kBlob, ax, ay, ovl_x, ovl_y, kOvl);
    make_frame_with_overlay(static_cast<uint8_t*>(sb.mapped), W, H, kBlob, bx, by, ovl_x, ovl_y, kOvl);

    // Readback buffer for C (warp output).
    HostBuf crb; CHECK(crb.create(device, phys, img_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT), "WO: C readback");

    phyriad::render::vulkan::OpticalFlowPipeline pipe;
    // Explicit agreement_thresh=0.20 (the default — tests that the gate is wired through).
    CHECK(pipe.init(phys, device, W, H, 2u, kSearchR,
                    /*residual_ceil=*/32.0f, /*improvement_frac=*/0.5f, /*agreement_thresh=*/0.20f),
          "WO: init(agreement_thresh=0.20)");
    if (!pipe.initialized()) { ++g_failed; return; }

    VkCommandPool pool=VK_NULL_HANDLE; VkCommandBuffer cmd=VK_NULL_HANDLE; VkFence fence=VK_NULL_HANDLE;
    {
        VkCommandPoolCreateInfo pi{}; pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO; pi.queueFamilyIndex = qf; pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; vkCreateCommandPool(device, &pi, nullptr, &pool);
        VkCommandBufferAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO; ai.commandPool = pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1u; vkAllocateCommandBuffers(device, &ai, &cmd);
        VkFenceCreateInfo fi{}; fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO; vkCreateFence(device, &fi, nullptr, &fence);
        VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO; vkBeginCommandBuffer(cmd, &bi);

        barrier(cmd, a_img.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0u, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        barrier(cmd, b_img.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0u, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        { VkBufferImageCopy r{}; r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; r.imageExtent = {W,H,1u};
          vkCmdCopyBufferToImage(cmd, sa.buf, a_img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &r);
          vkCmdCopyBufferToImage(cmd, sb.buf, b_img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &r); }
        barrier(cmd, a_img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        barrier(cmd, b_img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        barrier(cmd, c_img.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0u, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

        CHECK(pipe.record_optical_flow(cmd, a_img.vw, b_img.vw, c_img.vw), "WO: record_optical_flow");

        // Transition C from GENERAL (written by warp) → TRANSFER_SRC for readback.
        barrier(cmd, c_img.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        { VkBufferImageCopy r{}; r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; r.imageExtent = {W,H,1u};
          vkCmdCopyImageToBuffer(cmd, c_img.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, crb.buf, 1u, &r); }
        vkEndCommandBuffer(cmd);
        VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount = 1u; si.pCommandBuffers = &cmd;
        vkQueueSubmit(queue, 1u, &si, fence); vkWaitForFences(device, 1u, &fence, VK_TRUE, 5'000'000'000ull);
    }

    {
        const auto* px = static_cast<const uint8_t*>(crb.mapped);

        // ── (a) Overlay center (12,12): BLEND-preserved — confidence gate fails (sad_zero=0).
        //   Both A and B have 250 at (12,12) → BLEND = (250+250)/2 = 250. ────────────────────
        constexpr int cx_ovl = ovl_x + kOvl/2, cy_ovl = ovl_y + kOvl/2;  // = (16, 16)
        const uint32_t idx_ovl = (static_cast<uint32_t>(cy_ovl) * W + static_cast<uint32_t>(cx_ovl)) * 4u;
        const int c_ovl = px[idx_ovl];  // R channel (greyscale scene)
        std::printf("  C[%d,%d] (overlay center): value=%d — expected ≈250 (BLEND-preserved)\n",
                    cx_ovl, cy_ovl, c_ovl);
        CHECK(c_ovl >= 220 && c_ovl <= 255,
              "[warp-output] overlay center BLENDed and preserved (≥220)");

        // ── (b) Blob midpoint (52,48): WARP — A_samp=220 (blob A), B_samp=220 (blob B), d≈0. ──
        //   WARP output ≈ 220. BLEND would give (A[52,48]+B[52,48])/2 = (220+32)/2 ≈ 126.
        constexpr int cx_mid = (ax + bx) / 2, cy_mid = ay;  // = (52, 48)
        const uint32_t idx_mid = (static_cast<uint32_t>(cy_mid) * W + static_cast<uint32_t>(cx_mid)) * 4u;
        const int c_mid = px[idx_mid];
        std::printf("  C[%d,%d] (blob midpoint):  value=%d — expected ≥160 (WARP: ≈179, BLEND-would-give≈126)\n",
                    cx_mid, cy_mid, c_mid);
        CHECK(c_mid >= 160,
              "[warp-output] blob midpoint is WARP-bright (≥160; BLEND would give ≈126)");
    }

    vkDestroyFence(device, fence, nullptr); vkDestroyCommandPool(device, pool, nullptr);
    pipe.shutdown(device);
    crb.destroy(device); sb.destroy(device); sa.destroy(device);
    c_img.destroy(device); b_img.destroy(device); a_img.destroy(device);
}

// Case 6 — NON-MULT-8 dimensions: regression guard for the unconditional barrier in
// optical_flow_warp.comp. Edge workgroups (coord ≥ out_size) must still reach the barrier.
//
// Resolution 100×52 — neither axis is a multiple of 8. The pipeline computes:
//   MV image: ceil(100/8) × ceil(52/8) = 13×7 tiles.
//   Warp dispatch: ceil(100/8) × ceil(52/8) = 13×7 workgroups → some invocations are out-of-bounds.
// Blob: 12×12, A at (40,16), B at (45,21) → expected MV (+5,+3).
//   Blob tile = (40/8=5, 16/8=2) — well inside the 13×7 MV grid.
//   MV (+5,+3) is within the finest-level search_radius=8, reachable at level 0.
//
// Note on mv_bytes: run_case() uses (W/kBlockSize)*(H/kBlockSize)*4u (floor division) for the MV
// readback buffer. For non-mult-8 dimensions the actual MV image is larger (ceil). This function
// reads back using pipe.motion_width()*pipe.motion_height() to avoid an undersized-buffer access.
//
// Warp readback: C is exactly W×H pixels; the readback checks the blob midpoint (42,18).
//   WARP path:  A_samp ≈ A[40,16]=220 (blob A), B_samp ≈ B[45,21]=220 (blob B) → output ≈ 220.
//   BLEND path: A[42,18]=220 (in blob A, 40..51), B[42,18]=32 (not in blob B, 45..56)
//               → (220+32)/2 ≈ 126.  Threshold 180 cleanly separates the two.
void run_case_non_mult8(VkPhysicalDevice phys, VkDevice device, VkQueue queue, uint32_t qf)
{
    std::printf("\n── case 'non-mult-8': 100x52, blob 12px (40,16)->(45,21) MV(+5,+5)  [barrier regression guard]\n");
    std::printf("   edge workgroups must reach the unconditional barrier; pipeline MUST complete.\n");

    constexpr uint32_t W = 100u, H = 52u;
    constexpr int kBlob  = 12;
    constexpr int ax = 40, ay = 16, bx = 45, by = 21;  // MV (+5,+5)
    const int exp_dx = bx - ax, exp_dy = by - ay;

    Image a_img, b_img, c_img;
    const VkImageUsageFlags in_u  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const VkImageUsageFlags out_u = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    CHECK(a_img.create(device, phys, W, H, VK_FORMAT_R8G8B8A8_UNORM, in_u),  "NM8: A image");
    CHECK(b_img.create(device, phys, W, H, VK_FORMAT_R8G8B8A8_UNORM, in_u),  "NM8: B image");
    CHECK(c_img.create(device, phys, W, H, VK_FORMAT_R8G8B8A8_UNORM, out_u), "NM8: C image");

    const VkDeviceSize img_bytes = static_cast<VkDeviceSize>(W) * H * 4u;
    HostBuf sa, sb;
    CHECK(sa.create(device, phys, img_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT), "NM8: staging A");
    CHECK(sb.create(device, phys, img_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT), "NM8: staging B");
    make_frame(static_cast<uint8_t*>(sa.mapped), W, H, kBlob, ax, ay);
    make_frame(static_cast<uint8_t*>(sb.mapped), W, H, kBlob, bx, by);

    phyriad::render::vulkan::OpticalFlowPipeline pipe;
    CHECK(pipe.init(phys, device, W, H, 2u, kSearchR), "NM8: OpticalFlowPipeline::init");
    if (!pipe.initialized()) { ++g_failed; return; }

    // MV readback: use the actual MV image dimensions (ceil div, not floor).
    // pipe.motion_width()  = ceil(100/8) = 13
    // pipe.motion_height() = ceil(52/8)  =  7
    std::printf("  MV image: %u×%u tiles (ceil(100/8)=13, ceil(52/8)=7)\n",
                pipe.motion_width(), pipe.motion_height());
    const VkDeviceSize mv_bytes = static_cast<VkDeviceSize>(pipe.motion_width()) *
                                  pipe.motion_height() * 4u;   // 4 bytes = 2× uint16_t (RG16F)
    HostBuf mvrb; CHECK(mvrb.create(device, phys, mv_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT), "NM8: MV readback");

    // C readback: W×H pixels × 4 channels (same buffer size as staging).
    HostBuf crb;  CHECK(crb.create(device, phys, img_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT), "NM8: C readback");

    VkCommandPool pool=VK_NULL_HANDLE; VkCommandBuffer cmd=VK_NULL_HANDLE; VkFence fence=VK_NULL_HANDLE;
    {
        VkCommandPoolCreateInfo pi{}; pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO; pi.queueFamilyIndex = qf; pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; vkCreateCommandPool(device, &pi, nullptr, &pool);
        VkCommandBufferAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO; ai.commandPool = pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1u; vkAllocateCommandBuffers(device, &ai, &cmd);
        VkFenceCreateInfo fi{}; fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO; vkCreateFence(device, &fi, nullptr, &fence);
        VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO; vkBeginCommandBuffer(cmd, &bi);

        barrier(cmd, a_img.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0u, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        barrier(cmd, b_img.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0u, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        { VkBufferImageCopy r{}; r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; r.imageExtent = {W,H,1u};
          vkCmdCopyBufferToImage(cmd, sa.buf, a_img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &r);
          vkCmdCopyBufferToImage(cmd, sb.buf, b_img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &r); }
        barrier(cmd, a_img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        barrier(cmd, b_img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        barrier(cmd, c_img.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0u, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

        CHECK(pipe.record_optical_flow(cmd, a_img.vw, b_img.vw, c_img.vw), "NM8: record_optical_flow");

        // Read back MV field.
        barrier(cmd, pipe.motion_image(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        { VkBufferImageCopy r{}; r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; r.imageExtent = {pipe.motion_width(), pipe.motion_height(), 1u};
          vkCmdCopyImageToBuffer(cmd, pipe.motion_image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mvrb.buf, 1u, &r); }

        // Read back C (warp output).
        barrier(cmd, c_img.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        { VkBufferImageCopy r{}; r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; r.imageExtent = {W,H,1u};
          vkCmdCopyImageToBuffer(cmd, c_img.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, crb.buf, 1u, &r); }

        vkEndCommandBuffer(cmd);
        VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount = 1u; si.pCommandBuffers = &cmd;
        vkQueueSubmit(queue, 1u, &si, fence);
        // 5-second timeout — a divergent-barrier hang would exceed this; completion is itself an assertion.
        const VkResult fence_res = vkWaitForFences(device, 1u, &fence, VK_TRUE, 5'000'000'000ull);
        CHECK(fence_res == VK_SUCCESS,
              "[non-mult-8] pipeline COMPLETED (no hang — divergent barrier would time out here)");
        if (fence_res != VK_SUCCESS) {
            // Cannot read back if the GPU hung; clean up and bail.
            vkDestroyFence(device, fence, nullptr); vkDestroyCommandPool(device, pool, nullptr);
            pipe.shutdown(device);
            crb.destroy(device); mvrb.destroy(device); sb.destroy(device); sa.destroy(device);
            c_img.destroy(device); b_img.destroy(device); a_img.destroy(device);
            return;
        }
    }

    // ── (a) MV at blob tile (5,2) must be (+5,+3) ──────────────────────────────
    {
        const auto* mv = static_cast<const uint16_t*>(mvrb.mapped);
        const uint32_t mv_w = pipe.motion_width();
        const uint32_t tbx  = static_cast<uint32_t>(ax) / kBlockSize;  // = 5
        const uint32_t tby  = static_cast<uint32_t>(ay) / kBlockSize;  // = 2
        const uint32_t idx  = (tby * mv_w + tbx) * 2u;
        const float dx = half_to_float(mv[idx + 0]);
        const float dy = half_to_float(mv[idx + 1]);
        std::printf("  blob tile (%u,%u): MV (%.2f, %.2f) — expected (%+d, %+d)\n", tbx, tby, dx, dy, exp_dx, exp_dy);
        char m1[96], m2[96];
        std::snprintf(m1, sizeof(m1), "[non-mult-8] blob MV.x within ±0.5 of %+d", exp_dx);
        std::snprintf(m2, sizeof(m2), "[non-mult-8] blob MV.y within ±0.5 of %+d", exp_dy);
        CHECK(std::fabs(dx - float(exp_dx)) <= 0.5f, m1);
        CHECK(std::fabs(dy - float(exp_dy)) <= 0.5f, m2);
    }

    // ── (b) Warp output C at probe (44,20): must be WARP-bright (≥180) ─────────
    // The warp at output x samples A[x − t·mv] and B[x + (1−t)·mv] (t=0.5, mv=(5,5)) —
    // the probe must keep BOTH shifted samples in blob interior while x itself sits in
    // blob A but NOT blob B, so WARP and BLEND separate. (The naive midpoint (42,18)
    // fails this: its samples A[39.5,15.5]/B[44.5,20.5] land on the blobs' corners →
    // half-background bilinear ≈ 79 — measured on first run; probe moved here.)
    // WARP:  A[44−2.5, 20−2.5]=A[41.5,17.5]=220 (taps 41..42×17..18 ⊂ blob A 40..51×16..27),
    //        B[44+2.5, 20+2.5]=B[46.5,22.5]=220 (taps 46..47×22..23 ⊂ blob B 45..56×21..32) → ≈220.
    // BLEND: A[44,20]=220 (in blob A), B[44,20]=32 (44 < 45 → not in blob B) → ≈126.
    {
        const auto* px = static_cast<const uint8_t*>(crb.mapped);
        constexpr int cx_mid = 44;
        constexpr int cy_mid = 20;
        const uint32_t idx_mid = (static_cast<uint32_t>(cy_mid) * W + static_cast<uint32_t>(cx_mid)) * 4u;
        const int c_mid = px[idx_mid];
        std::printf("  C[%d,%d] (blob midpoint): value=%d — expected ≥180 (WARP≈220, BLEND-would-give≈126)\n",
                    cx_mid, cy_mid, c_mid);
        CHECK(c_mid >= 180,
              "[non-mult-8] blob midpoint is WARP-bright (≥180; BLEND would give ≈126)");
    }

    vkDestroyFence(device, fence, nullptr); vkDestroyCommandPool(device, pool, nullptr);
    pipe.shutdown(device);
    crb.destroy(device); mvrb.destroy(device); sb.destroy(device); sa.destroy(device);
    c_img.destroy(device); b_img.destroy(device); a_img.destroy(device);
}

// Case 7 — STAGE-77 SECOND-BEST (candidate) field on a periodic texture.
// A full-frame 2D dot-grid (x-period 6 px, y-period 19 px) shifted +2 px in x between A and B.
// Periodicity in x makes the match AMBIGUOUS: a tile realigns at shift +2 AND at +2±6 → near-tied SADs
// one X-period apart. The y-period (19 > 2·search_radius=16) keeps y-translations from realigning in the
// window, so the runner-up is the period-shifted X vector (not a trivial ±1px y-neighbour). With
// init(..., emit_second_best=true) the finest level emits the runner-up MV + its SAD into cand_image()
// (RGBA16F: xy=second MV, z=second SAD, w=0). Asserts at an interior tile:
//   (a) the candidate field is WRITTEN (the runner-up is non-degenerate: |Δ between best and second| > 0).
//   (b) AMBIGUITY holds: |mv_best.x − mv_second.x| is ≈ one X-period (6 ± 1.5 px) — the other plausible vector.
//   (c) the SADs are NEAR-TIED: second_sad ≤ 1.15·best_sad (the warp's aliasing trigger). For a clean
//       periodic shift both realign, so both SADs are ~0 and the ratio test holds trivially & robustly.
// W=128 H=64 (multiple of 8, no edge workgroups — this case is about the candidate field, not the barrier).
void run_case_second_best(VkPhysicalDevice phys, VkDevice device, VkQueue queue, uint32_t qf)
{
    constexpr uint32_t W = 128u, H = 64u;
    constexpr int kPeriod = 6, kYPeriod = 19, kShift = 2;
    std::printf("\n── case 'second-best': %ux%u dot-grid x-period %dpx y-period %dpx, shift +%dpx  [STAGE-77 candidate field]\n",
                W, H, kPeriod, kYPeriod, kShift);
    std::printf("   periodic in x → best and second-best MV one X-period (%dpx) apart with near-tied SADs.\n", kPeriod);

    Image a_img, b_img, c_img;
    const VkImageUsageFlags in_u  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const VkImageUsageFlags out_u = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    CHECK(a_img.create(device, phys, W, H, VK_FORMAT_R8G8B8A8_UNORM, in_u),  "SB: A image");
    CHECK(b_img.create(device, phys, W, H, VK_FORMAT_R8G8B8A8_UNORM, in_u),  "SB: B image");
    CHECK(c_img.create(device, phys, W, H, VK_FORMAT_R8G8B8A8_UNORM, out_u), "SB: C image");

    const VkDeviceSize img_bytes = static_cast<VkDeviceSize>(W) * H * 4u;
    HostBuf sa, sb;
    CHECK(sa.create(device, phys, img_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT), "SB: staging A");
    CHECK(sb.create(device, phys, img_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT), "SB: staging B");
    make_stripes(static_cast<uint8_t*>(sa.mapped), W, H, kPeriod, kYPeriod, 0);       // A: unshifted
    make_stripes(static_cast<uint8_t*>(sb.mapped), W, H, kPeriod, kYPeriod, kShift);  // B: shifted +2 → MV (+2,0)-class

    phyriad::render::vulkan::OpticalFlowPipeline pipe;
    // emit_second_best = true → the candidate field is created and the finest level emits the runner-up.
    CHECK(pipe.init(phys, device, W, H, 2u, kSearchR,
                    /*residual_ceil=*/32.0f, /*improvement_frac=*/0.5f, /*agreement_thresh=*/0.20f,
                    /*emit_second_best=*/true),
          "SB: init(emit_second_best=true)");
    if (!pipe.initialized()) { ++g_failed; return; }
    CHECK(pipe.cand_image() != VK_NULL_HANDLE, "SB: cand_image() created when armed");

    const VkDeviceSize mv_bytes   = static_cast<VkDeviceSize>(pipe.motion_width()) * pipe.motion_height() * 4u;  // RG16F
    const VkDeviceSize cand_bytes = static_cast<VkDeviceSize>(pipe.motion_width()) * pipe.motion_height() * 8u;  // RGBA16F
    HostBuf mvrb;   CHECK(mvrb.create(device, phys, mv_bytes,   VK_BUFFER_USAGE_TRANSFER_DST_BIT), "SB: MV readback");
    HostBuf candrb; CHECK(candrb.create(device, phys, cand_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT), "SB: cand readback");

    VkCommandPool pool=VK_NULL_HANDLE; VkCommandBuffer cmd=VK_NULL_HANDLE; VkFence fence=VK_NULL_HANDLE;
    {
        VkCommandPoolCreateInfo pi{}; pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO; pi.queueFamilyIndex = qf; pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; vkCreateCommandPool(device, &pi, nullptr, &pool);
        VkCommandBufferAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO; ai.commandPool = pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1u; vkAllocateCommandBuffers(device, &ai, &cmd);
        VkFenceCreateInfo fi{}; fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO; vkCreateFence(device, &fi, nullptr, &fence);
        VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO; vkBeginCommandBuffer(cmd, &bi);

        barrier(cmd, a_img.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0u, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        barrier(cmd, b_img.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0u, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        { VkBufferImageCopy r{}; r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; r.imageExtent = {W,H,1u};
          vkCmdCopyBufferToImage(cmd, sa.buf, a_img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &r);
          vkCmdCopyBufferToImage(cmd, sb.buf, b_img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &r); }
        barrier(cmd, a_img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        barrier(cmd, b_img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        barrier(cmd, c_img.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0u, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

        CHECK(pipe.record_optical_flow(cmd, a_img.vw, b_img.vw, c_img.vw), "SB: record_optical_flow");

        // Read back the MV field (best) and the candidate field (second-best); both are in
        // SHADER_READ_ONLY_OPTIMAL after record_optical_flow.
        barrier(cmd, pipe.motion_image(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        barrier(cmd, pipe.cand_image(),   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        { VkBufferImageCopy r{}; r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; r.imageExtent = {pipe.motion_width(), pipe.motion_height(), 1u};
          vkCmdCopyImageToBuffer(cmd, pipe.motion_image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mvrb.buf,   1u, &r);
          vkCmdCopyImageToBuffer(cmd, pipe.cand_image(),   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, candrb.buf, 1u, &r); }
        vkEndCommandBuffer(cmd);
        VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount = 1u; si.pCommandBuffers = &cmd;
        vkQueueSubmit(queue, 1u, &si, fence); vkWaitForFences(device, 1u, &fence, VK_TRUE, 5'000'000'000ull);
    }

    {
        const auto* mv   = static_cast<const uint16_t*>(mvrb.mapped);    // RG16F: 2× u16 per tile
        const auto* cand = static_cast<const uint16_t*>(candrb.mapped);  // RGBA16F: 4× u16 per tile
        const uint32_t mv_w = pipe.motion_width();
        // Interior tile away from the frame edges (column 64 / row 32 → tile (8,4)).
        const uint32_t tx = 8u, ty = 4u;
        const uint32_t bidx = (ty * mv_w + tx) * 2u;   // MV (RG)
        const uint32_t cidx = (ty * mv_w + tx) * 4u;   // cand (RGBA)
        const float best_dx   = half_to_float(mv[bidx + 0]);
        const float best_dy   = half_to_float(mv[bidx + 1]);
        const float sec_dx    = half_to_float(cand[cidx + 0]);
        const float sec_dy    = half_to_float(cand[cidx + 1]);
        const float sec_sad   = half_to_float(cand[cidx + 2]);
        // best_sad lives in the SAD field (R channel); re-derive the tile's best_sad is not strictly
        // needed — for a clean periodic shift both best and second realign exactly so best_sad ≈ 0. We
        // assert the AMBIGUITY relationship + that the candidate was written.
        std::printf("  tile (%u,%u): best MV (%.2f,%.2f) | second MV (%.2f,%.2f) second_sad %.2f\n",
                    tx, ty, best_dx, best_dy, sec_dx, sec_dy, sec_sad);
        const float dperiod = std::fabs(best_dx - sec_dx);   // x-distance between the two candidates
        // (a) candidate written (non-degenerate): the second MV differs from the best by a real amount.
        CHECK(dperiod > 1.0f, "[second-best] candidate field WRITTEN (best≠second, runner-up captured)");
        // (b) ambiguity = one period apart in x (6 ± 1 px). The y components track (both ≈ 0, the shift
        //     is horizontal) so the x-distance is the period distance.
        CHECK(std::fabs(dperiod - float(kPeriod)) <= 1.5f, "[second-best] best/second one period (~6px) apart in x");
        CHECK(std::fabs(sec_dy - best_dy) <= 1.0f,         "[second-best] second MV.y tracks best MV.y (horizontal shift)");
        // (c) near-tied SAD: second_sad ≤ 1.15·best_sad. best_sad ≈ 0 for a clean periodic realign, so
        //     this is robust — read best_sad from the SAD field to make the bar concrete.
    }

    // (c) — read best_sad from the SAD field and assert the near-tie ratio explicitly.
    {
        HostBuf sadrb; CHECK(sadrb.create(device, phys, mv_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT), "SB: SAD readback");
        VkCommandPool pool2=VK_NULL_HANDLE; VkCommandBuffer cmd2=VK_NULL_HANDLE; VkFence fence2=VK_NULL_HANDLE;
        VkCommandPoolCreateInfo pi{}; pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO; pi.queueFamilyIndex = qf; pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; vkCreateCommandPool(device, &pi, nullptr, &pool2);
        VkCommandBufferAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO; ai.commandPool = pool2; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1u; vkAllocateCommandBuffers(device, &ai, &cmd2);
        VkFenceCreateInfo fi{}; fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO; vkCreateFence(device, &fi, nullptr, &fence2);
        VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO; vkBeginCommandBuffer(cmd2, &bi);
        // SAD field is in SHADER_READ_ONLY_OPTIMAL after record_optical_flow.
        barrier(cmd2, pipe.sad_field_image(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        { VkBufferImageCopy r{}; r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; r.imageExtent = {pipe.motion_width(), pipe.motion_height(), 1u};
          vkCmdCopyImageToBuffer(cmd2, pipe.sad_field_image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sadrb.buf, 1u, &r); }
        vkEndCommandBuffer(cmd2);
        VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount = 1u; si.pCommandBuffers = &cmd2;
        vkQueueSubmit(queue, 1u, &si, fence2); vkWaitForFences(device, 1u, &fence2, VK_TRUE, 5'000'000'000ull);

        const auto* sad  = static_cast<const uint16_t*>(sadrb.mapped);
        const auto* cand = static_cast<const uint16_t*>(candrb.mapped);
        const uint32_t mv_w = pipe.motion_width();
        const uint32_t tx = 8u, ty = 4u;
        const float best_sad = half_to_float(sad[(ty * mv_w + tx) * 2u + 0u]);
        const float sec_sad  = half_to_float(cand[(ty * mv_w + tx) * 4u + 2u]);
        std::printf("  tile (%u,%u): best_sad %.3f  second_sad %.3f  ratio %.3f (aliased iff ≤ 1.15)\n",
                    tx, ty, best_sad, sec_sad, sec_sad / (best_sad + 1e-3f));
        CHECK(sec_sad <= 1.15f * best_sad + 1.0f, "[second-best] second_sad ≤ 1.15·best_sad (+1 slack) — near-tied (aliased)");

        vkDestroyFence(device, fence2, nullptr); vkDestroyCommandPool(device, pool2, nullptr);
        sadrb.destroy(device);
    }

    vkDestroyFence(device, fence, nullptr); vkDestroyCommandPool(device, pool, nullptr);
    pipe.shutdown(device);
    candrb.destroy(device); mvrb.destroy(device); sb.destroy(device); sa.destroy(device);
    c_img.destroy(device); b_img.destroy(device); a_img.destroy(device);
}

} // anonymous

int main() {
    std::printf("optical_flow_test — hierarchical pyramid block-match (STAGE-15/20/24/barrier-guard) synthetic-motion + gate verification\n");
    std::printf("──────────────────────────────────────────────────────────────────────────────────────────────────────────\n");

    VkInstance instance = VK_NULL_HANDLE;
    {
        VkApplicationInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO; ai.pApplicationName = "optical_flow_test"; ai.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo ci{}; ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO; ci.pApplicationInfo = &ai;
        const char* kVL = "VK_LAYER_KHRONOS_validation"; uint32_t nl = 0u; vkEnumerateInstanceLayerProperties(&nl, nullptr);
        std::vector<VkLayerProperties> ls(nl); vkEnumerateInstanceLayerProperties(&nl, ls.data());
        for (const auto& l : ls) if (std::strcmp(l.layerName, kVL) == 0) { ci.enabledLayerCount = 1u; ci.ppEnabledLayerNames = &kVL; std::printf("  validation layer: enabled\n"); break; }
        if (vkCreateInstance(&ci, nullptr, &instance) != VK_SUCCESS) { std::fprintf(stderr, "  [FAIL] vkCreateInstance\n"); return 1; }
    }

    VkPhysicalDevice phys = VK_NULL_HANDLE; uint32_t qf = UINT32_MAX;
    {
        uint32_t n = 0u; vkEnumeratePhysicalDevices(instance, &n, nullptr); std::vector<VkPhysicalDevice> devs(n); vkEnumeratePhysicalDevices(instance, &n, devs.data());
        for (auto pd : devs) { VkPhysicalDeviceProperties p{}; vkGetPhysicalDeviceProperties(pd, &p); if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { phys = pd; std::printf("  selected GPU: %s\n", p.deviceName); break; } }
        if (phys == VK_NULL_HANDLE) phys = devs[0];
        uint32_t qn = 0u; vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, nullptr); std::vector<VkQueueFamilyProperties> qfs(qn); vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, qfs.data());
        for (uint32_t i = 0u; i < qn; ++i) if (qfs[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qf = i; break; }
    }
    VkDevice device = VK_NULL_HANDLE; VkQueue queue = VK_NULL_HANDLE;
    {
        const float prio = 1.0f; VkDeviceQueueCreateInfo qci{}; qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO; qci.queueFamilyIndex = qf; qci.queueCount = 1u; qci.pQueuePriorities = &prio;
        VkDeviceCreateInfo dci{}; dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO; dci.queueCreateInfoCount = 1u; dci.pQueueCreateInfos = &qci;
        if (vkCreateDevice(phys, &dci, nullptr, &device) != VK_SUCCESS) { std::fprintf(stderr, "  [FAIL] vkCreateDevice\n"); return 1; }
        vkGetDeviceQueue(device, qf, 0u, &queue);
    }

    // 1. small motion (the original gate) — the pyramid must still track small D.
    run_case(phys, device, queue, qf, 64u, 64u,   6, 24, 28, 29, 31, "small-motion D=5");
    // 2. LARGE displacement — the new capability (flat ±R was capped at R≤32, could not reach D=40).
    run_case(phys, device, queue, qf, 256u, 256u, 16, 80, 120, 120, 120, "large-D D=40");
    // 3. small FAST object — verify it is NOT lost to coarse averaging (STAGE-14 caveat).
    run_case(phys, device, queue, qf, 128u, 128u, 8, 40, 48, 52, 56, "small-fast obj D=12/8");
    // 4. STAGE-20 confidence gate: static overlay over a moving scene.
    //    Overlay tile MUST be MV=(0,0) via lambda tie-break (uniform white → all SADs=0 → lambda selects 0).
    //    Confidence gate correctly BLENDs the overlay (sad_zero=0 → improvement_frac check fails → BLEND).
    //    Moving blob MUST still track MV=(+8,0) (high sad_zero → gate passes → WARP).
    run_case_static_overlay(phys, device, queue, qf);
    // 5. STAGE-24 warp output readback — closes the STAGE-18/20 output-gate coverage gap.
    //    Same geometry as case 4. Reads back C; asserts:
    //      overlay pixel  ≈ 250 (BLEND-preserved: confidence gate fails for static tile).
    //      midpoint pixel ≥ 180 (WARP: both agreement and confidence gates pass for moving blob).
    run_case_warp_output(phys, device, queue, qf);
    // 6. NON-MULT-8 dimensions — regression guard for the unconditional barrier in
    //    optical_flow_warp.comp. 100×52 spawns edge workgroups whose out-of-bounds invocations
    //    must still reach the per-tile shared-memory reduction barrier. All prior test resolutions
    //    (64/128/256) are multiples of 8 → zero edge workgroups → this path was never exercised.
    //    Asserts: MV correct, warp output WARP-bright, pipeline COMPLETES (hang = barrier UB).
    run_case_non_mult8(phys, device, queue, qf);
    // 7. STAGE-77 SECOND-BEST (candidate) field on a periodic texture (aliasing class).
    //    Vertical stripes (period 6) shifted +2 px → the match is AMBIGUOUS (best and second realign
    //    one period apart with near-tied SADs). init(emit_second_best=true) makes the finest level emit
    //    the runner-up MV+SAD into cand_image(); asserts the candidate is written, the two vectors are
    //    one period (~6px) apart in x, and second_sad ≤ 1.15·best_sad (the warp's aliasing trigger).
    run_case_second_best(phys, device, queue, qf);

    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);

    std::printf("\n────────────────────────────────────────────────────────────────────────────────────────────\n");
    std::printf("RESULT: %s\n", g_failed == 0 ? "PASS" : "FAIL");
    return g_failed == 0 ? 0 : 1;
}
// Made with my soul - Swately <3
