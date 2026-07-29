#pragma once

#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

// Standalone logging for the Panorama engine. The engine is a self-contained
// thirdparty library and must not depend on the host application's logger, so it
// emits through a sink the host can install. Without a sink, messages go to
// stderr. This keeps the dependency direction host -> engine only.
namespace panorama
{
enum class PanoramaLogLevel
{
    Info,
    Warning,
};

using PanoramaLogSink = std::function<void(PanoramaLogLevel level, std::string_view message)>;

// Installs the sink that receives engine log messages (pass nullptr to reset to
// the default stderr sink). Typically wired by the host to its own logger.
void set_panorama_log_sink(PanoramaLogSink sink);

// Sets the least-severe level that is emitted. Info is the compatibility
// default; selecting Warning suppresses informational messages before their
// format arguments are evaluated by std::format.
void set_panorama_log_level(PanoramaLogLevel minimum_level) noexcept;
[[nodiscard]] bool panorama_log_enabled(PanoramaLogLevel level) noexcept;

// Emits a fully-formatted message to the active sink.
void panorama_log_emit(PanoramaLogLevel level, std::string_view message);

template <class... Args>
void pano_log_info(std::format_string<Args...> fmt, Args&&... args)
{
    if (!panorama_log_enabled(PanoramaLogLevel::Info))
    {
        return;
    }
    panorama_log_emit(PanoramaLogLevel::Info, std::format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void pano_log_warning(std::format_string<Args...> fmt, Args&&... args)
{
    if (!panorama_log_enabled(PanoramaLogLevel::Warning))
    {
        return;
    }
    panorama_log_emit(PanoramaLogLevel::Warning, std::format(fmt, std::forward<Args>(args)...));
}
}
