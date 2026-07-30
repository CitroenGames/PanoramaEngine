// PanoramaEngine example 06 -- a Direct3D 12 Snake game with a Panorama HUD.
//
// The application owns the native window, D3D12 device/queue, swap chain,
// render targets, command allocators, frame fence, game simulation, and the
// custom vertex pipeline that draws the board, snake, and food. PanoramaEngine
// owns only the overlay document/runtime/input lifecycle and emits the HUD/menu
// draw list composited after the native game pass.
#include "native_snake_world.hpp"

#include "adapters/panorama_d3d12_backend.hpp"
#include "ui/panorama/panorama_font_atlas.hpp"
#include "ui/panorama/panorama_geometry_cache.hpp"
#include "ui/panorama/panorama_resource_provider.hpp"
#include "ui/panorama/panorama_view.hpp"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#pragma comment(lib, "dxgi.lib")

namespace
{
using Microsoft::WRL::ComPtr;
using native_snake::LevelViewport;
using native_snake::NativeSnakeRenderer;
using native_snake::SnakeGame;
using FrameClock = std::chrono::steady_clock;

constexpr wchar_t kWindowClassName[] = L"PanoramaExampleD3D12Snake";
constexpr int kInitialWidth = 1100;
constexpr int kInitialHeight = 720;
constexpr UINT_PTR kFrameTimerId = 1;
constexpr UINT kFrameIntervalMilliseconds = 16;
constexpr UINT kFrameCount = 2;
constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr float kHudHeight = 104.0F;
constexpr float kFooterHeight = 82.0F;

[[nodiscard]] std::string hr_to_hex(HRESULT hr)
{
    constexpr char digits[] = "0123456789abcdef";
    const auto value = static_cast<std::uint32_t>(hr);
    std::string out(8, '0');
    for (int i = 0; i < 8; ++i)
    {
        out[static_cast<std::size_t>(7 - i)] = digits[(value >> (i * 4)) & 0xFU];
    }
    return out;
}

void check_hr(HRESULT hr, std::string_view operation)
{
    if (FAILED(hr))
    {
        throw std::runtime_error(
            std::string(operation) + " failed (HRESULT 0x" + hr_to_hex(hr) + ")");
    }
}

void transition(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    command_list->ResourceBarrier(1, &barrier);
}

class D3D12WindowHost
{
public:
    ~D3D12WindowHost()
    {
        if (queue_ != nullptr && fence_ != nullptr)
        {
            try
            {
                wait_for_gpu();
            }
            catch (const std::exception& error)
            {
                std::fprintf(stderr, "D3D12 shutdown wait failed: %s\n", error.what());
            }
        }
        if (fence_event_ != nullptr)
        {
            CloseHandle(fence_event_);
        }
    }

    D3D12WindowHost(const D3D12WindowHost&) = delete;
    D3D12WindowHost& operator=(const D3D12WindowHost&) = delete;

    D3D12WindowHost() = default;

    void initialize(HWND hwnd, int width, int height, bool force_warp)
    {
        hwnd_ = hwnd;
        width_ = std::max(width, 1);
        height_ = std::max(height, 1);

        UINT factory_flags = 0;
#if defined(_DEBUG)
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
        {
            debug->EnableDebugLayer();
            factory_flags |= DXGI_CREATE_FACTORY_DEBUG;
        }
#endif

        check_hr(CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&factory_)), "CreateDXGIFactory2");

        const ComPtr<IDXGIAdapter1> adapter = select_adapter(force_warp);
        DXGI_ADAPTER_DESC1 adapter_desc{};
        check_hr(adapter->GetDesc1(&adapter_desc), "IDXGIAdapter1::GetDesc1");
        std::wprintf(L"D3D12 adapter: %ls\n", adapter_desc.Description);

