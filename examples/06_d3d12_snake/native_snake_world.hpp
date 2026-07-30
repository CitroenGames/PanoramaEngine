#pragma once

#include <cstdint>
#include <memory>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;

namespace native_snake
{
struct LevelViewport
{
    float left = 0.0F;
    float top = 0.0F;
    float right = 1.0F;
    float bottom = 1.0F;
};

class NativeSnakeRenderer;

class SnakeGame final
{
public:
    enum class Direction
    {
        Up,
        Down,
        Left,
        Right,
    };

    enum class State
    {
        Ready,
        Running,
        Paused,
        GameOver,
    };

    SnakeGame();
    ~SnakeGame();

    SnakeGame(const SnakeGame&) = delete;
    SnakeGame& operator=(const SnakeGame&) = delete;

    void reset(bool start_running);
    void primary_action();
    void toggle_pause();
    [[nodiscard]] bool set_direction(Direction direction);
    [[nodiscard]] bool update(float dt_seconds);

    [[nodiscard]] State state() const noexcept;
    [[nodiscard]] int score() const noexcept;
    [[nodiscard]] int best_score() const noexcept;
    [[nodiscard]] float speed_multiplier() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    friend class NativeSnakeRenderer;
};

class NativeSnakeRenderer final
{
public:
    NativeSnakeRenderer();
    ~NativeSnakeRenderer();

    NativeSnakeRenderer(const NativeSnakeRenderer&) = delete;
    NativeSnakeRenderer& operator=(const NativeSnakeRenderer&) = delete;

    void initialize(ID3D12Device* device);
    void record(
        ID3D12GraphicsCommandList* command_list,
        std::uint32_t frame_index,
        int viewport_width,
        int viewport_height,
        const LevelViewport& level_viewport,
        const SnakeGame& game);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
