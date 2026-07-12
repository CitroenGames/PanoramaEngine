#pragma once

// Optional, opt-in Direct3D 12 adapter for PanoramaEngine.
//
// This is NOT part of the PanoramaEngine library and is deliberately not
// compiled into it: the engine core has no graphics-API dependency (see
// docs/architecture.md). This header is a self-contained, header-only
// implementation of the engine's PanoramaRenderBackend contract on top of
// Direct3D 12, provided as a drop-in starting point for hosts that render
// Panorama UI through D3D12. A host that wants it #includes this file (Windows
// only); a host that does not simply never includes it and pays nothing.
//
// Scope: "basic generic". It owns exactly what it needs to turn a
// PanoramaDrawList into D3D12 draw calls -- a root signature, one pipeline
// state per blend mode, an SRV descriptor heap, textures, and per-command
// vertex/index buffers -- and nothing about windowing, swapchain, or the frame
// loop, which the host already owns. The host injects its ID3D12Device and the
// ID3D12CommandQueue it presents on once, then hands this backend the command
// list it is currently recording into each frame via new_frame(). Everything
// the geometry cache asks for after that records into that command list.
//
// The pixel/vertex shaders are compiled from embedded HLSL at construction with
// D3DCompile (d3dcompiler), so no offline shader build step is needed. Links
// d3d12.lib and d3dcompiler.lib (declared below via #pragma comment).
//
// Not implemented (safe no-ops inherited from PanoramaRenderBackend):
//   - blur_region(): backdrop gaussian blur is not implemented; panels that use
//     `blur:` render without the blur. A host that needs it can subclass and
//     override blur_region().
//
// Usage sketch (host owns the swapchain / render target):
//
//   panorama_adapters::PanoramaD3D12Init init;
//   init.device     = device;
//   init.queue      = present_queue;              // the queue frames are submitted on
//   init.rtv_format = DXGI_FORMAT_R8G8B8A8_UNORM; // must match your render target
//   panorama_adapters::PanoramaD3D12Backend backend(init);
//   panorama::set_panorama_render_backend(&backend);
//   font_atlas.load(...);                         // uploads the glyph atlas now
//
//   // per frame, after OMSetRenderTargets on `cmd`:
//   backend.new_frame(cmd, fb_width, fb_height);
//   geometry_cache.submit(draw_list, backend, ui_scale);   // or replay(backend)
//   // ... then the host closes/executes/presents `cmd` as usual.
//
// Lifetime: geometry/textures the engine releases mid-frame may still be
// referenced by the previously submitted frame, so this backend defers their
// destruction until an internal fence (signaled on `queue` each new_frame())
// proves that frame's GPU work has completed. The host therefore must call
// new_frame() once per frame for that reclamation to make progress.
//
// Threading: single-threaded, like the rest of the engine.

#include "ui/panorama/panorama_render_backend.hpp"