        check_hr(
            D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)),
            "D3D12CreateDevice");

        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        check_hr(device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue_)), "CreateCommandQueue");

        create_swap_chain();
        create_render_target_heap();
        create_render_targets();

        for (ComPtr<ID3D12CommandAllocator>& allocator : command_allocators_)
        {
            check_hr(
                device_->CreateCommandAllocator(
                    D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
                "CreateCommandAllocator");
        }
        check_hr(
            device_->CreateCommandList(
                0,
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                command_allocators_[frame_index_].Get(),
                nullptr,
                IID_PPV_ARGS(&command_list_)),
            "CreateCommandList");
        check_hr(command_list_->Close(), "ID3D12GraphicsCommandList::Close(initial)");

        check_hr(
            device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)),
            "CreateFence");
        fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (fence_event_ == nullptr)
        {
            throw std::runtime_error("CreateEventW(frame fence) failed");
        }
    }

    [[nodiscard]] ID3D12Device* device() const noexcept { return device_.Get(); }
    [[nodiscard]] ID3D12CommandQueue* queue() const noexcept { return queue_.Get(); }

    // Returns true when the swap-chain buffers were actually resized.
    bool resize(int width, int height)
    {
        if (width <= 0 || height <= 0 || (width == width_ && height == height_))
        {
            return false;
        }

        wait_for_gpu();
        for (ComPtr<ID3D12Resource>& render_target : render_targets_)
        {
            render_target.Reset();
        }

        check_hr(
            swap_chain_->ResizeBuffers(kFrameCount, static_cast<UINT>(width), static_cast<UINT>(height),
                kBackBufferFormat, 0),
            "IDXGISwapChain::ResizeBuffers");
        width_ = width;
        height_ = height;
        frame_index_ = swap_chain_->GetCurrentBackBufferIndex();
        frame_fence_values_.fill(0);
        create_render_targets();
        return true;
    }

    void render(
        NativeSnakeRenderer& snake_renderer,
        const SnakeGame& game,
        panorama_adapters::PanoramaD3D12Backend& backend,
        panorama::PanoramaGeometryCache& geometry_cache,
        const panorama::PanoramaDrawList& draw_list,
        bool draw_list_changed)
    {
        wait_for_frame(frame_index_);

        ID3D12CommandAllocator* allocator = command_allocators_[frame_index_].Get();
        check_hr(allocator->Reset(), "ID3D12CommandAllocator::Reset");
        check_hr(command_list_->Reset(allocator, nullptr), "ID3D12GraphicsCommandList::Reset");

        ID3D12Resource* back_buffer = render_targets_[frame_index_].Get();
        transition(
            command_list_.Get(), back_buffer,
            D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_handles_[frame_index_];
        command_list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        constexpr float clear_color[4] = {
            5.0F / 255.0F,
            10.0F / 255.0F,
            15.0F / 255.0F,
            1.0F,
        };
        command_list_->ClearRenderTargetView(rtv, clear_color, 0, nullptr);

        // This is the game pass. It uses its own root signature, PSO, dynamic
        // vertex buffer, and draw call; no Panorama nodes or draw-list commands
        // are involved in the board, food, or snake.
        const LevelViewport level_viewport{
            0.0F,
            kHudHeight,
            static_cast<float>(width_),
            std::max(kHudHeight + 1.0F, static_cast<float>(height_) - kFooterHeight),
        };
        snake_renderer.record(
            command_list_.Get(),
            frame_index_,
            width_,
            height_,
            level_viewport,
            game);

        // Panorama is a second pass used only for the transparent HUD/menu.
        const panorama::PanoramaSubmissionId ui_submission = backend.new_frame(
            command_list_.Get(), static_cast<std::uint32_t>(width_), static_cast<std::uint32_t>(height_));
        if (draw_list_changed || !geometry_cache.replay(backend))
        {
            geometry_cache.submit(draw_list, backend, 1.0F);
            if (!geometry_cache.valid())
            {
                throw std::runtime_error("PanoramaGeometryCache failed to compile the draw list");
            }
        }

        transition(
            command_list_.Get(), back_buffer,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        check_hr(command_list_->Close(), "ID3D12GraphicsCommandList::Close(frame)");

        ID3D12CommandList* command_lists[] = {command_list_.Get()};
        queue_->ExecuteCommandLists(1, command_lists);
        (void)backend.submit_frame(ui_submission);
        check_hr(swap_chain_->Present(1, 0), "IDXGISwapChain::Present");

        const UINT submitted_frame = frame_index_;
        const UINT64 fence_value = next_fence_value_++;
        check_hr(queue_->Signal(fence_.Get(), fence_value), "ID3D12CommandQueue::Signal(frame)");
        frame_fence_values_[submitted_frame] = fence_value;
        frame_index_ = swap_chain_->GetCurrentBackBufferIndex();
    }

    void wait_for_gpu()
    {
        if (queue_ == nullptr || fence_ == nullptr)
        {
            return;
        }
        const UINT64 fence_value = next_fence_value_++;
        check_hr(queue_->Signal(fence_.Get(), fence_value), "ID3D12CommandQueue::Signal(flush)");
        wait_for_fence(fence_value);
        frame_fence_values_.fill(0);
    }

private:
    [[nodiscard]] ComPtr<IDXGIAdapter1> select_adapter(bool force_warp)
    {
        if (force_warp)
        {
            ComPtr<IDXGIAdapter1> warp;
            check_hr(factory_->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "IDXGIFactory::EnumWarpAdapter");
            return warp;
        }

        ComPtr<IDXGIAdapter1> adapter;
        for (UINT index = 0;
             factory_->EnumAdapterByGpuPreference(
                 index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) !=
             DXGI_ERROR_NOT_FOUND;
             ++index)
        {
            DXGI_ADAPTER_DESC1 desc{};
            if (FAILED(adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
            {
                adapter.Reset();
                continue;
            }
            if (SUCCEEDED(D3D12CreateDevice(
                    adapter.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)))
            {
                return adapter;
            }
            adapter.Reset();
        }

        // WARP keeps the example usable on remote sessions and machines without
        // a D3D12-capable hardware adapter.
        check_hr(factory_->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "IDXGIFactory::EnumWarpAdapter");
        return adapter;
    }

    void create_swap_chain()
    {
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = static_cast<UINT>(width_);
        desc.Height = static_cast<UINT>(height_);
        desc.Format = kBackBufferFormat;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = kFrameCount;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

        ComPtr<IDXGISwapChain1> swap_chain;
        check_hr(
            factory_->CreateSwapChainForHwnd(queue_.Get(), hwnd_, &desc, nullptr, nullptr, &swap_chain),
            "IDXGIFactory::CreateSwapChainForHwnd");
        check_hr(swap_chain.As(&swap_chain_), "Query IDXGISwapChain3");
        check_hr(
            factory_->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER),
            "IDXGIFactory::MakeWindowAssociation");
        frame_index_ = swap_chain_->GetCurrentBackBufferIndex();
    }

    void create_render_target_heap()
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.NumDescriptors = kFrameCount;
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        check_hr(
            device_->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&rtv_heap_)),
            "CreateDescriptorHeap(RTV)");
        rtv_descriptor_size_ =
            device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }

    void create_render_targets()
    {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
        for (UINT index = 0; index < kFrameCount; ++index)
        {
            check_hr(
                swap_chain_->GetBuffer(index, IID_PPV_ARGS(&render_targets_[index])),
                "IDXGISwapChain::GetBuffer");
            device_->CreateRenderTargetView(render_targets_[index].Get(), nullptr, handle);
            rtv_handles_[index] = handle;
            handle.ptr += rtv_descriptor_size_;
        }
    }

    void wait_for_frame(UINT frame)
    {
        const UINT64 value = frame_fence_values_[frame];
        if (value != 0)
        {
            wait_for_fence(value);
        }
    }

    void wait_for_fence(UINT64 value)
    {
        if (fence_->GetCompletedValue() >= value)
        {
            return;
        }
        check_hr(
            fence_->SetEventOnCompletion(value, fence_event_),
            "ID3D12Fence::SetEventOnCompletion");
        if (WaitForSingleObject(fence_event_, INFINITE) != WAIT_OBJECT_0)
        {
            throw std::runtime_error("WaitForSingleObject(frame fence) failed");
        }
    }

    HWND hwnd_ = nullptr;
    int width_ = 0;
    int height_ = 0;

    ComPtr<IDXGIFactory6> factory_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<IDXGISwapChain3> swap_chain_;
    ComPtr<ID3D12DescriptorHeap> rtv_heap_;
    std::array<ComPtr<ID3D12Resource>, kFrameCount> render_targets_;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kFrameCount> rtv_handles_{};
    UINT rtv_descriptor_size_ = 0;

    std::array<ComPtr<ID3D12CommandAllocator>, kFrameCount> command_allocators_;
    ComPtr<ID3D12GraphicsCommandList> command_list_;
    ComPtr<ID3D12Fence> fence_;
    HANDLE fence_event_ = nullptr;
    std::array<UINT64, kFrameCount> frame_fence_values_{};
    UINT64 next_fence_value_ = 1;
    UINT frame_index_ = 0;
};

