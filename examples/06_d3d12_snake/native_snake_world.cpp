#include "native_snake_world.hpp"

#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace native_snake
{
using Microsoft::WRL::ComPtr;

constexpr std::uint32_t kNativeFrameCount = 2;
constexpr DXGI_FORMAT kNativeBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

[[nodiscard]] inline std::string native_hr_to_hex(HRESULT hr)
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

inline void check_native_hr(HRESULT hr, std::string_view operation)
{
    if (FAILED(hr))
    {
        throw std::runtime_error(
            std::string(operation) + " failed (HRESULT 0x" + native_hr_to_hex(hr) + ")");
    }
}

struct Cell
{
    int x = 0;
    int y = 0;

    [[nodiscard]] bool operator==(const Cell&) const noexcept = default;
};

class SnakeLevel
{
public:
    static constexpr int kColumns = 28;
    static constexpr int kRows = 20;

    [[nodiscard]] constexpr int columns() const noexcept { return kColumns; }
    [[nodiscard]] constexpr int rows() const noexcept { return kRows; }
    [[nodiscard]] constexpr std::size_t cell_count() const noexcept
    {
        return static_cast<std::size_t>(kColumns * kRows);
    }
    [[nodiscard]] constexpr bool contains(Cell cell) const noexcept
    {
        return cell.x >= 0 && cell.x < kColumns && cell.y >= 0 && cell.y < kRows;
    }
};

class SnakeGameState
{
public:
    using Direction = SnakeGame::Direction;
    using State = SnakeGame::State;

    SnakeGameState() { reset(false); }

    void reset(bool start_running)
    {
        snake_.clear();
        const int start_x = level_.columns() / 2 + 1;
        const int start_y = level_.rows() / 2;
        for (int offset = 0; offset < 4; ++offset)
        {
            snake_.push_back({start_x - offset, start_y});
        }
        direction_ = Direction::Right;
        queued_direction_ = direction_;
        score_ = 0;
        accumulator_ = 0.0F;
        state_ = start_running ? State::Running : State::Ready;
        spawn_food();
    }

    void primary_action()
    {
        switch (state_)
        {
        case State::Ready:
            state_ = State::Running;
            break;
        case State::Running:
            state_ = State::Paused;
            break;
        case State::Paused:
            state_ = State::Running;
            break;
        case State::GameOver:
            reset(true);
            break;
        }
        accumulator_ = 0.0F;
    }

    void toggle_pause()
    {
        if (state_ == State::GameOver)
        {
            reset(true);
            return;
        }
        primary_action();
    }

    [[nodiscard]] bool set_direction(Direction direction)
    {
        if (state_ == State::GameOver || state_ == State::Paused ||
            is_opposite(direction, direction_))
        {
            return false;
        }
        queued_direction_ = direction;
        if (state_ == State::Ready)
        {
            state_ = State::Running;
            accumulator_ = 0.0F;
            return true;
        }
        return false;
    }

    // Returns true when a score or state change requires a HUD update.
    [[nodiscard]] bool update(float dt_seconds)
    {
        if (state_ != State::Running)
        {
            return false;
        }

        bool hud_changed = false;
        accumulator_ += std::clamp(dt_seconds, 0.0F, 0.1F);
        const float interval = step_interval();
        while (accumulator_ >= interval && state_ == State::Running)
        {
            accumulator_ -= interval;
            hud_changed = advance_one_cell() || hud_changed;
        }
        return hud_changed;
    }

    [[nodiscard]] const std::deque<Cell>& snake() const noexcept { return snake_; }
    [[nodiscard]] Cell food() const noexcept { return food_; }
    [[nodiscard]] const SnakeLevel& level() const noexcept { return level_; }
    [[nodiscard]] Direction direction() const noexcept { return direction_; }
    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] int score() const noexcept { return score_; }
    [[nodiscard]] int best_score() const noexcept { return best_score_; }
    [[nodiscard]] float speed_multiplier() const noexcept
    {
        return 0.135F / step_interval();
    }

private:
    [[nodiscard]] static bool is_opposite(Direction lhs, Direction rhs) noexcept
    {
        return
            (lhs == Direction::Up && rhs == Direction::Down) ||
            (lhs == Direction::Down && rhs == Direction::Up) ||
            (lhs == Direction::Left && rhs == Direction::Right) ||
            (lhs == Direction::Right && rhs == Direction::Left);
    }

    [[nodiscard]] float step_interval() const noexcept
    {
        const float acceleration = static_cast<float>(score_ / 10) * 0.0035F;
        return std::max(0.055F, 0.135F - acceleration);
    }

    [[nodiscard]] bool advance_one_cell()
    {
        direction_ = queued_direction_;
        Cell head = snake_.front();
        switch (direction_)
        {
        case Direction::Up:    --head.y; break;
        case Direction::Down:  ++head.y; break;
        case Direction::Left:  --head.x; break;
        case Direction::Right: ++head.x; break;
        }

        const bool hits_wall = !level_.contains(head);
        const bool eats_food = head == food_;
        const std::size_t checked_segments =
            snake_.size() - (eats_food ? 0U : 1U);
        bool hits_self = false;
        for (std::size_t index = 0; index < checked_segments; ++index)
        {
            if (snake_[index] == head)
            {
                hits_self = true;
                break;
            }
        }
        if (hits_wall || hits_self)
        {
            state_ = State::GameOver;
            best_score_ = std::max(best_score_, score_);
            return true;
        }

        snake_.push_front(head);
        if (!eats_food)
        {
            snake_.pop_back();
            return false;
        }

        score_ += 10;
        best_score_ = std::max(best_score_, score_);
        spawn_food();
        return true;
    }

    void spawn_food()
    {
        std::vector<Cell> open_cells;
        open_cells.reserve(level_.cell_count() - snake_.size());
        for (int y = 0; y < level_.rows(); ++y)
        {
            for (int x = 0; x < level_.columns(); ++x)
            {
                const Cell candidate{x, y};
                if (std::find(snake_.begin(), snake_.end(), candidate) == snake_.end())
                {
                    open_cells.push_back(candidate);
                }
            }
        }
        if (open_cells.empty())
        {
            state_ = State::GameOver;
            return;
        }
        std::uniform_int_distribution<std::size_t> distribution(0, open_cells.size() - 1);
        food_ = open_cells[distribution(random_)];
    }

    SnakeLevel level_;
    std::deque<Cell> snake_;
    Cell food_{};
    Direction direction_ = Direction::Right;
    Direction queued_direction_ = Direction::Right;
    State state_ = State::Ready;
    int score_ = 0;
    int best_score_ = 0;
    float accumulator_ = 0.0F;
    std::mt19937 random_{std::random_device{}()};
};

