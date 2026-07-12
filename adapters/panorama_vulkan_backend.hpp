#pragma once

// Optional, opt-in Vulkan adapter for PanoramaEngine.
//
// This is NOT part of the PanoramaEngine library and is deliberately not
// compiled into it: the engine core has no graphics-API dependency (see
// docs/architecture.md). This header is a self-contained, header-only
// implementation of the engine's PanoramaRenderBackend contract on top of
// Vulkan, provided as a drop-in starting point for hosts that render Panorama
// UI through Vulkan. A host that wants it #includes this file and links the
// Vulkan loader itself; a host that does not simply never includes it and pays
// nothing.
//
// Scope: "basic generic". It owns exactly what it needs to turn a
// PanoramaDrawList into Vulkan draw calls -- shader modules, a pipeline layout,
// one graphics pipeline per blend mode, a sampler, textures, and per-command
// vertex/index buffers -- and nothing about windowing, swapchain, or the frame
// loop, which the host already owns. The host injects its VkDevice / VkQueue /
// VkRenderPass once, then hands this backend the command buffer it is currently
// recording into each frame via new_frame(). Everything the geometry cache asks
// for after that records into that command buffer.
//
// Not implemented (safe no-ops inherited from PanoramaRenderBackend):
//   - blur_region(): backdrop gaussian blur is not implemented; panels that use
//     `blur:` render without the blur. A host that needs it can subclass and
//     override blur_region().
//
// Usage sketch (host owns the render pass / swapchain):
//
//   panorama_adapters::PanoramaVulkanInit init;
//   init.physical_device    = phys;
//   init.device             = device;
//   init.queue              = graphics_queue;
//   init.queue_family_index = graphics_family;
//   init.render_pass        = ui_render_pass;   // the pass the UI draws into
//   panorama_adapters::PanoramaVulkanBackend backend(init);
//   panorama::set_panorama_render_backend(&backend);
//   font_atlas.load(...);                        // uploads the glyph atlas now
//
//   // per frame, inside your render pass on `cmd`:
//   backend.new_frame(cmd, fb_width, fb_height);
//   geometry_cache.submit(draw_list, backend, ui_scale);   // or replay(backend)
//
// Threading: single-threaded, like the rest of the engine. All calls must come
// from the thread that owns `init.queue`.