class PanoramaD3D12App
{
public:
    ~PanoramaD3D12App() { shutdown(); }

    PanoramaD3D12App(const PanoramaD3D12App&) = delete;
    PanoramaD3D12App& operator=(const PanoramaD3D12App&) = delete;

    PanoramaD3D12App() = default;

    void initialize(
        HWND hwnd,
        const std::filesystem::path& xml_path,
        int width,
        int height,
        bool force_warp)
    {
        graphics_.initialize(hwnd, width, height, force_warp);
        snake_renderer_.initialize(graphics_.device());

        panorama_adapters::PanoramaD3D12Init backend_init;
        backend_init.device = graphics_.device();
        backend_init.queue = graphics_.queue();
        backend_init.rtv_format = kBackBufferFormat;
        backend_ = std::make_unique<panorama_adapters::PanoramaD3D12Backend>(backend_init);
        panorama::set_panorama_render_backend(backend_.get());

        std::error_code path_error;
        const std::filesystem::path absolute = std::filesystem::absolute(xml_path, path_error);
        if (path_error)
        {
            throw std::runtime_error("invalid layout path: " + xml_path.string());
        }
        const std::filesystem::path resource_root = absolute.parent_path();

        const bool font_loaded = font_atlas_.load(resource_root);
        if (!font_loaded)
        {
            std::fprintf(
                stderr,
                "no font found under %s -- text will not render\n",
                (resource_root / "resource/ui/fonts").string().c_str());
        }

        view_.set_viewport(static_cast<float>(width), static_cast<float>(height));
        view_.set_font_atlas(font_loaded ? &font_atlas_ : nullptr);
        view_.resources().add_provider(
            std::make_unique<panorama::PanoramaDirectoryResourceProvider>(resource_root));
        view_.runtime().set_host_action_handler(
            [this](const std::string& action, const std::string& argument) {
                if (action == "snake")
                {
                    pending_action_ = argument;
                }
            });

        panorama::PanoramaViewLoadOptions options;
        options.document.resource_root = resource_root;
        if (!view_.load(absolute.filename().string(), std::move(options)))
        {
            throw std::runtime_error("failed to load layout: " + absolute.string());
        }

        cache_hud_nodes();
        sync_hud(true);
        const panorama::PanoramaViewUpdateResult result = view_.update(0.0F);
        draw_list_changed_ = true;
        draw_list_changed_ = draw_list_changed_ || result.draw_list_rebuilt;
        initialized_ = true;
        last_update_ = FrameClock::now();
        render();
    }