class NativeSnakeRendererImpl
{
public:
    ~NativeSnakeRendererImpl()
    {
        for (UINT index = 0; index < kNativeFrameCount; ++index)
        {
            if (vertex_buffers_[index] != nullptr && mapped_vertices_[index] != nullptr)
            {
                vertex_buffers_[index]->Unmap(0, nullptr);
                mapped_vertices_[index] = nullptr;
            }
        }
    }

    NativeSnakeRendererImpl() = default;
    NativeSnakeRendererImpl(const NativeSnakeRendererImpl&) = delete;
    NativeSnakeRendererImpl& operator=(const NativeSnakeRendererImpl&) = delete;

    void initialize(ID3D12Device* device)
    {
        static constexpr char shader_source[] = R"(
struct VSInput
{
    float2 position : POSITION;
    float4 color : COLOR;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float4 color : COLOR;
};

PSInput vs_main(VSInput input)
{
    PSInput output;
    output.position = float4(input.position, 0.0, 1.0);
    output.color = input.color;
    return output;
}

float4 ps_main(PSInput input) : SV_TARGET
{
    return input.color;
}
)";

        const ComPtr<ID3DBlob> vertex_shader =
            compile_shader(shader_source, "vs_main", "vs_5_0");
        const ComPtr<ID3DBlob> pixel_shader =
            compile_shader(shader_source, "ps_main", "ps_5_0");