#include "ui/panorama/panorama_render_backend.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace panorama_adapters
{
// Injected once at construction. Everything here is owned by the host and must
// outlive the backend; the backend borrows, never destroys, these handles.
struct PanoramaVulkanInit
{
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;         // graphics/transfer queue used for synchronous texture uploads
    std::uint32_t queue_family_index = 0;   // family `queue` belongs to
    VkRenderPass render_pass = VK_NULL_HANDLE; // the render pass UI is drawn into (pipelines are built compatible with it)
    std::uint32_t subpass = 0;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT; // must match the render pass' color attachment
    bool linear_filter = true;              // LINEAR for smoother text/images; false for nearest
    const VkAllocationCallbacks* allocator = nullptr;
};

class PanoramaVulkanBackend final : public panorama::PanoramaRenderBackend
{
public:
    explicit PanoramaVulkanBackend(const PanoramaVulkanInit& init) : init_(init)
    {
        if (init_.device == VK_NULL_HANDLE || init_.physical_device == VK_NULL_HANDLE ||
            init_.queue == VK_NULL_HANDLE || init_.render_pass == VK_NULL_HANDLE)
        {
            throw std::runtime_error("PanoramaVulkanBackend: incomplete PanoramaVulkanInit");
        }
        vkGetPhysicalDeviceMemoryProperties(init_.physical_device, &mem_props_);
        create_upload_pool();
        create_sampler();
        create_descriptor_set_layout();
        create_pipeline_layout();
        create_shader_modules();
        create_pipelines();
        create_white_texture();
    }

    ~PanoramaVulkanBackend() override
    {
        if (init_.device == VK_NULL_HANDLE)
        {
            return;
        }
        vkDeviceWaitIdle(init_.device);

        for (auto& [id, geometry] : geometries_)
        {
            destroy_buffer(geometry.vertex_buffer, geometry.vertex_memory);
            destroy_buffer(geometry.index_buffer, geometry.index_memory);
        }
        geometries_.clear();

        for (auto& [id, texture] : textures_)
        {
            destroy_texture(texture);
        }
        textures_.clear();
        destroy_texture(white_);

        for (VkPipeline pipeline : pipelines_)
        {
            if (pipeline != VK_NULL_HANDLE)
            {
                vkDestroyPipeline(init_.device, pipeline, init_.allocator);
            }
        }
        if (vertex_shader_ != VK_NULL_HANDLE)
        {
            vkDestroyShaderModule(init_.device, vertex_shader_, init_.allocator);
        }
        if (fragment_shader_ != VK_NULL_HANDLE)
        {
            vkDestroyShaderModule(init_.device, fragment_shader_, init_.allocator);
        }
        if (pipeline_layout_ != VK_NULL_HANDLE)
        {
            vkDestroyPipelineLayout(init_.device, pipeline_layout_, init_.allocator);
        }
        if (set_layout_ != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(init_.device, set_layout_, init_.allocator);
        }
        if (sampler_ != VK_NULL_HANDLE)
        {
            vkDestroySampler(init_.device, sampler_, init_.allocator);
        }
        for (VkDescriptorPool pool : descriptor_pools_)
        {
            vkDestroyDescriptorPool(init_.device, pool, init_.allocator);
        }
        if (upload_pool_ != VK_NULL_HANDLE)
        {
            vkDestroyCommandPool(init_.device, upload_pool_, init_.allocator);
        }
    }

    PanoramaVulkanBackend(const PanoramaVulkanBackend&) = delete;
    PanoramaVulkanBackend& operator=(const PanoramaVulkanBackend&) = delete;

    // Call once at the start of each frame, after you have begun the UI render
    // pass on `cmd`. `cmd` is the command buffer subsequent render_geometry
    // calls (via the geometry cache) record into; it must stay in the recording
    // state and inside the render pass until you stop issuing draws for this
    // frame. `fb_width`/`fb_height` are the render target size in pixels.
    void new_frame(VkCommandBuffer cmd, std::uint32_t fb_width, std::uint32_t fb_height)
    {
        current_cmd_ = cmd;
        framebuffer_extent_ = VkExtent2D{fb_width, fb_height};
        current_blend_ = panorama::PanoramaBlendMode::Normal;
        scissor_enabled_ = false;
        // Orthographic projection, framebuffer pixels -> Vulkan clip space
        // (y points down in NDC, so no vertical flip). Column-major for GLSL.
        const float w = fb_width > 0 ? static_cast<float>(fb_width) : 1.0F;
        const float h = fb_height > 0 ? static_cast<float>(fb_height) : 1.0F;
        projection_ = {
            2.0F / w, 0.0F, 0.0F, 0.0F,
            0.0F, 2.0F / h, 0.0F, 0.0F,
            0.0F, 0.0F, 1.0F, 0.0F,
            -1.0F, -1.0F, 0.0F, 1.0F,
        };
    }

    void wait_idle() { vkDeviceWaitIdle(init_.device); }

    // --- PanoramaRenderBackend ------------------------------------------------

    panorama::PanoramaTextureId generate_texture(std::span<const unsigned char> rgba, int width, int height) override
    {
        if (width <= 0 || height <= 0)
        {
            return 0;
        }
        Texture texture = create_texture(width, height);
        upload_texture(texture, rgba);
        const panorama::PanoramaTextureId id = next_texture_id_++;
        textures_.emplace(id, texture);
        return id;
    }

    bool update_texture(panorama::PanoramaTextureId texture, std::span<const unsigned char> rgba, int width, int height) override
    {
        const auto it = textures_.find(texture);
        if (it == textures_.end() || it->second.width != width || it->second.height != height)
        {
            return false;
        }
        // Uploads must not race a frame that may still be sampling this image;
        // the synchronous submit + queue wait inside upload_texture handles the
        // ordering, but the host must not have in-flight frames using it.
        vkDeviceWaitIdle(init_.device);
        upload_texture(it->second, rgba);
        return true;
    }

    void release_texture(panorama::PanoramaTextureId texture) override
    {
        const auto it = textures_.find(texture);
        if (it == textures_.end())
        {
            return;
        }
        vkDeviceWaitIdle(init_.device);
        destroy_texture(it->second);
        textures_.erase(it);
    }

    void set_scissor(bool enabled, int x, int y, int width, int height) override
    {
        scissor_enabled_ = enabled;
        scissor_x_ = x;
        scissor_y_ = y;
        scissor_width_ = width;
        scissor_height_ = height;
    }

    void set_blend_mode(panorama::PanoramaBlendMode mode) override { current_blend_ = mode; }

    panorama::PanoramaCompiledGeometryHandle compile_geometry(
        std::span<const panorama::PanoramaPaintVertex> vertices,
        std::span<const int> indices,
        float ui_scale) override
    {
        if (vertices.empty() || indices.empty())
        {
            return 0;
        }
        // Geometry is compiled in framebuffer pixels: scale the design-pixel
        // positions by ui_scale, matching the coordinate space new_frame()'s
        // projection expects. UVs and colors pass through unchanged.
        std::vector<panorama::PanoramaPaintVertex> scaled(vertices.begin(), vertices.end());
        for (panorama::PanoramaPaintVertex& vertex : scaled)
        {
            vertex.x *= ui_scale;
            vertex.y *= ui_scale;
        }
        std::vector<std::uint32_t> index_data(indices.begin(), indices.end());

        Geometry geometry;
        geometry.index_count = static_cast<std::uint32_t>(index_data.size());
        create_host_buffer(
            scaled.size() * sizeof(panorama::PanoramaPaintVertex),
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, scaled.data(), geometry.vertex_buffer, geometry.vertex_memory);
        create_host_buffer(
            index_data.size() * sizeof(std::uint32_t),
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT, index_data.data(), geometry.index_buffer, geometry.index_memory);

        const panorama::PanoramaCompiledGeometryHandle handle = next_geometry_id_++;
        geometries_.emplace(handle, geometry);
        return handle;
    }

    void render_geometry(panorama::PanoramaCompiledGeometryHandle geometry, panorama::PanoramaTextureId texture) override
    {
        if (current_cmd_ == VK_NULL_HANDLE)
        {
            return;
        }
        const auto it = geometries_.find(geometry);
        if (it == geometries_.end())
        {
            return;
        }
        const Geometry& mesh = it->second;

        VkDescriptorSet descriptor = white_.descriptor;
        if (texture != 0)
        {
            const auto texture_it = textures_.find(texture);
            if (texture_it != textures_.end())
            {
                descriptor = texture_it->second.descriptor;
            }
        }

        vkCmdBindPipeline(current_cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_for(current_blend_));

        const VkViewport viewport{
            0.0F, 0.0F,
            static_cast<float>(framebuffer_extent_.width), static_cast<float>(framebuffer_extent_.height),
            0.0F, 1.0F};
        vkCmdSetViewport(current_cmd_, 0, 1, &viewport);
        vkCmdSetScissor(current_cmd_, 0, 1, &current_scissor());

        vkCmdPushConstants(
            current_cmd_, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0,
            static_cast<std::uint32_t>(projection_.size() * sizeof(float)), projection_.data());
        vkCmdBindDescriptorSets(
            current_cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &descriptor, 0, nullptr);

        const VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(current_cmd_, 0, 1, &mesh.vertex_buffer, &offset);
        vkCmdBindIndexBuffer(current_cmd_, mesh.index_buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(current_cmd_, mesh.index_count, 1, 0, 0, 0);
    }

    void release_geometry(panorama::PanoramaCompiledGeometryHandle geometry) override
    {
        const auto it = geometries_.find(geometry);
        if (it == geometries_.end())
        {
            return;
        }
        // The geometry cache releases handles that fell out of the draw list;
        // those buffers may still be referenced by an in-flight frame.
        vkDeviceWaitIdle(init_.device);
        destroy_buffer(it->second.vertex_buffer, it->second.vertex_memory);
        destroy_buffer(it->second.index_buffer, it->second.index_memory);
        geometries_.erase(it);
    }

private:
    struct Texture
    {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSet descriptor = VK_NULL_HANDLE;
        VkDescriptorPool descriptor_pool = VK_NULL_HANDLE; // pool `descriptor` was allocated from, for vkFreeDescriptorSets
        int width = 0;
        int height = 0;
    };

    struct Geometry
    {
        VkBuffer vertex_buffer = VK_NULL_HANDLE;
        VkDeviceMemory vertex_memory = VK_NULL_HANDLE;
        VkBuffer index_buffer = VK_NULL_HANDLE;
        VkDeviceMemory index_memory = VK_NULL_HANDLE;
        std::uint32_t index_count = 0;
    };

    static void check(VkResult result, const char* what)
    {
        if (result != VK_SUCCESS)
        {
            throw std::runtime_error(std::string("PanoramaVulkanBackend: ") + what + " failed (VkResult " +
                                     std::to_string(static_cast<int>(result)) + ")");
        }
    }

    [[nodiscard]] std::uint32_t find_memory_type(std::uint32_t type_bits, VkMemoryPropertyFlags properties) const
    {
        for (std::uint32_t i = 0; i < mem_props_.memoryTypeCount; ++i)
        {
            if ((type_bits & (1U << i)) != 0 &&
                (mem_props_.memoryTypes[i].propertyFlags & properties) == properties)
            {
                return i;
            }
        }
        throw std::runtime_error("PanoramaVulkanBackend: no compatible memory type");
    }

    [[nodiscard]] std::size_t blend_index(panorama::PanoramaBlendMode mode) const
    {
        switch (mode)
        {
        case panorama::PanoramaBlendMode::Normal: return 0;
        case panorama::PanoramaBlendMode::Additive: return 1;
        case panorama::PanoramaBlendMode::Screen: return 2;
        case panorama::PanoramaBlendMode::Multiply: return 3;
        case panorama::PanoramaBlendMode::Opaque: return 4;
        }
        return 0;
    }

    [[nodiscard]] VkPipeline pipeline_for(panorama::PanoramaBlendMode mode) const { return pipelines_[blend_index(mode)]; }

    [[nodiscard]] const VkRect2D& current_scissor()
    {
        if (!scissor_enabled_)
        {
            scissor_cache_ = VkRect2D{{0, 0}, framebuffer_extent_};
            return scissor_cache_;
        }
        // Clamp the requested rect to the framebuffer; Vulkan rejects a scissor
        // that runs past the attachment.
        const std::int32_t fb_w = static_cast<std::int32_t>(framebuffer_extent_.width);
        const std::int32_t fb_h = static_cast<std::int32_t>(framebuffer_extent_.height);
        std::int32_t x0 = scissor_x_ < 0 ? 0 : scissor_x_;
        std::int32_t y0 = scissor_y_ < 0 ? 0 : scissor_y_;
        std::int32_t x1 = scissor_x_ + scissor_width_;
        std::int32_t y1 = scissor_y_ + scissor_height_;
        x0 = x0 > fb_w ? fb_w : x0;
        y0 = y0 > fb_h ? fb_h : y0;
        x1 = x1 < x0 ? x0 : (x1 > fb_w ? fb_w : x1);
        y1 = y1 < y0 ? y0 : (y1 > fb_h ? fb_h : y1);
        scissor_cache_ = VkRect2D{
            {x0, y0},
            {static_cast<std::uint32_t>(x1 - x0), static_cast<std::uint32_t>(y1 - y0)}};
        return scissor_cache_;
    }

    // --- one-time-submit upload helpers --------------------------------------

    [[nodiscard]] VkCommandBuffer begin_upload()
    {
        VkCommandBufferAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc.commandPool = upload_pool_;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        check(vkAllocateCommandBuffers(init_.device, &alloc, &cmd), "vkAllocateCommandBuffers(upload)");

        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer(upload)");
        return cmd;
    }

    void end_upload(VkCommandBuffer cmd)
    {
        check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer(upload)");

        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = VK_NULL_HANDLE;
        check(vkCreateFence(init_.device, &fence_info, init_.allocator, &fence), "vkCreateFence(upload)");

        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        check(vkQueueSubmit(init_.queue, 1, &submit, fence), "vkQueueSubmit(upload)");
        check(vkWaitForFences(init_.device, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences(upload)");

        vkDestroyFence(init_.device, fence, init_.allocator);
        vkFreeCommandBuffers(init_.device, upload_pool_, 1, &cmd);
    }

    // --- resource creation ----------------------------------------------------

    void create_upload_pool()
    {
        VkCommandPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        info.queueFamilyIndex = init_.queue_family_index;
        check(vkCreateCommandPool(init_.device, &info, init_.allocator, &upload_pool_), "vkCreateCommandPool");
    }

    void create_sampler()
    {
        const VkFilter filter = init_.linear_filter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        VkSamplerCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.magFilter = filter;
        info.minFilter = filter;
        info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.maxLod = VK_LOD_CLAMP_NONE;
        info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        check(vkCreateSampler(init_.device, &info, init_.allocator, &sampler_), "vkCreateSampler");
    }

    void create_descriptor_set_layout()
    {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        binding.pImmutableSamplers = &sampler_; // one shared sampler for every texture

        VkDescriptorSetLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = 1;
        info.pBindings = &binding;
        check(vkCreateDescriptorSetLayout(init_.device, &info, init_.allocator, &set_layout_),
              "vkCreateDescriptorSetLayout");
    }

    void create_pipeline_layout()
    {
        VkPushConstantRange range{};
        range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        range.offset = 0;
        range.size = 16 * sizeof(float); // mat4 projection

        VkPipelineLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        info.setLayoutCount = 1;
        info.pSetLayouts = &set_layout_;
        info.pushConstantRangeCount = 1;
        info.pPushConstantRanges = &range;
        check(vkCreatePipelineLayout(init_.device, &info, init_.allocator, &pipeline_layout_),
              "vkCreatePipelineLayout");
    }

    [[nodiscard]] VkShaderModule create_shader_module(std::span<const std::uint32_t> code) const
    {
        VkShaderModuleCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = code.size() * sizeof(std::uint32_t);
        info.pCode = code.data();
        VkShaderModule module = VK_NULL_HANDLE;
        check(vkCreateShaderModule(init_.device, &info, init_.allocator, &module), "vkCreateShaderModule");
        return module;
    }

    void create_shader_modules()
    {
        vertex_shader_ = create_shader_module(vertex_spirv());
        fragment_shader_ = create_shader_module(fragment_spirv());
    }

    [[nodiscard]] static VkPipelineColorBlendAttachmentState blend_state(panorama::PanoramaBlendMode mode)
    {
        VkPipelineColorBlendAttachmentState state{};
        state.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                               VK_COLOR_COMPONENT_A_BIT;
        state.blendEnable = VK_TRUE;
        state.colorBlendOp = VK_BLEND_OP_ADD;
        state.alphaBlendOp = VK_BLEND_OP_ADD;
        // Straight (non-premultiplied) alpha, matching PanoramaDrawList colors.
        switch (mode)
        {
        case panorama::PanoramaBlendMode::Normal:
            state.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            break;
        case panorama::PanoramaBlendMode::Additive:
            state.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            break;
        case panorama::PanoramaBlendMode::Screen:
            state.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
            state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            break;
        case panorama::PanoramaBlendMode::Multiply:
            state.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
            state.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
            state.srcAlphaBlendFactor = VK_BLEND_FACTOR_DST_ALPHA;
            state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            break;
        case panorama::PanoramaBlendMode::Opaque:
            state.blendEnable = VK_FALSE;
            break;
        }
        return state;
    }

    void create_pipelines()
    {
        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertex_shader_;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragment_shader_;
        stages[1].pName = "main";

        VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = sizeof(panorama::PanoramaPaintVertex);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        std::array<VkVertexInputAttributeDescription, 3> attributes{};
        attributes[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(panorama::PanoramaPaintVertex, x)};
        attributes[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(panorama::PanoramaPaintVertex, u)};
        attributes[2] = {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(panorama::PanoramaPaintVertex, color)};

        VkPipelineVertexInputStateCreateInfo vertex_input{};
        vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertex_input.vertexBindingDescriptionCount = 1;
        vertex_input.pVertexBindingDescriptions = &binding;
        vertex_input.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
        vertex_input.pVertexAttributeDescriptions = attributes.data();

        VkPipelineInputAssemblyStateCreateInfo input_assembly{};
        input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewport_state{};
        viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport_state.viewportCount = 1;
        viewport_state.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo raster{};
        raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0F;

        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = init_.samples;

        VkPipelineDepthStencilStateCreateInfo depth_stencil{};
        depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth_stencil.depthTestEnable = VK_FALSE;
        depth_stencil.depthWriteEnable = VK_FALSE;

        const std::array<VkDynamicState, 2> dynamic_states{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{};
        dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size());
        dynamic.pDynamicStates = dynamic_states.data();

        constexpr std::array<panorama::PanoramaBlendMode, 5> modes{
            panorama::PanoramaBlendMode::Normal, panorama::PanoramaBlendMode::Additive,
            panorama::PanoramaBlendMode::Screen, panorama::PanoramaBlendMode::Multiply,
            panorama::PanoramaBlendMode::Opaque};
        for (const panorama::PanoramaBlendMode mode : modes)
        {
            const VkPipelineColorBlendAttachmentState attachment = blend_state(mode);
            VkPipelineColorBlendStateCreateInfo color_blend{};
            color_blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            color_blend.attachmentCount = 1;
            color_blend.pAttachments = &attachment;

            VkGraphicsPipelineCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            info.stageCount = static_cast<std::uint32_t>(stages.size());
            info.pStages = stages.data();
            info.pVertexInputState = &vertex_input;
            info.pInputAssemblyState = &input_assembly;
            info.pViewportState = &viewport_state;
            info.pRasterizationState = &raster;
            info.pMultisampleState = &multisample;
            info.pDepthStencilState = &depth_stencil;
            info.pColorBlendState = &color_blend;
            info.pDynamicState = &dynamic;
            info.layout = pipeline_layout_;
            info.renderPass = init_.render_pass;
            info.subpass = init_.subpass;
            check(vkCreateGraphicsPipelines(
                      init_.device, VK_NULL_HANDLE, 1, &info, init_.allocator, &pipelines_[blend_index(mode)]),
                  "vkCreateGraphicsPipelines");
        }
    }

    // Matches the enum-to-index mapping in blend_index(): Normal, Additive,
    // Screen, Multiply, Opaque.
    static_assert(static_cast<int>(panorama::PanoramaBlendMode::Normal) == 0);

    void create_white_texture()
    {
        white_ = create_texture(1, 1);
        const std::array<unsigned char, 4> pixel{255, 255, 255, 255};
        upload_texture(white_, pixel);
    }

    [[nodiscard]] Texture create_texture(int width, int height)
    {
        Texture texture;
        texture.width = width;
        texture.height = height;

        VkImageCreateInfo image_info{};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        image_info.extent = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), 1};
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        check(vkCreateImage(init_.device, &image_info, init_.allocator, &texture.image), "vkCreateImage");

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(init_.device, texture.image, &requirements);
        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = requirements.size;
        alloc.memoryTypeIndex = find_memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(init_.device, &alloc, init_.allocator, &texture.memory), "vkAllocateMemory(image)");
        check(vkBindImageMemory(init_.device, texture.image, texture.memory, 0), "vkBindImageMemory");

        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = texture.image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        check(vkCreateImageView(init_.device, &view_info, init_.allocator, &texture.view), "vkCreateImageView");

        texture.descriptor = allocate_descriptor_set(texture.descriptor_pool);
        VkDescriptorImageInfo descriptor_image{};
        descriptor_image.sampler = sampler_; // ignored (immutable in layout) but harmless to set
        descriptor_image.imageView = texture.view;
        descriptor_image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = texture.descriptor;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &descriptor_image;
        vkUpdateDescriptorSets(init_.device, 1, &write, 0, nullptr);

        return texture;
    }

    void upload_texture(const Texture& texture, std::span<const unsigned char> rgba)
    {
        const VkDeviceSize size =
            static_cast<VkDeviceSize>(texture.width) * static_cast<VkDeviceSize>(texture.height) * 4;

        VkBuffer staging = VK_NULL_HANDLE;
        VkDeviceMemory staging_memory = VK_NULL_HANDLE;
        create_buffer(
            size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging, staging_memory);

        void* mapped = nullptr;
        check(vkMapMemory(init_.device, staging_memory, 0, size, 0, &mapped), "vkMapMemory(staging)");
        const VkDeviceSize copy_size = rgba.size() < size ? static_cast<VkDeviceSize>(rgba.size()) : size;
        std::memcpy(mapped, rgba.data(), static_cast<std::size_t>(copy_size));
        vkUnmapMemory(init_.device, staging_memory);

        VkCommandBuffer cmd = begin_upload();

        transition_image(cmd, texture.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {static_cast<std::uint32_t>(texture.width), static_cast<std::uint32_t>(texture.height), 1};
        vkCmdCopyBufferToImage(cmd, staging, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        transition_image(cmd, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        end_upload(cmd);

        destroy_buffer(staging, staging_memory);
    }

    static void transition_image(VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout, VkImageLayout new_layout)
    {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = old_layout;
        barrier.newLayout = new_layout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        VkPipelineStageFlags src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkPipelineStageFlags dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED && new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
        {
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        }
        else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
                 new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        {
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            dst_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        }
        vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    // --- descriptor pools -----------------------------------------------------

    [[nodiscard]] VkDescriptorSet allocate_descriptor_set(VkDescriptorPool& out_pool)
    {
        if (descriptor_pools_.empty())
        {
            descriptor_pools_.push_back(create_descriptor_pool());
        }
        VkDescriptorSetAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool = descriptor_pools_.back();
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &set_layout_;

        VkDescriptorSet set = VK_NULL_HANDLE;
        VkResult result = vkAllocateDescriptorSets(init_.device, &alloc, &set);
        if (result == VK_ERROR_OUT_OF_POOL_MEMORY || result == VK_ERROR_FRAGMENTED_POOL)
        {
            // Current pool is full; grow and retry once.
            descriptor_pools_.push_back(create_descriptor_pool());
            alloc.descriptorPool = descriptor_pools_.back();
            result = vkAllocateDescriptorSets(init_.device, &alloc, &set);
        }
        check(result, "vkAllocateDescriptorSets");
        out_pool = alloc.descriptorPool;
        return set;
    }

    [[nodiscard]] VkDescriptorPool create_descriptor_pool()
    {
        constexpr std::uint32_t kPoolCapacity = 256;
        VkDescriptorPoolSize size{};
        size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        size.descriptorCount = kPoolCapacity;

        VkDescriptorPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        info.maxSets = kPoolCapacity;
        info.poolSizeCount = 1;
        info.pPoolSizes = &size;

        VkDescriptorPool pool = VK_NULL_HANDLE;
        check(vkCreateDescriptorPool(init_.device, &info, init_.allocator, &pool), "vkCreateDescriptorPool");
        return pool;
    }

    // --- buffers --------------------------------------------------------------

    void create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
                       VkBuffer& buffer, VkDeviceMemory& memory)
    {
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = size;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateBuffer(init_.device, &info, init_.allocator, &buffer), "vkCreateBuffer");

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(init_.device, buffer, &requirements);
        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = requirements.size;
        alloc.memoryTypeIndex = find_memory_type(requirements.memoryTypeBits, properties);
        check(vkAllocateMemory(init_.device, &alloc, init_.allocator, &memory), "vkAllocateMemory(buffer)");
        check(vkBindBufferMemory(init_.device, buffer, memory, 0), "vkBindBufferMemory");
    }

    void create_host_buffer(std::size_t size, VkBufferUsageFlags usage, const void* data, VkBuffer& buffer,
                           VkDeviceMemory& memory)
    {
        create_buffer(
            size, usage, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, buffer, memory);
        void* mapped = nullptr;
        check(vkMapMemory(init_.device, memory, 0, size, 0, &mapped), "vkMapMemory(host buffer)");
        std::memcpy(mapped, data, size);
        vkUnmapMemory(init_.device, memory);
    }

    void destroy_buffer(VkBuffer buffer, VkDeviceMemory memory)
    {
        if (buffer != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(init_.device, buffer, init_.allocator);
        }
        if (memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(init_.device, memory, init_.allocator);
        }
    }

    void destroy_texture(Texture& texture)
    {
        if (texture.descriptor != VK_NULL_HANDLE && texture.descriptor_pool != VK_NULL_HANDLE)
        {
            // Pools are created with FREE_DESCRIPTOR_SET_BIT, so the set can be
            // returned individually to the pool it came from (tracked per texture).
            vkFreeDescriptorSets(init_.device, texture.descriptor_pool, 1, &texture.descriptor);
        }
        if (texture.view != VK_NULL_HANDLE)
        {
            vkDestroyImageView(init_.device, texture.view, init_.allocator);
            texture.view = VK_NULL_HANDLE;
        }
        if (texture.image != VK_NULL_HANDLE)
        {
            vkDestroyImage(init_.device, texture.image, init_.allocator);
            texture.image = VK_NULL_HANDLE;
        }
        if (texture.memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(init_.device, texture.memory, init_.allocator);
            texture.memory = VK_NULL_HANDLE;
        }
        texture.descriptor = VK_NULL_HANDLE;
        texture.descriptor_pool = VK_NULL_HANDLE;
    }

    // --- embedded SPIR-V (see adapters/shaders/, compiled with glslc) ---------

    [[nodiscard]] static std::span<const std::uint32_t> vertex_spirv()
    {
        static const std::uint32_t code[] = {
#include "shaders/panorama_ui.vert.spv.inl"
        };
        return {code, std::size(code)};
    }

    [[nodiscard]] static std::span<const std::uint32_t> fragment_spirv()
    {
        static const std::uint32_t code[] = {
#include "shaders/panorama_ui.frag.spv.inl"
        };
        return {code, std::size(code)};
    }

    PanoramaVulkanInit init_;
    VkPhysicalDeviceMemoryProperties mem_props_{};

    VkCommandPool upload_pool_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkShaderModule vertex_shader_ = VK_NULL_HANDLE;
    VkShaderModule fragment_shader_ = VK_NULL_HANDLE;
    std::array<VkPipeline, 5> pipelines_{};
    std::vector<VkDescriptorPool> descriptor_pools_;

    std::unordered_map<panorama::PanoramaTextureId, Texture> textures_;
    std::unordered_map<panorama::PanoramaCompiledGeometryHandle, Geometry> geometries_;
    Texture white_;
    panorama::PanoramaTextureId next_texture_id_ = 1;
    panorama::PanoramaCompiledGeometryHandle next_geometry_id_ = 1;

    // Per-frame state set by new_frame().
    VkCommandBuffer current_cmd_ = VK_NULL_HANDLE;
    VkExtent2D framebuffer_extent_{0, 0};
    std::array<float, 16> projection_{};
    panorama::PanoramaBlendMode current_blend_ = panorama::PanoramaBlendMode::Normal;
    bool scissor_enabled_ = false;
    int scissor_x_ = 0;
    int scissor_y_ = 0;
    int scissor_width_ = 0;
    int scissor_height_ = 0;
    VkRect2D scissor_cache_{};
};
}