    void shutdown() noexcept
    {
        if (backend_ == nullptr)
        {
            return;
        }

        try
        {
            graphics_.wait_for_gpu();
            geometry_cache_.release();
            view_.unload();
            font_atlas_.clear();
            backend_->flush();
            clear_hud_nodes();
        }
        catch (const std::exception& error)
        {
            std::fprintf(stderr, "D3D12 Snake shutdown failed: %s\n", error.what());
        }

        if (panorama::panorama_render_backend() == backend_.get())
        {
            panorama::set_panorama_render_backend(nullptr);
        }
        backend_.reset();
        initialized_ = false;
    }

    [[nodiscard]] bool initialized() const noexcept { return initialized_; }

    void tick()
    {
        const FrameClock::time_point now = FrameClock::now();
        const float dt_seconds = std::clamp(
            std::chrono::duration<float>(now - last_update_).count(), 0.0F, 0.1F);
        last_update_ = now;
        apply_pending_action();
        if (game_.update(dt_seconds))
        {
            sync_hud(false);
        }
        const panorama::PanoramaViewUpdateResult result = view_.update(dt_seconds);
        draw_list_changed_ = draw_list_changed_ || result.draw_list_rebuilt;
        render();
    }

    void resize(int width, int height)
    {
        if (!graphics_.resize(width, height))
        {
            return;
        }
        view_.set_viewport(static_cast<float>(width), static_cast<float>(height));
        const panorama::PanoramaViewUpdateResult result = view_.update(0.0F);
        draw_list_changed_ = draw_list_changed_ || result.draw_list_rebuilt;
        render();
    }

