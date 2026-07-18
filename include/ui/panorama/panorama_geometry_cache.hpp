#pragma once

#include "ui/panorama/panorama_paint.hpp"
#include "ui/panorama/panorama_render_backend.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

// Incremental GPU-geometry submission for a PanoramaDrawList. Writing a correct
// version of this by hand is the single hardest part of wiring a new host up to
// PanoramaEngine: naively recompiling every command's geometry every frame is
// far too slow for an animated UI, but reusing GPU handles across frames without
// silently going stale (behind a text/color change, a resized scissor rect, a
// tree edit that shifts command order) is easy to get subtly wrong. This class
// is the host-independent solution, factored out of a production Panorama host
// bridge after it had already had these bugs found and fixed once.
namespace panorama
{
// Content signature of a single draw command, scaled by `ui_scale` (geometry is
// compiled in framebuffer pixels = design pixels * ui_scale, so the same command
// at a different scale must not collide). Hashes ui_scale + texture + blend_mode
// + the vertex/index bytes ONLY -- scissor rect, blur params, and the command's
// PanoramaDrawConstants are mutable per-entry state PanoramaGeometryCache updates
// in place on a signature match (see submit()), so a change to any of them must
// NOT change the signature (a scissor-only change, for instance, must not force
// a recompile). Two calls with an identical (command, ui_scale) always hash
// equal; used by PanoramaGeometryCache to detect commands whose vertex/index
// content did not change since the previous submit(). Exposed publicly so a
// host or test can reason about cache behavior directly.
[[nodiscard]] std::uint64_t panorama_geometry_signature(const PanoramaDrawCommand& command, float ui_scale);

// Optional per-call cost/reuse report for submit() (a nullable out-param so the
// pre-existing 3-argument call sites are unaffected). `commands` is every
// non-blur, non-empty command submit() considered (reused + recompiled, plus
// any that failed to compile); `uploaded_bytes` totals the vertex+index bytes
// passed to compile_geometry for `recompiled` commands only (a reused command
// uploads nothing). `hash_us`/`compile_us` are wall time measured with
// std::chrono around exactly the panorama_geometry_signature and
// compile_geometry calls -- zero clock reads when a submit() call passes no
// stats pointer, so a host that never profiles pays nothing extra. This type
// (and submit()'s use of it) intentionally has no dependency on any host
// profiler: the engine stays host-independent, so a host wires these numbers
// into its own profiling type itself (see UiProfileFrame in OpenStrike).
struct PanoramaGeometrySubmitStats
{
    double hash_us = 0.0;
    double compile_us = 0.0;
    int commands = 0;
    int reused = 0;
    int recompiled = 0;
    std::size_t uploaded_bytes = 0;
};

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
    // signature -- see panorama_geometry_signature -- compared position by
    // position) and issues render_geometry/compile_geometry/blur_region calls
    // through `backend` for every command: unchanged commands are reused as-is
    // (their scissor rect, blur params and PanoramaDrawConstants are copied onto
    // the reused entry in place, even if those changed -- see
    // panorama_geometry_signature's comment), changed or new ones are
    // (re)compiled, geometry that fell out of the list is released. `ui_scale`
    // must be the same value passed to
    // layout_panorama_tree() this frame (scissor rects and the content signature
    // are computed in framebuffer pixels = design pixels * ui_scale). Leaves
    // valid() true only when every command compiled or reused successfully.
    // `stats`, when non-null, is filled with this call's cost/reuse report (see
    // PanoramaGeometrySubmitStats); left untouched when null.
    void submit(const PanoramaDrawList& draw_list, PanoramaRenderBackend& backend, float ui_scale,
        PanoramaGeometrySubmitStats* stats = nullptr);

    // ---- Recomposite-only fast path (PanoramaDrawConstants campaign, Slice 3) ----
    //
    // Lets a caller that retained the layer-context table from the last
    // submit()'s PanoramaDrawList (see PanoramaLayerContextEntry in
    // panorama_paint.hpp) recompute just the transform/opacity constants for
    // an animation-advance frame and patch them into the cache in place -- no
    // diffing, no hashing, no compile_geometry, no backend calls until the
    // caller's own replay() re-issues render_geometry with the patched
    // values. The accessors below are READ-ONLY views into the same per-entry
    // state submit()/replay() already maintain; `patch_constants`/
    // `patch_blur` are the only mutators. A caller MUST confirm
    // entry_constants_patchable() before patch_constants() on an entry whose
    // constants are actually changing -- these methods do not re-validate it
    // themselves (see PanoramaNativeView::try_recomposite_fast_path for the
    // reference precondition scan).

    // Number of currently-cached entries (one per non-empty draw command
    // submit() compiled or reused, plus one per backdrop-blur command; NOT
    // 1:1 with the source PanoramaDrawList::commands when it contained
    // empty/skipped commands -- see submit()'s own comment).
    [[nodiscard]] std::size_t entry_count() const noexcept { return entries_.size(); }
    // Mirrors PanoramaDrawCommand::context_index for entry `index` (-1 =
    // root/legacy-baked, out of range = -1).
    [[nodiscard]] int entry_context_index(std::size_t index) const;
    // Mirrors PanoramaDrawCommand::constants_patchable for entry `index`
    // (out of range = false, the conservative "cannot patch" answer).
    [[nodiscard]] bool entry_constants_patchable(std::size_t index) const;
    // The PanoramaDrawConstants entry `index` currently renders with (out of
    // range returns the identity value).
    [[nodiscard]] const PanoramaDrawConstants& entry_constants(std::size_t index) const;
    // Mirrors PanoramaDrawCommand::blur_source_node for entry `index`
    // (nullptr for a non-blur entry or an out-of-range index).
    [[nodiscard]] const PanoramaNode* entry_blur_source_node(std::size_t index) const;

    // Overwrites entry `index`'s PanoramaDrawConstants in place. Out-of-range
    // `index` is a no-op (defensive -- a caller whose own bounds are
    // entry_count() should never trigger this).
    void patch_constants(std::size_t index, const PanoramaDrawConstants& constants);
    // Overwrites a backdrop-blur entry's std/passes in place (the mutable
    // per-entry state a value-only blur change needs, see
    // PanoramaAnimationAdvanceResult::recomposite_changed's blur rule). No-op
    // if `index` is out of range or is not a blur entry.
    void patch_blur(std::size_t index, float std_x, float std_y, int passes);

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
        // Mirrors the command's PanoramaDrawConstants (see panorama_paint.hpp).
        // Not part of `signature` -- updated in place on a signature match, like
        // the scissor fields above.
        PanoramaDrawConstants constants;
        // Mirrors PanoramaDrawCommand::constants_patchable. Not part of
        // `signature`, same as `constants` above -- a command's scissor
        // dependency on a layer context does not change what content it draws.
        bool constants_patchable = true;
        // Mirrors PanoramaDrawCommand::context_index (Slice 3): which layer
        // context (in the PanoramaDrawList::contexts a caller retained from
        // the submit() that produced this entry) `constants` came from; -1
        // for root/legacy-baked. Not part of `signature`, same reasoning as
        // `constants` above.
        int context_index = -1;
        // Mirrors PanoramaDrawCommand::blur_source_node. Not part of
        // `signature` -- same reasoning as `constants` above.
        const PanoramaNode* blur_source_node = nullptr;

        [[nodiscard]] bool is_blur() const { return geometry == 0 && blur_passes > 0; }
    };

    std::vector<Entry> entries_;
    PanoramaRenderBackend* backend_ = nullptr;
    bool valid_ = false;
};
}
