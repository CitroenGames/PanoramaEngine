#include "ui/panorama/panorama_diagnostics.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

namespace panorama
{
namespace
{
bool environment_switch_enabled(const char* name)
{
#if defined(_MSC_VER)
    char* raw = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&raw, &size, name) != 0 || raw == nullptr)
    {
        return false;
    }
    std::string value(raw);
    std::free(raw);
#else
    const char* raw = std::getenv(name);
    if (raw == nullptr)
    {
        return false;
    }
    std::string value(raw);
#endif
    if (value.empty())
    {
        return false;
    }
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value != "0" && value != "false" && value != "off";
}

PanoramaDiagnostics& active_diagnostics()
{
    static PanoramaDiagnostics diagnostics = panorama_diagnostics_from_environment();
    return diagnostics;
}
}

void set_panorama_diagnostics(PanoramaDiagnostics diagnostics)
{
    active_diagnostics() = diagnostics;
}

PanoramaDiagnostics panorama_diagnostics()
{
    return active_diagnostics();
}

PanoramaDiagnostics panorama_diagnostics_from_environment()
{
    return PanoramaDiagnostics{
        .tree_guard = environment_switch_enabled("PANORAMA_TREE_GUARD"),
        .disable_style_index = environment_switch_enabled("PANORAMA_DISABLE_STYLE_INDEX"),
        .disable_style_sharing = environment_switch_enabled("PANORAMA_DISABLE_STYLE_SHARING"),
    };
}
}