    bool update_pointer(float x, float y, bool down) { return view_.update_pointer(x, y, down); }
    bool update_wheel(float x, float y, float ticks) { return view_.update_wheel(x, y, ticks); }
    bool handle_key_down(const panorama::PanoramaKeyEvent& event) { return view_.handle_key_down(event); }
    bool handle_text_input(std::string_view text) { return view_.handle_text_input(text); }

    bool handle_game_key(WPARAM key, bool repeated)
    {
        bool consumed = true;
        bool hud_changed = false;
        switch (key)
        {
        case VK_UP:
        case 'W':
            hud_changed = game_.set_direction(SnakeGame::Direction::Up);
            break;
        case VK_DOWN:
        case 'S':
            hud_changed = game_.set_direction(SnakeGame::Direction::Down);
            break;
        case VK_LEFT:
        case 'A':
            hud_changed = game_.set_direction(SnakeGame::Direction::Left);
            break;
        case VK_RIGHT:
        case 'D':
            hud_changed = game_.set_direction(SnakeGame::Direction::Right);
            break;
        case VK_SPACE:
            if (!repeated)
            {
                game_.toggle_pause();
                hud_changed = true;
            }
            break;
        case 'R':
            if (!repeated)
            {
                game_.reset(true);
                hud_changed = true;
            }
            break;
        default:
            consumed = false;
            break;
        }
        if (hud_changed)
        {
            sync_hud(false);
        }
        return consumed;
    }

private:
    [[nodiscard]] panorama::PanoramaNode* require_hud_node(std::string_view id)
    {
        panorama::PanoramaNode* root = view_.root();
        panorama::PanoramaNode* node = root != nullptr ? root->find_by_id(id) : nullptr;
        if (node == nullptr)
        {
            throw std::runtime_error("Snake HUD is missing panel #" + std::string(id));
        }
        return node;
    }

    void cache_hud_nodes()
    {
        score_label_ = require_hud_node("ScoreValue");
        best_label_ = require_hud_node("BestValue");
        speed_label_ = require_hud_node("SpeedValue");
        status_label_ = require_hud_node("StatusValue");
        pause_button_label_ = require_hud_node("PauseButtonLabel");
        overlay_ = require_hud_node("StateOverlay");
        overlay_title_ = require_hud_node("OverlayTitle");
        overlay_body_ = require_hud_node("OverlayBody");
        overlay_button_label_ = require_hud_node("OverlayButtonLabel");
    }

    void clear_hud_nodes() noexcept
    {
        score_label_ = nullptr;
        best_label_ = nullptr;
        speed_label_ = nullptr;
        status_label_ = nullptr;
        pause_button_label_ = nullptr;
        overlay_ = nullptr;
        overlay_title_ = nullptr;
        overlay_body_ = nullptr;
        overlay_button_label_ = nullptr;
    }

    [[nodiscard]] static bool set_node_text(
        panorama::PanoramaNode* node,
        std::string text,
        bool force)
    {
        if (node == nullptr || (!force && node->text == text))
        {
            return false;
        }
        node->text = std::move(text);
        return true;
    }

