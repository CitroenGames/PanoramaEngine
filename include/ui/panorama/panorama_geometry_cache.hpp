#pragma once

#include "ui/panorama/panorama_paint.hpp"
#include "ui/panorama/panorama_render_backend.hpp"

#include <cstdint>
#include <vector>

// Incremental GPU-geometry submission for a PanoramaDrawList. Writing a correct
// version of this by hand is the single hardest part of wiring a new host up to
// PanoramaEngine: naively recompiling every command's geometry every frame is
// far too slow for an animated UI, but reusing GPU handles across frames without
// silently going stale (behind a text/color change, a resized scissor rect, a
// tree edit that shifts command order) is easy to get subtly wrong. This class
// is the host-independent solution, factored out of OpenStrike's own Panorama
// host bridge after it had already had these bugs found and fixed once.
namespace panorama
{
// Content signature of a single draw command, scaled by `ui_scale` (geometry is
// compiled in framebuffer pixels = design pixels * ui_scale, so the same command
// at a different scale must not collide). Two calls with an identical
// (command, ui_scale) always hash equal; used by PanoramaGeometryCache to detect
// commands that did not change since the previous submit(). Exposed publicly so
// a host or test can reason about cache behavior directly.
[[nodiscard]] std::uint64_t panorama_geometry_signature(const PanoramaDrawCommand& command, float ui_scale);

// Owns the previous frame's compiled geometry and either replays it verbatim or
// diffs a fresh PanoramaDrawList against it, compiling only what changed.
//
// Typical per-frame use:
//
//   if (!visual_dirty && geometry_cache.replay(backend)) {
//       return; // nothing changed: cheapest possible frame
//   }
//   PanoramaDrawList list = build_panorama_draw_list(root, glyphs);
//   geometry_cache.submit(list, backend, ui_scale);
//
// Ownership: compiled geometry handles are tied to whichever PanoramaRenderBackend
// last compiled them. Call release() before destroying that backend, or before
// switching to a different one, so this cache never holds a dangling handle.
class PanoramaGeometryCache
{
public:
    PanoramaGeometryCache() = default;
    ~PanoramaGeometryCache();

    PanoramaGeometryCache(const PanoramaGeometryCache&) = delete;
    PanoramaGeometryCache& operator=(const PanoramaGeometryCache&) = delete;

    // True once submit() last completed with every command compiled or reused
    // successfully (a partially-failed submit() leaves this false and releases
    // whatever it built, so a cache is never silently missing draw commands).
    [[nodiscard]] bool valid() const { return valid_; }

    // Re-issues every cached command's render_geometry/blur_region call through
    // `backend` unchanged: no diffing, no draw-list needed. The fast path for a
    // frame where nothing visual changed. Returns false and does nothing if
    // valid() is false or `backend` differs from the one submit() last used
    // (the caller should fall back to submit() in that case).
    bool replay(PanoramaRenderBackend& backend) const;

    // Diffs `draw_list`'s commands against the previous submit() (by content
    // signature + texture + blend mode + scissor rect, compared position by
    // position) and issues render_geometry/compile_geometry/blur_region calls
    // through `backend` for every command: unchanged commands are reused as-is,
    // changed or new ones are (re)compiled, geometry that fell out of the list is
    // released. `ui_scale` must be the same value passed to
    // layout_panorama_tree() this frame (scissor rects and the content signature
    // are computed in framebuffer pixels = design pixels * ui_scale). Leaves
    // valid() true only when every command compiled or reused successfully.
    void submit(const PanoramaDrawList& draw_list, PanoramaRenderBackend& backend, float ui_scale);

    // Releases every currently-cached GPU geometry handle through
    // panorama_render_backend() (the library's current-backend global) IF it
    // still matches the backend that compiled them, then clears the cache.
    // Safe to call when nothing is cached, when the owning backend has already
    // changed (skips the release call, just forgets the stale handles), or from
    // a destructor. Call this before destroying/replacing the active render
    // backend so this cache never holds a dangling handle across the switch.
    void release();

private:
    struct Entry
    {
        PanoramaCompiledGeometryHandle geometry = 0;
        PanoramaTextureId texture = 0;
        PanoramaBlendMode blend_mode = PanoramaBlendMode::Normal;
        bool scissor = false;
        int scissor_x = 0;
        int scissor_y = 0;
        int scissor_width = 0;
        int scissor_height = 0;
        std::uint64_t signature = 0;
        // Backdrop-blur entry (geometry == 0, carries no compiled geometry):
        // blur_region() the scissor rect in place instead of rendering a quad.
        float blur_std_x = 0.0F;
        float blur_std_y = 0.0F;
        int blur_passes = 0;

        [[nodiscard]] bool is_blur() const { return geometry == 0 && blur_passes > 0; }
    };

    std::vector<Entry> entries_;
    PanoramaRenderBackend* backend_ = nullptr;
    bool valid_ = false;
};
}
