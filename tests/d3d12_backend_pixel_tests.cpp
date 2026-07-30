#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "adapters/panorama_d3d12_backend.hpp"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace
{
using Microsoft::WRL::ComPtr;

[[noreturn]] void fail(const char* message)
{
    std::fprintf(stderr, "d3d12 backend pixel test failed: %s\n", message);
    std::exit(1);
}

void check_hr(HRESULT hr, const char* operation)
{
    if (FAILED(hr))
    {
        throw std::runtime_error(std::string(operation) + " failed");
    }
}

void wait_for_queue(ID3D12Device* device, ID3D12CommandQueue* queue)
{
    ComPtr<ID3D12Fence> fence;
    check_hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)),
        "CreateFence");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event == nullptr)
    {
        fail("CreateEventW");
    }
    check_hr(queue->Signal(fence.Get(), 1), "Signal");
    check_hr(fence->SetEventOnCompletion(1, event), "SetEventOnCompletion");
    WaitForSingleObject(event, INFINITE);
    CloseHandle(event);
}

ComPtr<ID3D12Resource> create_render_target(ID3D12Device* device)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 4;
    desc.Height = 4;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc = {1, 0};
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    const D3D12_CLEAR_VALUE clear{DXGI_FORMAT_R8G8B8A8_UNORM, {0.0F, 0.0F, 0.0F, 0.0F}};

    ComPtr<ID3D12Resource> target;
    check_hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                 D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(&target)),
        "CreateCommittedResource(render target)");
    return target;
}

std::array<std::uint8_t, 4> read_center_pixel(
    ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* target)
{
    const D3D12_RESOURCE_DESC texture_desc = target->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 total_bytes = 0;
    device->GetCopyableFootprints(
        &texture_desc, 0, 1, 0, &footprint, nullptr, nullptr, &total_bytes);

    D3D12_HEAP_PROPERTIES readback_heap{};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer_desc{};
    buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer_desc.Width = total_bytes;
    buffer_desc.Height = 1;
    buffer_desc.DepthOrArraySize = 1;
    buffer_desc.MipLevels = 1;
    buffer_desc.SampleDesc = {1, 0};
    buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> readback;
    check_hr(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE,
                 &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                 IID_PPV_ARGS(&readback)),
        "CreateCommittedResource(readback)");

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    check_hr(device->CreateCommandAllocator(
                 D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
        "CreateCommandAllocator(readback)");
    check_hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
                 nullptr, IID_PPV_ARGS(&list)),
        "CreateCommandList(readback)");

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = target;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = target;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    check_hr(list->Close(), "Close(readback)");
    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    wait_for_queue(device, queue);

    void* mapped_data = nullptr;
    const D3D12_RANGE read_range{
        footprint.Offset, footprint.Offset + static_cast<SIZE_T>(total_bytes)};
    check_hr(readback->Map(0, &read_range, &mapped_data),
        "Map(readback)");
    const auto* mapped = static_cast<const std::uint8_t*>(mapped_data);
    const std::size_t offset = static_cast<std::size_t>(footprint.Offset) +
        2U * footprint.Footprint.RowPitch + 2U * 4U;
    const std::array<std::uint8_t, 4> pixel{
        mapped[offset], mapped[offset + 1], mapped[offset + 2], mapped[offset + 3]};
    readback->Unmap(0, nullptr);
    return pixel;
}
}

int main()
{
    try
    {
        ComPtr<IDXGIFactory6> factory;
        check_hr(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
        ComPtr<IDXGIAdapter> warp;
        check_hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "EnumWarpAdapter");
        ComPtr<ID3D12Device> device;
        check_hr(D3D12CreateDevice(
                     warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)),
            "D3D12CreateDevice(WARP)");

        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ComPtr<ID3D12CommandQueue> queue;
        check_hr(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)),
            "CreateCommandQueue");

        panorama_adapters::PanoramaD3D12Init init;
        init.device = device.Get();
        init.queue = queue.Get();
        init.linear_filter = false;
        panorama_adapters::PanoramaD3D12Backend backend(init);

        ComPtr<ID3D12Resource> target = create_render_target(device.Get());
        D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{};
        rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtv_heap_desc.NumDescriptors = 1;
        ComPtr<ID3D12DescriptorHeap> rtv_heap;
        check_hr(device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&rtv_heap)),
            "CreateDescriptorHeap(RTV)");
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv =
            rtv_heap->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(target.Get(), nullptr, rtv);

        // Font atlas coverage is premultiplied (a,a,a,a). With the old
        // straight-alpha pipeline, a=128 was multiplied by alpha again and
        // read back near 64. Correct premultiplied composition stays near 128.
        const std::array<unsigned char, 4> atlas_texel{128, 128, 128, 128};
        const panorama::PanoramaTextureId texture =
            backend.generate_texture(atlas_texel, 1, 1);
        if (texture == 0)
        {
            fail("generate_texture returned zero");
        }
        const std::array<panorama::PanoramaPaintVertex, 4> vertices{{
            {0.0F, 0.0F, 0.5F, 0.5F, {255, 255, 255, 255}},
            {4.0F, 0.0F, 0.5F, 0.5F, {255, 255, 255, 255}},
            {4.0F, 4.0F, 0.5F, 0.5F, {255, 255, 255, 255}},
            {0.0F, 4.0F, 0.5F, 0.5F, {255, 255, 255, 255}},
        }};
        const std::array<int, 6> indices{0, 1, 2, 0, 2, 3};
        const panorama::PanoramaCompiledGeometryHandle geometry =
            backend.compile_geometry(vertices, indices, 1.0F);
        if (geometry == 0)
        {
            fail("compile_geometry returned zero");
        }

        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        check_hr(device->CreateCommandAllocator(
                     D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
            "CreateCommandAllocator(frame)");
        check_hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                     allocator.Get(), nullptr, IID_PPV_ARGS(&list)),
            "CreateCommandList(frame)");
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const float transparent[4] = {0.0F, 0.0F, 0.0F, 0.0F};
        list->ClearRenderTargetView(rtv, transparent, 0, nullptr);
        const panorama::PanoramaSubmissionId submission =
            backend.new_frame(list.Get(), 4, 4);
        backend.render_geometry(geometry, texture, panorama::PanoramaDrawConstants{});
        check_hr(list->Close(), "Close(frame)");
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        (void)backend.submit_frame(submission);
        backend.flush();

        const std::array<std::uint8_t, 4> pixel =
            read_center_pixel(device.Get(), queue.Get(), target.Get());
        for (std::uint8_t channel : pixel)
        {
            if (channel < 127 || channel > 129)
            {
                fail("premultiplied coverage readback was not 128 +/- 1");
            }
        }

        backend.release_geometry(geometry);
        backend.release_texture(texture);
        backend.flush();
        std::puts("panorama d3d12 backend pixel tests passed");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "d3d12 backend pixel test failed: %s\n", error.what());
        return 1;
    }
}