    void sync_hud(bool force)
    {
        char score_text[32] = {};
        char best_text[32] = {};
        char speed_text[32] = {};
        std::snprintf(score_text, sizeof(score_text), "%04d", game_.score());
        std::snprintf(best_text, sizeof(best_text), "%04d", game_.best_score());
        std::snprintf(
            speed_text,
            sizeof(speed_text),
            "%.1fx",
            static_cast<double>(game_.speed_multiplier()));

        std::string status;
        std::string pause_button;
        std::string overlay_title;
        std::string overlay_body;
        std::string overlay_button;
        bool overlay_visible = true;
        switch (game_.state())
        {
        case SnakeGame::State::Ready:
            status = "READY";
            pause_button = "START";
            overlay_title = "READY?";
            overlay_body = "Press an arrow key or WASD to move";
            overlay_button = "START GAME";
            break;
        case SnakeGame::State::Running:
            status = "RUNNING";
            pause_button = "PAUSE";
            overlay_title.clear();
            overlay_body.clear();
            overlay_button.clear();
            overlay_visible = false;
            break;
        case SnakeGame::State::Paused:
            status = "PAUSED";
            pause_button = "RESUME";
            overlay_title = "PAUSED";
            overlay_body = "Take a breath. The board is frozen.";
            overlay_button = "RESUME";
            break;
        case SnakeGame::State::GameOver:
            status = "GAME OVER";
            pause_button = "NEW GAME";
            overlay_title = "GAME OVER";
            overlay_body = "Score " + std::to_string(game_.score()) + "  -  press R to retry";
            overlay_button = "PLAY AGAIN";
            break;
        }

        bool text_changed = false;
        text_changed = set_node_text(score_label_, score_text, force) || text_changed;
        text_changed = set_node_text(best_label_, best_text, force) || text_changed;
        text_changed = set_node_text(speed_label_, speed_text, force) || text_changed;
        text_changed = set_node_text(status_label_, std::move(status), force) || text_changed;
        text_changed =
            set_node_text(pause_button_label_, std::move(pause_button), force) || text_changed;
        text_changed =
            set_node_text(overlay_title_, std::move(overlay_title), force) || text_changed;
        text_changed =
            set_node_text(overlay_body_, std::move(overlay_body), force) || text_changed;
        text_changed =
            set_node_text(overlay_button_label_, std::move(overlay_button), force) || text_changed;

        const int visibility = overlay_visible ? 1 : 0;
        const bool visibility_changed =
            overlay_ != nullptr && (force || overlay_->visibility_override != visibility);
        if (visibility_changed)
        {
            overlay_->visibility_override = visibility;
            overlay_->mark_style_dirty();
            view_.invalidate_style();
        }
        else if (text_changed)
        {
            view_.invalidate_layout();
        }
    }

    void apply_pending_action()
    {
        if (pending_action_.empty())
        {
            return;
        }
        const std::string action = std::move(pending_action_);
        pending_action_.clear();
        if (action == "restart")
        {
            game_.reset(true);
        }
        else if (action == "pause")
        {
            game_.toggle_pause();
        }
        else if (action == "primary")
        {
            game_.primary_action();
        }
        else
        {
            return;
        }
        sync_hud(false);
    }

    void render()
    {
        graphics_.render(
            snake_renderer_,
            game_,
            *backend_,
            geometry_cache_,
            view_.draw_list(),
            draw_list_changed_);
        draw_list_changed_ = false;
    }

    D3D12WindowHost graphics_;
    NativeSnakeRenderer snake_renderer_;
    SnakeGame game_;
    std::unique_ptr<panorama_adapters::PanoramaD3D12Backend> backend_;
    panorama::PanoramaGeometryCache geometry_cache_;
    panorama::PanoramaFontAtlas font_atlas_;
    panorama::PanoramaView view_;
    panorama::PanoramaNode* score_label_ = nullptr;
    panorama::PanoramaNode* best_label_ = nullptr;
    panorama::PanoramaNode* speed_label_ = nullptr;
    panorama::PanoramaNode* status_label_ = nullptr;
    panorama::PanoramaNode* pause_button_label_ = nullptr;
    panorama::PanoramaNode* overlay_ = nullptr;
    panorama::PanoramaNode* overlay_title_ = nullptr;
    panorama::PanoramaNode* overlay_body_ = nullptr;
    panorama::PanoramaNode* overlay_button_label_ = nullptr;
    std::string pending_action_;
    FrameClock::time_point last_update_ = FrameClock::now();
    bool draw_list_changed_ = true;
    bool initialized_ = false;
};

struct AppState
{
    PanoramaD3D12App app;
    bool failed = false;
    bool mouse_down = false;
    bool tracking_mouse_leave = false;
    float mouse_x = 0.0F;
    float mouse_y = 0.0F;
};

template <typename Work>
bool guarded(HWND hwnd, AppState& state, Work&& work) noexcept
{
    try
    {
        work();
        return true;
    }
    catch (const std::exception& error)
    {
        if (!state.failed)
        {
            std::fprintf(stderr, "Panorama D3D12 example failed: %s\n", error.what());
        }
        state.failed = true;
        DestroyWindow(hwnd);
        return false;
    }
}

void refresh(HWND hwnd, AppState& state) noexcept
{
    (void)guarded(hwnd, state, [&state]() { state.app.tick(); });
}