        D3D12_ROOT_SIGNATURE_DESC root_desc{};
        root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> serialized_root;
        ComPtr<ID3DBlob> root_errors;
        const HRESULT serialize_hr = D3D12SerializeRootSignature(
            &root_desc, D3D_ROOT_SIGNATURE_VERSION_1,
            &serialized_root, &root_errors);
        if (FAILED(serialize_hr))
        {
            const std::string details = root_errors != nullptr
                ? std::string(
                    static_cast<const char*>(root_errors->GetBufferPointer()),
                    root_errors->GetBufferSize())
                : std::string();
            throw std::runtime_error(
                "D3D12SerializeRootSignature failed (HRESULT 0x" +
                native_hr_to_hex(serialize_hr) + "): " + details);
        }
        check_native_hr(
            device->CreateRootSignature(
                0,
                serialized_root->GetBufferPointer(),
                serialized_root->GetBufferSize(),
                IID_PPV_ARGS(&root_signature_)),
            "CreateRootSignature(native snake)");

        const D3D12_INPUT_ELEMENT_DESC input_elements[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex, x),
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, offsetof(Vertex, color),
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline_desc{};
        pipeline_desc.pRootSignature = root_signature_.Get();
        pipeline_desc.VS = {
            vertex_shader->GetBufferPointer(),
            vertex_shader->GetBufferSize(),
        };
        pipeline_desc.PS = {
            pixel_shader->GetBufferPointer(),
            pixel_shader->GetBufferSize(),
        };
        pipeline_desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
            D3D12_COLOR_WRITE_ENABLE_ALL;
        pipeline_desc.SampleMask = std::numeric_limits<UINT>::max();
        pipeline_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pipeline_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pipeline_desc.RasterizerState.DepthClipEnable = TRUE;
        pipeline_desc.DepthStencilState.DepthEnable = FALSE;
        pipeline_desc.DepthStencilState.StencilEnable = FALSE;
        pipeline_desc.InputLayout = {input_elements, static_cast<UINT>(std::size(input_elements))};
        pipeline_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pipeline_desc.NumRenderTargets = 1;
        pipeline_desc.RTVFormats[0] = kNativeBackBufferFormat;
        pipeline_desc.SampleDesc.Count = 1;
        check_native_hr(
            device->CreateGraphicsPipelineState(&pipeline_desc, IID_PPV_ARGS(&pipeline_)),
            "CreateGraphicsPipelineState(native snake)");

        const UINT64 buffer_size = static_cast<UINT64>(kMaxVertices * sizeof(Vertex));
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = buffer_size;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        for (UINT index = 0; index < kNativeFrameCount; ++index)
        {
            check_native_hr(
                device->CreateCommittedResource(
                    &heap,
                    D3D12_HEAP_FLAG_NONE,
                    &buffer,
                    D3D12_RESOURCE_STATE_GENERIC_READ,
                    nullptr,
                    IID_PPV_ARGS(&vertex_buffers_[index])),
                "CreateCommittedResource(native snake vertex buffer)");
            D3D12_RANGE no_read{0, 0};
            check_native_hr(
                vertex_buffers_[index]->Map(
                    0, &no_read, reinterpret_cast<void**>(&mapped_vertices_[index])),
                "Map(native snake vertex buffer)");
        }
        vertices_.reserve(kMaxVertices);
    }

    void record(
        ID3D12GraphicsCommandList* command_list,
        UINT frame_index,
        int viewport_width,
        int viewport_height,
        const LevelViewport& level_viewport,
        const SnakeGameState& game)
    {
        vertices_.clear();
        build_board(viewport_width, viewport_height, level_viewport, game);
        if (vertices_.size() > kMaxVertices)
        {
            throw std::runtime_error("native snake vertex capacity exceeded");
        }

        std::memcpy(
            mapped_vertices_[frame_index],
            vertices_.data(),
            vertices_.size() * sizeof(Vertex));

        const D3D12_VIEWPORT viewport{
            0.0F,
            0.0F,
            static_cast<float>(viewport_width),
            static_cast<float>(viewport_height),
            0.0F,
            1.0F,
        };
        const D3D12_RECT scissor{
            0,
            0,
            static_cast<LONG>(viewport_width),
            static_cast<LONG>(viewport_height),
        };
        const D3D12_VERTEX_BUFFER_VIEW vertex_view{
            vertex_buffers_[frame_index]->GetGPUVirtualAddress(),
            static_cast<UINT>(vertices_.size() * sizeof(Vertex)),
            sizeof(Vertex),
        };

        command_list->SetGraphicsRootSignature(root_signature_.Get());
        command_list->SetPipelineState(pipeline_.Get());
        command_list->RSSetViewports(1, &viewport);
        command_list->RSSetScissorRects(1, &scissor);
        command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        command_list->IASetVertexBuffers(0, 1, &vertex_view);
        command_list->DrawInstanced(static_cast<UINT>(vertices_.size()), 1, 0, 0);
    }

