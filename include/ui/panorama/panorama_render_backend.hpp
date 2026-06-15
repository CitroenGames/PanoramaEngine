#pragma once

#include "ui/panorama/panorama_paint.hpp"

#include <cstdint>
#include <span>
#include <string_view>

// The GPU contract a host implements to display Panorama draw lists. The engine
// itself never talks to a graphics API: it produces PanoramaDrawList batches and
// the host feeds them through this interface (compile once, render per frame).
// Every hook except texture/geometry management has a safe no-op default, so a
// minimal backend only needs textures + geometry to get pixels on screen.
namespace openstrike
{
using PanoramaCompiledGeometryHandle = std::uintptr_t;

class PanoramaRenderBackend
{
public:
    virtual ~PanoramaRenderBackend() = default;

    virtual PanoramaTextureId generate_texture(std::span<const unsigned char> rgba, int width, int height) = 0;
    virtual PanoramaTextureId load_texture(std::string_view, int& width, int& height)
    {
        width = 0;
        height = 0;
        return 0;
    }
    virtual void release_texture(PanoramaTextureId texture) = 0;

    // Sets (or clears) the scissor rectangle applied to subsequent render_geometry
    // calls, in framebuffer pixels. Used for Panorama `overflow` clipping. Default
    // no-op so backends without scissor support simply do not clip.
    virtual void set_scissor(bool /*enabled*/, int /*x*/, int /*y*/, int /*width*/, int /*height*/) {}

    // Selects the compositing mode for subsequent render_geometry calls (Panorama
    // -mix-blend-mode). Default no-op so backends without blend-mode support render
    // everything with their normal (alpha-over) blend.
    virtual void set_blend_mode(PanoramaBlendMode /*mode*/) {}

    // Backdrop blur: gaussian-blurs everything already rendered this frame inside
    // the given framebuffer-pixel rect (Panorama `blur: gaussian/fastgaussian` on
    // a panel — CS:GO's frosted submenu backgrounds). std deviations are in
    // framebuffer pixels. Default no-op for backends without blur support.
    virtual void blur_region(float /*x*/, float /*y*/, float /*width*/, float /*height*/,
        float /*std_x*/, float /*std_y*/, int /*passes*/) {}

    virtual PanoramaCompiledGeometryHandle compile_geometry(
        std::span<const PanoramaPaintVertex> vertices,
        std::span<const int> indices,
        float ui_scale) = 0;
    virtual void render_geometry(PanoramaCompiledGeometryHandle geometry, PanoramaTextureId texture) = 0;
    virtual void release_geometry(PanoramaCompiledGeometryHandle geometry) = 0;
};

PanoramaRenderBackend* panorama_render_backend();
void set_panorama_render_backend(PanoramaRenderBackend* backend);
}
