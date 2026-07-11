#include "ui/panorama/panorama_render_backend.hpp"

namespace panorama
{
namespace
{
PanoramaRenderBackend* g_panorama_render_backend = nullptr;
}

PanoramaRenderBackend* panorama_render_backend()
{
    return g_panorama_render_backend;
}

void set_panorama_render_backend(PanoramaRenderBackend* backend)
{
    g_panorama_render_backend = backend;
}
}
