#include "ui/panorama/panorama_log.hpp"

#include <atomic>
#include <cstdio>
#include <utility>

namespace panorama
{
namespace
{
PanoramaLogSink& sink()
{
    static PanoramaLogSink instance;
    return instance;
}

std::atomic<PanoramaLogLevel>& minimum_level()
{
    static std::atomic<PanoramaLogLevel> instance{PanoramaLogLevel::Info};
    return instance;
}
}

void set_panorama_log_sink(PanoramaLogSink new_sink)
{
    sink() = std::move(new_sink);
}

void set_panorama_log_level(PanoramaLogLevel level) noexcept
{
    minimum_level().store(level, std::memory_order_relaxed);
}

bool panorama_log_enabled(PanoramaLogLevel level) noexcept
{
    return static_cast<int>(level) >= static_cast<int>(minimum_level().load(std::memory_order_relaxed));
}

void panorama_log_emit(PanoramaLogLevel level, std::string_view message)
{
    if (!panorama_log_enabled(level))
    {
        return;
    }
    if (sink())
    {
        sink()(level, message);
        return;
    }

    std::FILE* stream = level == PanoramaLogLevel::Warning ? stderr : stdout;
    std::fprintf(stream, "[panorama] %.*s\n", static_cast<int>(message.size()), message.data());
}
}
