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
namespace panorama
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

    // Re-uploads `rgba` into an EXISTING texture returned by generate_texture,
    // in place, instead of the caller doing release_texture+generate_texture.
    // For a host that repeatedly recomposites a texture of unchanging size (the
    // CS:GO radar disc), this avoids a GPU resource create/destroy every dirty
    // frame. Returns false when the backend cannot service the request in place
    // (unknown/stale id, size mismatch, no update support) — the caller must
    // then fall back to release_texture + generate_texture. Default no-op
    // backend always returns false, so hosts without in-place update support
    // (the software rasterizer, examples) keep today's create/destroy behavior
    // with no code change.
    virtual bool update_texture(PanoramaTextureId /*texture*/, std::span<const unsigned char> /*rgba*/,
        int /*width*/, int /*height*/)
    {
        return false;
    }

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
    // `constants` is the issuing command's PanoramaDrawConstants (see
    // panorama_paint.hpp): a 2x3 affine transform (design px) + opacity the
    // backend must apply on top of `geometry`'s already-compiled vertices,
    // instead of the painter baking it in. Identity for an untransformed,
    // fully-opaque command (or one that went through the painter's
    // legacy-bake fallback, which still bakes), so an implementer that
    // ignores it renders those unchanged but WILL render animated
    // transform/opacity content wrong; a GPU backend folds it into per-draw
    // shader state (see RhiUiRenderInterface::render_geometry), a CPU
    // rasterizer applies it to vertex positions/colour before rasterizing
    // (see panorama_apply_draw_constants).
    virtual void render_geometry(
        PanoramaCompiledGeometryHandle geometry, PanoramaTextureId texture, const PanoramaDrawConstants& constants) = 0;
    // Releases a handle previously returned by compile_geometry. Must tolerate
    // being called for a handle whose draw was already recorded into the
    // CURRENT frame's command list (PanoramaGeometryCache::submit() does this
    // on a partial-failure rollback -- see its `release()` call -- and a
    // normal submit() releases geometry that fell out of this frame's list
    // right after recording whatever DID stay in it): an implementation must
    // defer the actual GPU free (a retire/reclaim ring, fence, or similar)
    // rather than destroying the resource synchronously, or an
    // already-recorded-but-not-yet-submitted draw will read freed memory. See
    // RhiUiRenderInterface's kReclaimFrames ring for a reference
    // implementation of this contract.
    virtual void release_geometry(PanoramaCompiledGeometryHandle geometry) = 0;
};

PanoramaRenderBackend* panorama_render_backend();
void set_panorama_render_backend(PanoramaRenderBackend* backend);
}