panorama::PanoramaKey vk_to_panorama_key(WPARAM vk)
{
    switch (vk)
    {
    case VK_LEFT:   return panorama::PanoramaKey::ArrowLeft;
    case VK_RIGHT:  return panorama::PanoramaKey::ArrowRight;
    case VK_UP:     return panorama::PanoramaKey::ArrowUp;
    case VK_DOWN:   return panorama::PanoramaKey::ArrowDown;
    case VK_HOME:   return panorama::PanoramaKey::Home;
    case VK_END:    return panorama::PanoramaKey::End;
    case VK_BACK:   return panorama::PanoramaKey::Backspace;
    case VK_DELETE: return panorama::PanoramaKey::Delete;
    case VK_RETURN: return panorama::PanoramaKey::Enter;
    case VK_TAB:    return panorama::PanoramaKey::Tab;
    case VK_ESCAPE: return panorama::PanoramaKey::Escape;
    case 'A':       return panorama::PanoramaKey::A;
    default:        return panorama::PanoramaKey::Unknown;
    }
}

int wchar_to_utf8(wchar_t ch, char* out)
{
    if (ch < 0x80)
    {
        out[0] = static_cast<char>(ch);
        return 1;
    }
    if (ch < 0x800)
    {
        out[0] = static_cast<char>(0xC0 | (ch >> 6));
        out[1] = static_cast<char>(0x80 | (ch & 0x3F));
        return 2;
    }
    out[0] = static_cast<char>(0xE0 | (ch >> 12));
    out[1] = static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
    out[2] = static_cast<char>(0x80 | (ch & 0x3F));
    return 3;
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    AppState* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        state = static_cast<AppState*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }

    switch (message)
    {
    case WM_GETMINMAXINFO:
        if (lparam != 0)
        {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
            limits->ptMinTrackSize.x = 760;
            limits->ptMinTrackSize.y = 560;
        }
        return 0;

    case WM_SIZE:
        if (state != nullptr && state->app.initialized() && wparam != SIZE_MINIMIZED)
        {
            const int width = LOWORD(lparam);
            const int height = HIWORD(lparam);
            if (width > 0 && height > 0)
            {
                (void)guarded(hwnd, *state, [&]() { state->app.resize(width, height); });
            }
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(hwnd, &paint);
        EndPaint(hwnd, &paint);
        return 0;
    }

    case WM_MOUSEMOVE:
        if (state != nullptr && state->app.initialized())
        {
            state->mouse_x = static_cast<float>(GET_X_LPARAM(lparam));
            state->mouse_y = static_cast<float>(GET_Y_LPARAM(lparam));
            if (!state->tracking_mouse_leave)
            {
                TRACKMOUSEEVENT tracking{};
                tracking.cbSize = sizeof(tracking);
                tracking.dwFlags = TME_LEAVE;
                tracking.hwndTrack = hwnd;
                state->tracking_mouse_leave = TrackMouseEvent(&tracking) != FALSE;
            }
            (void)state->app.update_pointer(state->mouse_x, state->mouse_y, state->mouse_down);
            refresh(hwnd, *state);
        }
        return 0;

    case WM_LBUTTONDOWN:
        if (state != nullptr && state->app.initialized())
        {
            SetCapture(hwnd);
            state->mouse_down = true;
            state->mouse_x = static_cast<float>(GET_X_LPARAM(lparam));
            state->mouse_y = static_cast<float>(GET_Y_LPARAM(lparam));
            (void)state->app.update_pointer(state->mouse_x, state->mouse_y, true);
            refresh(hwnd, *state);
        }
        return 0;

    case WM_LBUTTONUP:
        if (state != nullptr && state->app.initialized())
        {
            ReleaseCapture();
            state->mouse_down = false;
            state->mouse_x = static_cast<float>(GET_X_LPARAM(lparam));
            state->mouse_y = static_cast<float>(GET_Y_LPARAM(lparam));
            (void)state->app.update_pointer(state->mouse_x, state->mouse_y, false);
            refresh(hwnd, *state);
        }
        return 0;

    case WM_MOUSEWHEEL:
        if (state != nullptr && state->app.initialized())
        {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ScreenToClient(hwnd, &point);
            state->mouse_x = static_cast<float>(point.x);
            state->mouse_y = static_cast<float>(point.y);
            const float wheel_ticks =
                static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / static_cast<float>(WHEEL_DELTA);
            (void)state->app.update_wheel(state->mouse_x, state->mouse_y, wheel_ticks);
            refresh(hwnd, *state);
        }
        return 0;

    case WM_KEYDOWN:
        if (state != nullptr && state->app.initialized())
        {
            if (wparam == VK_ESCAPE)
            {
                DestroyWindow(hwnd);
                return 0;
            }
            const bool repeated =
                (static_cast<std::uint64_t>(lparam) & (1ULL << 30U)) != 0;
            if (state->app.handle_game_key(wparam, repeated))
            {
                refresh(hwnd, *state);
                return 0;
            }
            const panorama::PanoramaKey key = vk_to_panorama_key(wparam);
            if (key != panorama::PanoramaKey::Unknown)
            {
                panorama::PanoramaKeyEvent event;
                event.key = key;
                event.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                event.ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                event.alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
                (void)state->app.handle_key_down(event);
                refresh(hwnd, *state);
            }
        }
        return 0;

    case WM_CHAR:
        if (state != nullptr && state->app.initialized())
        {
            const wchar_t ch = static_cast<wchar_t>(wparam);
            if (ch >= 0x20 && ch != 0x7F && ch != L'\r')
            {
                char utf8[4] = {};
                const int length = wchar_to_utf8(ch, utf8);
                (void)state->app.handle_text_input(std::string_view(utf8, static_cast<std::size_t>(length)));
                refresh(hwnd, *state);
            }
        }
        return 0;

    case WM_MOUSELEAVE:
        if (state != nullptr && state->app.initialized())
        {
            state->tracking_mouse_leave = false;
            state->mouse_x = -1.0F;
            state->mouse_y = -1.0F;
            (void)state->app.update_pointer(state->mouse_x, state->mouse_y, state->mouse_down);
            refresh(hwnd, *state);
        }
        return 0;

    case WM_TIMER:
        if (state != nullptr && state->app.initialized() && wparam == kFrameTimerId)
        {
            refresh(hwnd, *state);
        }
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, kFrameTimerId);
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}
}

