// framework/render/vulkan/src/BlendPipeline.cpp
// Implementation of BlendPipeline — see header for the contract.
//
#include <phyriad/render/vulkan/BlendPipeline.hpp>

// The SPV bytecode is generated at build time by the
// phyriad_compile_spv_to_header CMake rule from shaders/frame_blend_linear.comp.
#include "frame_blend_linear_spv.hpp"

#include <cstdio>
#include <cstring>

namespace phyriad::render::vulkan {

// ─────────────────────────────────────────────────────────────────────────────
// Helpers — keep them static (TU-local) so they don't pollute the link surface.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

void log_vk_err(const char* op, VkResult r) noexcept {
    std::fprintf(stderr, "[BlendPipeline] %s -> VkResult=%d\n", op,
                 static_cast<int>(r));
}

[[nodiscard]] VkResult create_shader_module(VkDevice         device,
                                            const uint32_t*  spv_words,
                                            std::size_t      word_count,
                                            VkShaderModule&  out_mod) noexcept
{
    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = word_count * sizeof(uint32_t);
    ci.pCode    = spv_words;
    return vkCreateShaderModule(device, &ci, nullptr, &out_mod);
}

} // anonymous

// ─────────────────────────────────────────────────────────────────────────────
// init
// ─────────────────────────────────────────────────────────────────────────────
bool BlendPipeline::init(VkDevice         device,
                         VkPhysicalDevice /*phys_dev*/,
                         uint32_t         max_in_flight) noexcept
{
    if (initialized()) {
        std::fprintf(stderr, "[BlendPipeline] already initialized\n");
        return true;
    }
    if (device == VK_NULL_HANDLE) {
        std::fprintf(stderr, "[BlendPipeline] init: device is VK_NULL_HANDLE\n");
        return false;
    }
    if (max_in_flight == 0u || max_in_flight > kMaxInFlight) {
        std::fprintf(stderr,
            "[BlendPipeline] init: max_in_flight=%u out of range (1..%u)\n",
            max_in_flight, kMaxInFlight);
        return false;
    }

    device_ = device;
    n_sets_ = max_in_flight;

    // ── 1. Sampler (linear filter for slight resilience against tiny size
    //       mismatches; clamp-to-edge so the shader's UV out-of-range
    //       guard isn't strictly required at the borders).
    {
        VkSamplerCreateInfo s{};
        s.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        s.magFilter    = VK_FILTER_LINEAR;
        s.minFilter    = VK_FILTER_LINEAR;
        s.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        s.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        s.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        s.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        s.maxLod       = 0.0f;
        s.unnormalizedCoordinates = VK_FALSE;
        const VkResult r = vkCreateSampler(device, &s, nullptr, &sampler_);
        if (r != VK_SUCCESS) { log_vk_err("vkCreateSampler", r); shutdown(device); return false; }
    }

    // ── 2. Descriptor set layout: 3 bindings.
    //   0: combined image sampler — input A
    //   1: combined image sampler — input B
    //   2: storage image (rgba8)  — output C
    {
        const VkSampler immutable_samplers[] = { sampler_, sampler_ };
        const VkDescriptorSetLayoutBinding bindings[3] = {
            { 0u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
              VK_SHADER_STAGE_COMPUTE_BIT, &immutable_samplers[0] },
            { 1u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
              VK_SHADER_STAGE_COMPUTE_BIT, &immutable_samplers[1] },
            { 2u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          1u,
              VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
        };
        VkDescriptorSetLayoutCreateInfo ci{};
        ci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ci.bindingCount = 3u;
        ci.pBindings    = bindings;
        const VkResult r = vkCreateDescriptorSetLayout(device, &ci, nullptr, &dsl_);
        if (r != VK_SUCCESS) {
            log_vk_err("vkCreateDescriptorSetLayout", r);
            shutdown(device);
            return false;
        }
    }

    // ── 3. Pipeline layout (no push constants — the image extent is
    //       discovered at runtime via GLSL's imageSize()).
    {
        VkPipelineLayoutCreateInfo ci{};
        ci.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        ci.setLayoutCount = 1u;
        ci.pSetLayouts    = &dsl_;
        const VkResult r = vkCreatePipelineLayout(device, &ci, nullptr, &pipeline_layout_);
        if (r != VK_SUCCESS) {
            log_vk_err("vkCreatePipelineLayout", r);
            shutdown(device);
            return false;
        }
    }

    // ── 4. Shader module + compute pipeline.
    VkShaderModule shader_mod = VK_NULL_HANDLE;
    {
        const VkResult r = create_shader_module(
            device, kFrameBlendLinearSpv.data(), kFrameBlendLinearSpv.size(),
            shader_mod);
        if (r != VK_SUCCESS) {
            log_vk_err("vkCreateShaderModule(blend_linear)", r);
            shutdown(device);
            return false;
        }

        VkPipelineShaderStageCreateInfo stage{};
        stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = shader_mod;
        stage.pName  = "main";

        VkComputePipelineCreateInfo ci{};
        ci.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        ci.stage  = stage;
        ci.layout = pipeline_layout_;

        const VkResult cr = vkCreateComputePipelines(
            device, VK_NULL_HANDLE, 1u, &ci, nullptr, &pipeline_);

        // Shader module is no longer needed once the pipeline references it;
        // destroy it regardless of success.
        vkDestroyShaderModule(device, shader_mod, nullptr);

        if (cr != VK_SUCCESS) {
            log_vk_err("vkCreateComputePipelines(blend_linear)", cr);
            shutdown(device);
            return false;
        }
    }

    // ── 5. Descriptor pool sized for max_in_flight × (2 samplers + 1 storage).
    {
        const VkDescriptorPoolSize sizes[2] = {
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2u * max_in_flight },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          1u * max_in_flight },
        };
        VkDescriptorPoolCreateInfo ci{};
        ci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        ci.maxSets       = max_in_flight;
        ci.poolSizeCount = 2u;
        ci.pPoolSizes    = sizes;
        // No FREE_DESCRIPTOR_SET_BIT — we never call vkFreeDescriptorSets;
        // shutdown destroys the whole pool which transitively frees the sets.
        const VkResult r = vkCreateDescriptorPool(device, &ci, nullptr, &desc_pool_);
        if (r != VK_SUCCESS) {
            log_vk_err("vkCreateDescriptorPool", r);
            shutdown(device);
            return false;
        }
    }

    // ── 6. Pre-allocate the descriptor-set ring.
    {
        VkDescriptorSetLayout layouts[kMaxInFlight];
        for (uint32_t i = 0u; i < max_in_flight; ++i) layouts[i] = dsl_;

        VkDescriptorSetAllocateInfo ai{};
        ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool     = desc_pool_;
        ai.descriptorSetCount = max_in_flight;
        ai.pSetLayouts        = layouts;

        const VkResult r = vkAllocateDescriptorSets(device, &ai, sets_);
        if (r != VK_SUCCESS) {
            log_vk_err("vkAllocateDescriptorSets", r);
            shutdown(device);
            return false;
        }
    }

    next_set_ = 0u;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// shutdown