private:
    struct Vertex
    {
        float x = 0.0F;
        float y = 0.0F;
        std::uint32_t color = 0;
    };

    static constexpr std::size_t kMaxVertices = 12'288;

    [[nodiscard]] static ComPtr<ID3DBlob> compile_shader(
        const char* source,
        const char* entry,
        const char* target)
    {
        ComPtr<ID3DBlob> shader;
        ComPtr<ID3DBlob> errors;
        const HRESULT hr = D3DCompile(
            source,
            std::strlen(source),
            "native_snake_shader",
            nullptr,
            nullptr,
            entry,
            target,
            D3DCOMPILE_ENABLE_STRICTNESS,
            0,
            &shader,
            &errors);
        if (FAILED(hr))
        {
            const std::string details = errors != nullptr
                ? std::string(
                    static_cast<const char*>(errors->GetBufferPointer()),
                    errors->GetBufferSize())
                : std::string();
            throw std::runtime_error(
                std::string("D3DCompile(") + entry + ") failed (HRESULT 0x" +
                native_hr_to_hex(hr) + "): " + details);
        }
        return shader;
    }

    [[nodiscard]] static constexpr std::uint32_t rgba(
        std::uint8_t red,
        std::uint8_t green,
        std::uint8_t blue,
        std::uint8_t alpha = 255) noexcept
    {
        return
            static_cast<std::uint32_t>(red) |
            (static_cast<std::uint32_t>(green) << 8U) |
            (static_cast<std::uint32_t>(blue) << 16U) |
            (static_cast<std::uint32_t>(alpha) << 24U);
    }

    void add_rect(
        float left,
        float top,
        float right,
        float bottom,
        std::uint32_t color,
        float viewport_width,
        float viewport_height)
    {
        const float x0 = left * 2.0F / viewport_width - 1.0F;
        const float x1 = right * 2.0F / viewport_width - 1.0F;
        const float y0 = 1.0F - top * 2.0F / viewport_height;
        const float y1 = 1.0F - bottom * 2.0F / viewport_height;
        vertices_.insert(vertices_.end(), {
            {x0, y0, color},
            {x1, y0, color},
            {x1, y1, color},
            {x0, y0, color},
            {x1, y1, color},
            {x0, y1, color},
        });
    }

    void build_board(
        int width,
        int height,
        const LevelViewport& level_viewport,
        const SnakeGameState& game)
    {
        const float viewport_width = static_cast<float>(std::max(width, 1));
        const float viewport_height = static_cast<float>(std::max(height, 1));
        const float level_left =
            std::clamp(level_viewport.left, 0.0F, std::max(viewport_width - 1.0F, 0.0F));
        const float level_top =
            std::clamp(level_viewport.top, 0.0F, std::max(viewport_height - 1.0F, 0.0F));
        const float level_right =
            std::clamp(level_viewport.right, level_left + 1.0F, viewport_width);
        const float level_bottom =
            std::clamp(level_viewport.bottom, level_top + 1.0F, viewport_height);
        const float available_width = std::max(level_right - level_left - 64.0F, 2.0F);
        const float available_height = std::max(level_bottom - level_top - 28.0F, 2.0F);
        const SnakeLevel& level = game.level();
        const float cell_size = std::max(
            2.0F,
            std::floor(std::min(
                available_width / static_cast<float>(level.columns()),
                available_height / static_cast<float>(level.rows()))));
        const float board_width = cell_size * static_cast<float>(level.columns());
        const float board_height = cell_size * static_cast<float>(level.rows());
        const float board_left =
            level_left + (level_right - level_left - board_width) * 0.5F;
        const float board_top =
            level_top + (level_bottom - level_top - board_height) * 0.5F;

        add_rect(
            board_left - 10.0F,
            board_top - 8.0F,
            board_left + board_width + 12.0F,
            board_top + board_height + 14.0F,
            rgba(0, 0, 0),
            viewport_width,
            viewport_height);
        add_rect(
            board_left - 3.0F,
            board_top - 3.0F,
            board_left + board_width + 3.0F,
            board_top + board_height + 3.0F,
            rgba(38, 78, 80),
            viewport_width,
            viewport_height);
        add_rect(
            board_left,
            board_top,
            board_left + board_width,
            board_top + board_height,
            rgba(8, 20, 26),
            viewport_width,
            viewport_height);

        for (int x = 1; x < level.columns(); ++x)
        {
            const float line_x = board_left + static_cast<float>(x) * cell_size;
            add_rect(
                line_x,
                board_top,
                line_x + 1.0F,
                board_top + board_height,
                rgba(16, 42, 48),
                viewport_width,
                viewport_height);
        }
        for (int y = 1; y < level.rows(); ++y)
        {
            const float line_y = board_top + static_cast<float>(y) * cell_size;
            add_rect(
                board_left,
                line_y,
                board_left + board_width,
                line_y + 1.0F,
                rgba(16, 42, 48),
                viewport_width,
                viewport_height);
        }

        const auto cell_rect = [=](Cell cell, float inset) {
            const float left = board_left + static_cast<float>(cell.x) * cell_size + inset;
            const float top = board_top + static_cast<float>(cell.y) * cell_size + inset;
            return std::array<float, 4>{
                left,
                top,
                board_left + static_cast<float>(cell.x + 1) * cell_size - inset,
                board_top + static_cast<float>(cell.y + 1) * cell_size - inset,
            };
        };

        const std::array<float, 4> food = cell_rect(game.food(), cell_size * 0.18F);
        add_rect(
            food[0] + 2.0F, food[1] + 2.0F, food[2] + 2.0F, food[3] + 2.0F,
            rgba(70, 12, 22), viewport_width, viewport_height);
        add_rect(
            food[0], food[1], food[2], food[3],
            rgba(239, 71, 92), viewport_width, viewport_height);
        add_rect(
            food[0] + cell_size * 0.12F,
            food[1] + cell_size * 0.10F,
            food[0] + cell_size * 0.30F,
            food[1] + cell_size * 0.28F,
            rgba(255, 186, 174),
            viewport_width,
            viewport_height);
        add_rect(
            food[0] + cell_size * 0.48F,
            food[1] - cell_size * 0.20F,
            food[0] + cell_size * 0.60F,
            food[1] + cell_size * 0.08F,
            rgba(112, 184, 92),
            viewport_width,
            viewport_height);

        const std::deque<Cell>& snake = game.snake();
        for (std::size_t reverse_index = snake.size(); reverse_index-- > 0;)
        {
            const Cell segment = snake[reverse_index];
            const std::array<float, 4> rect = cell_rect(segment, cell_size * 0.10F);
            const float head_weight = snake.size() > 1
                ? 1.0F - static_cast<float>(reverse_index) /
                    static_cast<float>(snake.size() - 1)
                : 1.0F;
            const auto green = static_cast<std::uint8_t>(145.0F + 70.0F * head_weight);
            const auto blue = static_cast<std::uint8_t>(86.0F + 42.0F * head_weight);
            add_rect(
                rect[0], rect[1], rect[2], rect[3],
                rgba(54, green, blue), viewport_width, viewport_height);
            add_rect(
                rect[0] + cell_size * 0.10F,
                rect[1] + cell_size * 0.10F,
                rect[2] - cell_size * 0.10F,
                rect[1] + cell_size * 0.22F,
                rgba(137, 242, 151),
                viewport_width,
                viewport_height);
        }

        const std::array<float, 4> head = cell_rect(snake.front(), cell_size * 0.10F);
        const float eye_size = std::max(cell_size * 0.11F, 2.0F);
        const float eye_a_x =
            game.direction() == SnakeGameState::Direction::Left ? head[0] + cell_size * 0.12F :
            game.direction() == SnakeGameState::Direction::Right ? head[2] - cell_size * 0.22F :
            head[0] + cell_size * 0.18F;
        const float eye_b_x =
            game.direction() == SnakeGameState::Direction::Left ? head[0] + cell_size * 0.12F :
            game.direction() == SnakeGameState::Direction::Right ? head[2] - cell_size * 0.22F :
            head[2] - cell_size * 0.29F;
        const float eye_a_y =
            game.direction() == SnakeGameState::Direction::Up ? head[1] + cell_size * 0.10F :
            game.direction() == SnakeGameState::Direction::Down ? head[3] - cell_size * 0.21F :
            head[1] + cell_size * 0.17F;
        const float eye_b_y =
            game.direction() == SnakeGameState::Direction::Up ? head[1] + cell_size * 0.10F :
            game.direction() == SnakeGameState::Direction::Down ? head[3] - cell_size * 0.21F :
            head[3] - cell_size * 0.28F;
        add_rect(
            eye_a_x, eye_a_y, eye_a_x + eye_size, eye_a_y + eye_size,
            rgba(5, 18, 20), viewport_width, viewport_height);
        add_rect(
            eye_b_x, eye_b_y, eye_b_x + eye_size, eye_b_y + eye_size,
            rgba(5, 18, 20), viewport_width, viewport_height);
    }

    ComPtr<ID3D12RootSignature> root_signature_;
    ComPtr<ID3D12PipelineState> pipeline_;
    std::array<ComPtr<ID3D12Resource>, kNativeFrameCount> vertex_buffers_;
    std::array<Vertex*, kNativeFrameCount> mapped_vertices_{};
    std::vector<Vertex> vertices_;
};
struct SnakeGame::Impl
{
    SnakeGameState game;
};