int main(int argc, char** argv)
{
    bool force_warp = false;
    int path_argument = 1;
    if (argc > 1 && std::string_view(argv[1]) == "--warp")
    {
        force_warp = true;
        path_argument = 2;
    }

    // Example 06 carries its own Panorama HUD assets. Supplying another layout
    // remains useful for adapter experiments, although the default snake host
    // expects the documented HUD panel ids to be present.
    const std::filesystem::path xml_path = argc > path_argument
        ? std::filesystem::path(argv[path_argument])
        : std::filesystem::path("../../../examples/06_d3d12_snake/sample/snake.xml");
    if (argc <= path_argument)
    {
        std::printf(
            "usage: %s [--warp] [layout.xml]  (no path given, trying %ls)\n",
            argv[0], xml_path.c_str());
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = kWindowClassName;
    if (RegisterClassExW(&window_class) == 0)
    {
        std::fprintf(stderr, "failed to register the window class\n");
        return 1;
    }

    RECT window_rect{0, 0, kInitialWidth, kInitialHeight};
    AdjustWindowRect(&window_rect, WS_OVERLAPPEDWINDOW, FALSE);

    AppState state;
    HWND hwnd = CreateWindowExW(
        0,
        kWindowClassName,
        L"PanoramaEngine - Native D3D12 Snake",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        window_rect.right - window_rect.left,
        window_rect.bottom - window_rect.top,
        nullptr,
        nullptr,
        instance,
        &state);
    if (hwnd == nullptr)
    {
        std::fprintf(stderr, "failed to create the window\n");
        return 1;
    }

    RECT client_rect{};
    GetClientRect(hwnd, &client_rect);
    const int width = std::max(client_rect.right - client_rect.left, 1L);
    const int height = std::max(client_rect.bottom - client_rect.top, 1L);
    try
    {
        state.app.initialize(hwnd, xml_path, width, height, force_warp);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Panorama D3D12 initialization failed: %s\n", error.what());
        DestroyWindow(hwnd);
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);
    if (SetTimer(hwnd, kFrameTimerId, kFrameIntervalMilliseconds, nullptr) == 0)
    {
        std::fprintf(stderr, "failed to create the frame timer\n");
        DestroyWindow(hwnd);
        return 1;
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    state.app.shutdown();
    return state.failed ? 1 : static_cast<int>(message.wParam);
}