// ─────────────────────────────────────────────────────────────────────────────
void BlendPipeline::shutdown(VkDevice device) noexcept {
    if (device == VK_NULL_HANDLE) device = device_;
    if (device == VK_NULL_HANDLE) {
        // Never initialised — nothing to release.
        return;
    }

    // Reverse-order teardown. Descriptor-set ring is freed implicitly
    // when its pool is destroyed.
    if (desc_pool_       != VK_NULL_HANDLE) { vkDestroyDescriptorPool(device, desc_pool_, nullptr);       desc_pool_ = VK_NULL_HANDLE; }
    if (pipeline_        != VK_NULL_HANDLE) { vkDestroyPipeline(device, pipeline_, nullptr);              pipeline_ = VK_NULL_HANDLE; }
    if (pipeline_layout_ != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device, pipeline_layout_, nullptr); pipeline_layout_ = VK_NULL_HANDLE; }
    if (dsl_             != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(device, dsl_, nullptr);        dsl_ = VK_NULL_HANDLE; }
    if (sampler_         != VK_NULL_HANDLE) { vkDestroySampler(device, sampler_, nullptr);                sampler_ = VK_NULL_HANDLE; }

    for (auto& s : sets_) s = VK_NULL_HANDLE;
    n_sets_   = 0u;
    next_set_ = 0u;
    device_   = VK_NULL_HANDLE;
}

// ─────────────────────────────────────────────────────────────────────────────
// record_blend
// ─────────────────────────────────────────────────────────────────────────────
bool BlendPipeline::record_blend(VkCommandBuffer cmd,
                                 VkImageView     a_view,
                                 VkImageView     b_view,
                                 VkImageView     c_view,
                                 uint32_t        width,
                                 uint32_t        height) noexcept
{
    if (!initialized() || cmd == VK_NULL_HANDLE ||
        a_view == VK_NULL_HANDLE || b_view == VK_NULL_HANDLE ||
        c_view == VK_NULL_HANDLE)
    {
        return false;
    }
    if (n_sets_ == 0u) return false;

    // Pick the next descriptor set from the ring.
    const uint32_t set_idx = next_set_;
    next_set_ = (next_set_ + 1u) % n_sets_;
    VkDescriptorSet set = sets_[set_idx];

    // Image-info descriptors. The sampler is baked in as an immutable
    // binding sampler at DSL-creation time, so we just need the view.
    VkDescriptorImageInfo a_info{};
    a_info.imageView   = a_view;
    a_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorImageInfo b_info{};
    b_info.imageView   = b_view;
    b_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorImageInfo c_info{};
    c_info.imageView   = c_view;
    c_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    const VkWriteDescriptorSet writes[3] = {
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0u, 0u, 1u,
          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &a_info, nullptr, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 1u, 0u, 1u,
          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &b_info, nullptr, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 2u, 0u, 1u,
          VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &c_info, nullptr, nullptr },
    };
    vkUpdateDescriptorSets(device_, 3u, writes, 0u, nullptr);

    // Record the dispatch.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline_layout_, 0u, 1u, &set, 0u, nullptr);

    // Workgroup size = 8×8; round up so partial tiles still get covered.
    const uint32_t gx = (width  + 7u) / 8u;
    const uint32_t gy = (height + 7u) / 8u;
    vkCmdDispatch(cmd, gx, gy, 1u);
    return true;
}

} // namespace phyriad::render::vulkan
// Made with my soul - Swately <3