SnakeGame::SnakeGame()
    : impl_(std::make_unique<Impl>())
{
}

SnakeGame::~SnakeGame() = default;

void SnakeGame::reset(bool start_running)
{
    impl_->game.reset(start_running);
}

void SnakeGame::primary_action()
{
    impl_->game.primary_action();
}

void SnakeGame::toggle_pause()
{
    impl_->game.toggle_pause();
}

bool SnakeGame::set_direction(Direction direction)
{
    return impl_->game.set_direction(direction);
}

bool SnakeGame::update(float dt_seconds)
{
    return impl_->game.update(dt_seconds);
}

SnakeGame::State SnakeGame::state() const noexcept
{
    return impl_->game.state();
}

int SnakeGame::score() const noexcept
{
    return impl_->game.score();
}

int SnakeGame::best_score() const noexcept
{
    return impl_->game.best_score();
}

float SnakeGame::speed_multiplier() const noexcept
{
    return impl_->game.speed_multiplier();
}

struct NativeSnakeRenderer::Impl
{
    NativeSnakeRendererImpl renderer;
};

NativeSnakeRenderer::NativeSnakeRenderer()
    : impl_(std::make_unique<Impl>())
{
}

NativeSnakeRenderer::~NativeSnakeRenderer() = default;

void NativeSnakeRenderer::initialize(ID3D12Device* device)
{
    impl_->renderer.initialize(device);
}

void NativeSnakeRenderer::record(
    ID3D12GraphicsCommandList* command_list,
    std::uint32_t frame_index,
    int viewport_width,
    int viewport_height,
    const LevelViewport& level_viewport,
    const SnakeGame& game)
{
    impl_->renderer.record(
        command_list,
        static_cast<UINT>(frame_index),
        viewport_width,
        viewport_height,
        level_viewport,
        game.impl_->game);
}
}