#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace panorama_adapters
{
// Injected once at construction. `device` and `queue` are owned by the host and
// must outlive the backend; it borrows, never destroys, them.
struct PanoramaD3D12Init
{
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;                  // the queue frames are submitted on (also used for uploads)
    DXGI_FORMAT rtv_format = DXGI_FORMAT_R8G8B8A8_UNORM;  // must match the render target the UI draws into
    DXGI_SAMPLE_DESC sample_desc = {1, 0};                // must match the render target's MSAA
    bool linear_filter = true;                            // LINEAR for smoother text/images; false for point
    std::uint32_t max_textures = 1024;                    // SRV heap capacity (textures alive at once)
};

class PanoramaD3D12Backend final : public panorama::PanoramaRenderBackend
{
public:
    explicit PanoramaD3D12Backend(const PanoramaD3D12Init& init) : init_(init)
    {
        if (init_.device == nullptr || init_.queue == nullptr)
        {
            throw std::runtime_error("PanoramaD3D12Backend: PanoramaD3D12Init.device/queue must be set");
        }
        create_fence();
        create_upload_objects();
        create_root_signature();
        create_pipelines();
        create_srv_heap();
        create_white_texture();
    }

    ~PanoramaD3D12Backend() override
    {
        flush(); // wait for the GPU so nothing below is still in use
        reclaim_trash(fence_->GetCompletedValue());
        if (fence_event_ != nullptr)
        {
            CloseHandle(fence_event_);
        }
    }

    PanoramaD3D12Backend(const PanoramaD3D12Backend&) = delete;
    PanoramaD3D12Backend& operator=(const PanoramaD3D12Backend&) = delete;

    // Call once at the start of each frame, after you have bound the render
    // target on `cmd` (OMSetRenderTargets). `cmd` is the command list the
    // geometry cache's render_geometry calls record into; it must stay open
    // until you finish issuing UI draws. `fb_width`/`fb_height` are the render
    // target size in pixels. Signals the reclaim fence so deferred-freed GPU
    // resources from earlier frames can be released once they are safe.
    void new_frame(ID3D12GraphicsCommandList* cmd, std::uint32_t fb_width, std::uint32_t fb_height)
    {
        current_cmd_ = cmd;
        framebuffer_width_ = fb_width;
        framebuffer_height_ = fb_height;
        current_blend_ = panorama::PanoramaBlendMode::Normal;
        scissor_enabled_ = false;

        // Orthographic projection, framebuffer pixels -> D3D clip space (NDC y
        // points up, so the top row maps to +1). Row-major to match the
        // `row_major float4x4` in the embedded HLSL.
        const float w = fb_width > 0 ? static_cast<float>(fb_width) : 1.0F;
        const float h = fb_height > 0 ? static_cast<float>(fb_height) : 1.0F;
        projection_ = {
            2.0F / w, 0.0F, 0.0F, -1.0F,
            0.0F, -2.0F / h, 0.0F, 1.0F,
            0.0F, 0.0F, 1.0F, 0.0F,
            0.0F, 0.0F, 0.0F, 1.0F,
        };

        // Signal on the host queue: completion of this value means every frame
        // submitted before now (whose command lists were already executed) is
        // done, so their retired resources can be freed.
        ++fence_value_;
        check_hr(init_.queue->Signal(fence_.Get(), fence_value_), "queue Signal");
        reclaim_trash(fence_->GetCompletedValue());
    }

    // Blocks until all work submitted on the host queue so far has completed.
    void flush()
    {
        ++fence_value_;
        if (FAILED(init_.queue->Signal(fence_.Get(), fence_value_)))
        {
            return;
        }
        wait_for_fence(fence_value_);
    }

    // --- PanoramaRenderBackend ------------------------------------------------

    panorama::PanoramaTextureId generate_texture(std::span<const unsigned char> rgba, int width, int height) override
    {
        if (width <= 0 || height <= 0)
        {
            return 0;
        }
        Texture texture = create_texture(width, height);
        upload_texture(texture, rgba, /*already_shader_readable=*/false);
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
        // The synchronous upload executes on the host queue after any frame
        // already submitted, so it will not race in-flight sampling of this
        // texture.
        upload_texture(it->second, rgba, /*already_shader_readable=*/true);
        return true;
    }

    void release_texture(panorama::PanoramaTextureId texture) override
    {
        const auto it = textures_.find(texture);
        if (it == textures_.end())
        {
            return;
        }
        retire_texture(it->second);
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
        // positions by ui_scale to match new_frame()'s projection.
        std::vector<panorama::PanoramaPaintVertex> scaled(vertices.begin(), vertices.end());
        for (panorama::PanoramaPaintVertex& vertex : scaled)
        {
            vertex.x *= ui_scale;
            vertex.y *= ui_scale;
        }
        std::vector<std::uint32_t> index_data(indices.begin(), indices.end());

        Geometry geometry;
        geometry.index_count = static_cast<UINT>(index_data.size());
        const UINT64 vertex_bytes = scaled.size() * sizeof(panorama::PanoramaPaintVertex);
        const UINT64 index_bytes = index_data.size() * sizeof(std::uint32_t);

        geometry.vertex_buffer = create_upload_buffer(vertex_bytes, scaled.data());
        geometry.index_buffer = create_upload_buffer(index_bytes, index_data.data());

        geometry.vertex_view.BufferLocation = geometry.vertex_buffer->GetGPUVirtualAddress();
        geometry.vertex_view.SizeInBytes = static_cast<UINT>(vertex_bytes);
        geometry.vertex_view.StrideInBytes = sizeof(panorama::PanoramaPaintVertex);

        geometry.index_view.BufferLocation = geometry.index_buffer->GetGPUVirtualAddress();
        geometry.index_view.SizeInBytes = static_cast<UINT>(index_bytes);
        geometry.index_view.Format = DXGI_FORMAT_R32_UINT;

        const panorama::PanoramaCompiledGeometryHandle handle = next_geometry_id_++;
        geometries_.emplace(handle, std::move(geometry));
        return handle;
    }

    void render_geometry(panorama::PanoramaCompiledGeometryHandle geometry, panorama::PanoramaTextureId texture) override
    {
        if (current_cmd_ == nullptr)
        {
            return;
        }
        const auto it = geometries_.find(geometry);
        if (it == geometries_.end())
        {
            return;
        }
        const Geometry& mesh = it->second;

        D3D12_GPU_DESCRIPTOR_HANDLE srv = white_.gpu_handle;
        if (texture != 0)
        {
            const auto texture_it = textures_.find(texture);
            if (texture_it != textures_.end())
            {
                srv = texture_it->second.gpu_handle;
            }
        }

        ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get()};
        current_cmd_->SetDescriptorHeaps(1, heaps);
        current_cmd_->SetGraphicsRootSignature(root_signature_.Get());
        current_cmd_->SetPipelineState(pipeline_for(current_blend_));
        current_cmd_->SetGraphicsRoot32BitConstants(0, 16, projection_.data(), 0);
        current_cmd_->SetGraphicsRootDescriptorTable(1, srv);
        current_cmd_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        const D3D12_VIEWPORT viewport{
            0.0F, 0.0F,
            static_cast<float>(framebuffer_width_), static_cast<float>(framebuffer_height_),
            0.0F, 1.0F};
        current_cmd_->RSSetViewports(1, &viewport);
        const D3D12_RECT scissor = current_scissor();
        current_cmd_->RSSetScissorRects(1, &scissor);

        current_cmd_->IASetVertexBuffers(0, 1, &mesh.vertex_view);
        current_cmd_->IASetIndexBuffer(&mesh.index_view);
        current_cmd_->DrawIndexedInstanced(mesh.index_count, 1, 0, 0, 0);
    }

    void release_geometry(panorama::PanoramaCompiledGeometryHandle geometry) override
    {
        const auto it = geometries_.find(geometry);
        if (it == geometries_.end())
        {
            return;
        }
        // Last used by the frame submitted before the most recent new_frame();
        // defer the buffer release until that frame's fence has passed.
        Trash trash;
        trash.resource_a = std::move(it->second.vertex_buffer);
        trash.resource_b = std::move(it->second.index_buffer);
        trash.srv_slot = kInvalidSlot;
        trash.fence_value = fence_value_;
        trash_.push_back(std::move(trash));
        geometries_.erase(it);
    }

private:
    static constexpr std::uint32_t kInvalidSlot = 0xFFFFFFFFU;

    struct Texture
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        std::uint32_t srv_slot = kInvalidSlot;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle{};
        int width = 0;
        int height = 0;
    };

    struct Geometry
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> vertex_buffer;
        Microsoft::WRL::ComPtr<ID3D12Resource> index_buffer;
        D3D12_VERTEX_BUFFER_VIEW vertex_view{};
        D3D12_INDEX_BUFFER_VIEW index_view{};
        UINT index_count = 0;
    };

    // A GPU resource retired while it may still be referenced by an in-flight
    // frame; freed once `fence_value` completes.
    struct Trash
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource_a;
        Microsoft::WRL::ComPtr<ID3D12Resource> resource_b;
        std::uint32_t srv_slot = kInvalidSlot;
        UINT64 fence_value = 0;
    };

    static void check_hr(HRESULT hr, const char* what)
    {
        if (FAILED(hr))
        {
            throw std::runtime_error(std::string("PanoramaD3D12Backend: ") + what + " failed (HRESULT 0x" +
                                     hr_to_hex(hr) + ")");
        }
    }

    static std::string hr_to_hex(HRESULT hr)
    {
        static const char* digits = "0123456789abcdef";
        const auto value = static_cast<std::uint32_t>(hr);
        std::string out(8, '0');
        for (int i = 0; i < 8; ++i)
        {
            out[static_cast<std::size_t>(7 - i)] = digits[(value >> (i * 4)) & 0xF];
        }
        return out;
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

    [[nodiscard]] ID3D12PipelineState* pipeline_for(panorama::PanoramaBlendMode mode) const
    {
        return pipelines_[blend_index(mode)].Get();
    }

    [[nodiscard]] D3D12_RECT current_scissor() const
    {
        const LONG fb_w = static_cast<LONG>(framebuffer_width_);
        const LONG fb_h = static_cast<LONG>(framebuffer_height_);
        if (!scissor_enabled_)
        {
            return D3D12_RECT{0, 0, fb_w, fb_h};
        }
        LONG left = scissor_x_ < 0 ? 0 : scissor_x_;
        LONG top = scissor_y_ < 0 ? 0 : scissor_y_;
        LONG right = scissor_x_ + scissor_width_;
        LONG bottom = scissor_y_ + scissor_height_;
        left = left > fb_w ? fb_w : left;
        top = top > fb_h ? fb_h : top;
        right = right < left ? left : (right > fb_w ? fb_w : right);
        bottom = bottom < top ? top : (bottom > fb_h ? fb_h : bottom);
        return D3D12_RECT{left, top, right, bottom};
    }

    // --- fence / deferred free -----------------------------------------------

    void create_fence()
    {
        check_hr(init_.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)), "CreateFence");
        fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (fence_event_ == nullptr)
        {
            throw std::runtime_error("PanoramaD3D12Backend: CreateEvent failed");
        }
    }

    void wait_for_fence(UINT64 value)
    {
        if (fence_->GetCompletedValue() >= value)
        {
            return;
        }
        check_hr(fence_->SetEventOnCompletion(value, fence_event_), "SetEventOnCompletion");
        WaitForSingleObject(fence_event_, INFINITE);
    }

    void reclaim_trash(UINT64 completed)
    {
        std::size_t write = 0;
        for (std::size_t read = 0; read < trash_.size(); ++read)
        {
            if (trash_[read].fence_value <= completed)
            {
                if (trash_[read].srv_slot != kInvalidSlot)
                {
                    free_srv_slot(trash_[read].srv_slot);
                }
                // ComPtrs release as this entry is overwritten/dropped.
            }
            else
            {
                if (write != read)
                {
                    trash_[write] = std::move(trash_[read]);
                }
                ++write;
            }
        }
        trash_.resize(write);
    }

    void retire_texture(Texture& texture)
    {
        Trash trash;
        trash.resource_a = std::move(texture.resource);
        trash.srv_slot = texture.srv_slot;
        trash.fence_value = fence_value_;
        trash_.push_back(std::move(trash));
        texture.srv_slot = kInvalidSlot;
    }

    // --- upload command objects ----------------------------------------------

    void create_upload_objects()
    {
        check_hr(init_.device->CreateCommandAllocator(
                     D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&upload_allocator_)),
                 "CreateCommandAllocator(upload)");
        check_hr(init_.device->CreateCommandList(
                     0, D3D12_COMMAND_LIST_TYPE_DIRECT, upload_allocator_.Get(), nullptr, IID_PPV_ARGS(&upload_list_)),
                 "CreateCommandList(upload)");
        check_hr(upload_list_->Close(), "Close(upload list)");
    }

    // --- root signature / pipelines ------------------------------------------

    void create_root_signature()
    {
        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = 0;
        range.RegisterSpace = 0;
        range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        std::array<D3D12_ROOT_PARAMETER, 2> params{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0; // b0
        params[0].Constants.RegisterSpace = 0;
        params[0].Constants.Num32BitValues = 16; // mat4 projection
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges = &range;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = init_.linear_filter ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = 0; // s0
        sampler.RegisterSpace = 0;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters = static_cast<UINT>(params.size());
        desc.pParameters = params.data();
        desc.NumStaticSamplers = 1;
        desc.pStaticSamplers = &sampler;
        desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        Microsoft::WRL::ComPtr<ID3DBlob> blob;
        Microsoft::WRL::ComPtr<ID3DBlob> error;
        const HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
        if (FAILED(hr))
        {
            std::string message = "PanoramaD3D12Backend: D3D12SerializeRootSignature failed";
            if (error)
            {
                message += ": ";
                message.append(static_cast<const char*>(error->GetBufferPointer()), error->GetBufferSize());
            }
            throw std::runtime_error(message);
        }
        check_hr(init_.device->CreateRootSignature(
                     0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_signature_)),
                 "CreateRootSignature");
    }

    [[nodiscard]] static Microsoft::WRL::ComPtr<ID3DBlob> compile_shader(const char* entry, const char* target)
    {
        static const char* kHlsl = R"HLSL(
cbuffer Constants : register(b0) { row_major float4x4 uProj; };

struct VSInput { float2 pos : POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; };
struct PSInput { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 col : COLOR0; };

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.pos = mul(uProj, float4(input.pos, 0.0f, 1.0f));
    output.uv = input.uv;
    output.col = input.col;
    return output;
}

