#include "ui/panorama/panorama_log.hpp"

#include <cstdio>
#include <utility>

namespace openstrike
{
namespace
{
PanoramaLogSink& sink()
{
    static PanoramaLogSink instance;
    return instance;
}
}

void set_panorama_log_sink(PanoramaLogSink new_sink)
{
    sink() = std::move(new_sink);
}

void panorama_log_emit(PanoramaLogLevel level, std::string_view message)
{
    if (sink())
    {
        sink()(level, message);
        return;
    }

    std::FILE* stream = level == PanoramaLogLevel::Warning ? stderr : stdout;
    std::fprintf(stream, "[panorama] %.*s\n", static_cast<int>(message.size()), message.data());
}
}