Texture2D uTex : register(t0);
SamplerState uSampler : register(s0);

float4 PSMain(PSInput input) : SV_Target
{
    return input.col * uTex.Sample(uSampler, input.uv);
}
)HLSL";

        Microsoft::WRL::ComPtr<ID3DBlob> code;
        Microsoft::WRL::ComPtr<ID3DBlob> error;
        const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
        const HRESULT hr = D3DCompile(
            kHlsl, std::strlen(kHlsl), "panorama_ui.hlsl", nullptr, nullptr, entry, target, flags, 0, &code, &error);
        if (FAILED(hr))
        {
            std::string message = "PanoramaD3D12Backend: shader compile failed";
            if (error)
            {
                message += ": ";
                message.append(static_cast<const char*>(error->GetBufferPointer()), error->GetBufferSize());
            }
            throw std::runtime_error(message);
        }
        return code;
    }

    [[nodiscard]] static D3D12_RENDER_TARGET_BLEND_DESC blend_desc(panorama::PanoramaBlendMode mode)
    {
        D3D12_RENDER_TARGET_BLEND_DESC blend{};
        blend.BlendEnable = TRUE;
        blend.LogicOpEnable = FALSE;
        blend.LogicOp = D3D12_LOGIC_OP_NOOP;
        blend.BlendOp = D3D12_BLEND_OP_ADD;
        blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        // Straight (non-premultiplied) alpha, matching PanoramaDrawList colors.
        switch (mode)
        {
        case panorama::PanoramaBlendMode::Normal:
            blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
            blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            blend.SrcBlendAlpha = D3D12_BLEND_ONE;
            blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
            break;
        case panorama::PanoramaBlendMode::Additive:
            blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
            blend.DestBlend = D3D12_BLEND_ONE;
            blend.SrcBlendAlpha = D3D12_BLEND_ONE;
            blend.DestBlendAlpha = D3D12_BLEND_ONE;
            break;
        case panorama::PanoramaBlendMode::Screen:
            blend.SrcBlend = D3D12_BLEND_ONE;
            blend.DestBlend = D3D12_BLEND_INV_SRC_COLOR;
            blend.SrcBlendAlpha = D3D12_BLEND_ONE;
            blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
            break;
        case panorama::PanoramaBlendMode::Multiply:
            blend.SrcBlend = D3D12_BLEND_DEST_COLOR;
            blend.DestBlend = D3D12_BLEND_ZERO;
            blend.SrcBlendAlpha = D3D12_BLEND_DEST_ALPHA;
            blend.DestBlendAlpha = D3D12_BLEND_ZERO;
            break;
        case panorama::PanoramaBlendMode::Opaque:
            blend.BlendEnable = FALSE;
            blend.SrcBlend = D3D12_BLEND_ONE;
            blend.DestBlend = D3D12_BLEND_ZERO;
            blend.SrcBlendAlpha = D3D12_BLEND_ONE;
            blend.DestBlendAlpha = D3D12_BLEND_ZERO;
            break;
        }
        return blend;
    }

    void create_pipelines()
    {
        const Microsoft::WRL::ComPtr<ID3DBlob> vs = compile_shader("VSMain", "vs_5_0");
        const Microsoft::WRL::ComPtr<ID3DBlob> ps = compile_shader("PSMain", "ps_5_0");

        const std::array<D3D12_INPUT_ELEMENT_DESC, 3> layout{{
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(panorama::PanoramaPaintVertex, x),
             D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(panorama::PanoramaPaintVertex, u),
             D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, offsetof(panorama::PanoramaPaintVertex, color),
             D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        }};

        D3D12_RASTERIZER_DESC raster{};
        raster.FillMode = D3D12_FILL_MODE_SOLID;
        raster.CullMode = D3D12_CULL_MODE_NONE;
        raster.FrontCounterClockwise = FALSE;
        raster.DepthClipEnable = TRUE;

        constexpr std::array<panorama::PanoramaBlendMode, 5> modes{
            panorama::PanoramaBlendMode::Normal, panorama::PanoramaBlendMode::Additive,
            panorama::PanoramaBlendMode::Screen, panorama::PanoramaBlendMode::Multiply,
            panorama::PanoramaBlendMode::Opaque};
        for (const panorama::PanoramaBlendMode mode : modes)
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
            desc.pRootSignature = root_signature_.Get();
            desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
            desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
            desc.BlendState.AlphaToCoverageEnable = FALSE;
            desc.BlendState.IndependentBlendEnable = FALSE;
            desc.BlendState.RenderTarget[0] = blend_desc(mode);
            desc.SampleMask = UINT_MAX;
            desc.RasterizerState = raster;
            desc.DepthStencilState.DepthEnable = FALSE;
            desc.DepthStencilState.StencilEnable = FALSE;
            desc.InputLayout = {layout.data(), static_cast<UINT>(layout.size())};
            desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            desc.NumRenderTargets = 1;
            desc.RTVFormats[0] = init_.rtv_format;
            desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
            desc.SampleDesc = init_.sample_desc;
            check_hr(init_.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipelines_[blend_index(mode)])),
                     "CreateGraphicsPipelineState");
        }
    }

    // Matches the enum-to-index mapping in blend_index().
    static_assert(static_cast<int>(panorama::PanoramaBlendMode::Normal) == 0);

    // --- SRV heap -------------------------------------------------------------

    void create_srv_heap()
    {
        srv_descriptor_size_ =
            init_.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = init_.max_textures;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        check_hr(init_.device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&srv_heap_)), "CreateDescriptorHeap");

        free_slots_.reserve(init_.max_textures);
        for (std::uint32_t i = init_.max_textures; i-- > 0;)
        {
            free_slots_.push_back(i);
        }
    }

    [[nodiscard]] std::uint32_t allocate_srv_slot()
    {
        if (free_slots_.empty())
        {
            throw std::runtime_error("PanoramaD3D12Backend: SRV heap exhausted (raise PanoramaD3D12Init.max_textures)");
        }
        const std::uint32_t slot = free_slots_.back();
        free_slots_.pop_back();
        return slot;
    }

    void free_srv_slot(std::uint32_t slot) { free_slots_.push_back(slot); }

    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle(std::uint32_t slot) const
    {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = srv_heap_->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(slot) * srv_descriptor_size_;
        return handle;
    }

    [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle(std::uint32_t slot) const
    {
        D3D12_GPU_DESCRIPTOR_HANDLE handle = srv_heap_->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<UINT64>(slot) * srv_descriptor_size_;
        return handle;
    }

    // --- textures -------------------------------------------------------------

    void create_white_texture()
    {
        white_ = create_texture(1, 1);
        const std::array<unsigned char, 4> pixel{255, 255, 255, 255};
        upload_texture(white_, pixel, /*already_shader_readable=*/false);
    }

    [[nodiscard]] Texture create_texture(int width, int height)
    {
        Texture texture;
        texture.width = width;
        texture.height = height;

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = static_cast<UINT64>(width);
        desc.Height = static_cast<UINT>(height);
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc = {1, 0};
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;

        check_hr(init_.device->CreateCommittedResource(
                     &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                     IID_PPV_ARGS(&texture.resource)),
                 "CreateCommittedResource(texture)");

        texture.srv_slot = allocate_srv_slot();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        init_.device->CreateShaderResourceView(texture.resource.Get(), &srv, cpu_handle(texture.srv_slot));
        texture.gpu_handle = gpu_handle(texture.srv_slot);
        return texture;
    }

    void upload_texture(const Texture& texture, std::span<const unsigned char> rgba, bool already_shader_readable)
    {
        const D3D12_RESOURCE_DESC desc = texture.resource->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT num_rows = 0;
        UINT64 row_size = 0;
        UINT64 upload_size = 0;
        init_.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &num_rows, &row_size, &upload_size);

        Microsoft::WRL::ComPtr<ID3D12Resource> upload = create_upload_buffer(upload_size, nullptr);

        std::uint8_t* mapped = nullptr;
        const D3D12_RANGE no_read{0, 0};
        check_hr(upload->Map(0, &no_read, reinterpret_cast<void**>(&mapped)), "Map(texture upload)");
        const auto src_row_pitch = static_cast<std::size_t>(texture.width) * 4;
        const auto copy_row = row_size < src_row_pitch ? static_cast<std::size_t>(row_size) : src_row_pitch;
        for (UINT row = 0; row < num_rows; ++row)
        {
            const std::size_t src_offset = row * src_row_pitch;
            if (src_offset >= rgba.size())
            {
                break;
            }
            std::memcpy(
                mapped + footprint.Offset + static_cast<std::size_t>(row) * footprint.Footprint.RowPitch,
                rgba.data() + src_offset,
                copy_row);
        }
        upload->Unmap(0, nullptr);

        check_hr(upload_allocator_->Reset(), "upload allocator Reset");
        check_hr(upload_list_->Reset(upload_allocator_.Get(), nullptr), "upload list Reset");

        if (already_shader_readable)
        {
            barrier(upload_list_.Get(), texture.resource.Get(),
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        }

        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = texture.resource.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = upload.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = footprint;
        upload_list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        barrier(upload_list_.Get(), texture.resource.Get(),
                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        check_hr(upload_list_->Close(), "Close(upload list)");
        ID3D12CommandList* lists[] = {upload_list_.Get()};
        init_.queue->ExecuteCommandLists(1, lists);

        ++fence_value_;
        check_hr(init_.queue->Signal(fence_.Get(), fence_value_), "Signal(upload)");
        wait_for_fence(fence_value_); // keep `upload` alive until the copy finishes
    }

    static void barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                        D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = resource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
        list->ResourceBarrier(1, &b);
    }

    // --- upload-heap buffers --------------------------------------------------

    // Creates an UPLOAD-heap buffer, optionally seeded with `data` (nullptr to
    // leave it for a later Map). UPLOAD-heap buffers are CPU-writable and
    // GPU-readable directly, which is plenty for UI vertex/index data and the
    // texture staging copy.
    [[nodiscard]] Microsoft::WRL::ComPtr<ID3D12Resource> create_upload_buffer(UINT64 size, const void* data)
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size == 0 ? 1 : size;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc = {1, 0};
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
        check_hr(init_.device->CreateCommittedResource(
                     &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                     IID_PPV_ARGS(&buffer)),
                 "CreateCommittedResource(upload buffer)");

        if (data != nullptr && size > 0)
        {
            void* mapped = nullptr;
            const D3D12_RANGE no_read{0, 0};
            check_hr(buffer->Map(0, &no_read, &mapped), "Map(upload buffer)");
            std::memcpy(mapped, data, static_cast<std::size_t>(size));
            buffer->Unmap(0, nullptr);
        }
        return buffer;
    }

    PanoramaD3D12Init init_;

    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    HANDLE fence_event_ = nullptr;
    UINT64 fence_value_ = 0;

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> upload_allocator_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> upload_list_;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature_;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 5> pipelines_;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srv_heap_;
    UINT srv_descriptor_size_ = 0;
    std::vector<std::uint32_t> free_slots_;

    std::unordered_map<panorama::PanoramaTextureId, Texture> textures_;
    std::unordered_map<panorama::PanoramaCompiledGeometryHandle, Geometry> geometries_;
    std::vector<Trash> trash_;
    Texture white_;
    panorama::PanoramaTextureId next_texture_id_ = 1;
    panorama::PanoramaCompiledGeometryHandle next_geometry_id_ = 1;

    // Per-frame state set by new_frame().
    ID3D12GraphicsCommandList* current_cmd_ = nullptr;
    std::uint32_t framebuffer_width_ = 0;
    std::uint32_t framebuffer_height_ = 0;
    std::array<float, 16> projection_{};
    panorama::PanoramaBlendMode current_blend_ = panorama::PanoramaBlendMode::Normal;
    bool scissor_enabled_ = false;
    int scissor_x_ = 0;
    int scissor_y_ = 0;
    int scissor_width_ = 0;
    int scissor_height_ = 0;
};
}
