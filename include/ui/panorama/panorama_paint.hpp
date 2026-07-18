#pragma once

#include "ui/panorama/panorama_dom.hpp"
#include "ui/panorama/panorama_style.hpp"

#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

// Panorama paint: turns a laid-out PanoramaNode tree into a renderer-agnostic
// display list of textured/coloured quads. The host translates the display list
// into its own backend calls. Nothing here depends on FreeType or a GPU — colours
// are straight (non-premultiplied) RGBA; the host premultiplies if it must.
namespace panorama
{
struct PanoramaPaintVertex
{
    float x = 0.0F;
    float y = 0.0F;
    float u = 0.0F;
    float v = 0.0F;
    PanoramaColor color;
};

// A 2x3 affine transform + opacity a backend applies to a command's geometry at
// render time, instead of the painter baking it into vertex positions/colours.
// Mirrors panorama_paint.cpp's internal Matrix2D exactly -- same field layout
// (a,b,c,d linear part + e,f translation) and the same convention:
//   x' = a*x + c*y + e
//   y' = b*x + d*y + f
// (CSS `matrix(a,b,c,d,e,f)` order) -- so a later slice can copy a node's
// accumulated transform straight into this struct with no reshuffling.
// Positions are in design pixels, same space as PanoramaPaintVertex; a backend
// compiling to framebuffer pixels scales e/f (not a/b/c/d) by ui_scale -- see
// RhiUiRenderInterface::render_geometry.
//
// The painter (DrawListBuilder::paint) emits this per command as the node
// tree's accumulated transform/opacity for whichever layer context was
// current when the command was created: promotion-on-animate -- a node with
// an ACTIVELY ANIMATING transform/opacity (see panorama_node_opens_layer_
// context below) opens a new context (matrix = the FULL root-accumulated
// transform, opacity = the FULL root-accumulated opacity -- not relative to
// the parent context, see node_transform_matrix's associativity argument in
// panorama_paint.cpp) and every command painted under it -- this node's own
// geometry and its descendants', until a nested context takes over -- carries
// that context's accumulated value here instead of it being baked into
// vertex positions/colours. A node with a STATIC non-identity transform or
// opacity < 1 does NOT open a context -- it bakes instead, relative to the
// enclosing context (see paint()'s local baked-accumulator paragraph), so
// its own contribution never appears here at all. Identity means either the
// root context (no transform/opacity applies anywhere above), a static bake,
// or the legacy-bake fallback (vertex generation that genuinely depends on
// the final matrix, e.g. clip:radial's wedge clipper -- see the fallback's
// comment in panorama_paint.cpp), which still bakes into vertices exactly as
// the painter always did.
struct PanoramaDrawConstants
{
    float a = 1.0F;
    float b = 0.0F;
    float c = 0.0F;
    float d = 1.0F;
    float e = 0.0F;
    float f = 0.0F;
    float opacity = 1.0F;

    [[nodiscard]] bool is_identity() const
    {
        return a == 1.0F && b == 0.0F && c == 0.0F && d == 1.0F && e == 0.0F && f == 0.0F && opacity == 1.0F;
    }

    [[nodiscard]] bool operator==(const PanoramaDrawConstants& other) const
    {
        return a == other.a && b == other.b && c == other.c && d == other.d && e == other.e && f == other.f &&
            opacity == other.opacity;
    }
};

// Applies `constants`'s affine transform + opacity to a paint-space vertex, in
// design-px space (constants.e/f are NOT ui_scale-scaled here -- that scaling
// is only correct once positions are in framebuffer px, see
// PanoramaDrawConstants's own comment). For consumers that walk
// PanoramaDrawCommand::vertices on the CPU directly (the engine's CPU
// rasterizer examples, BMP render probes) instead of going through a
// PanoramaRenderBackend, which instead folds constants into its own per-draw
// uniforms/shader state (see RhiUiRenderInterface::render_geometry).
[[nodiscard]] PanoramaPaintVertex panorama_apply_draw_constants(
    const PanoramaPaintVertex& vertex, const PanoramaDrawConstants& constants);

// True when `node` itself would open a NEW layer context in the painter (see
// PanoramaDrawCommand::constants's comment and DrawListBuilder::paint's
// layer-context paragraph in panorama_paint.cpp): promotion-on-animate --
// `node` has an ACTIVELY ANIMATING recomposite-class channel (a running CSS
// transition on `transform`/`pre-transform-scale2d`/`opacity`, or an applied
// @keyframes run declaring a Transform or Opacity channel). A node with a
// STATIC non-identity transform or STATIC opacity < 1 does NOT open a context
// -- it is baked into vertices/colours instead, exactly like the pre-layer-
// context painter, just relative to the enclosing context (see paint()'s
// local baked-accumulator paragraph) -- so this function answers "does
// `node` need a context RIGHT NOW", not "would baking it ever have been
// possible". Exposed so a caller retaining layer contexts across frames
// (PanoramaNativeView's recomposite-only fast path) can detect a node
// crossing INTO this condition without already having been a context source
// in the retained table -- which the fast path cannot represent (adding a
// context requires a repaint, and an animation start always arrives on a
// style-recompute frame that rebuilds anyway) -- and fall back to a real
// repaint that frame. Mirrors paint()'s own context-open check exactly (both
// call this SAME function) so the two can never disagree.
[[nodiscard]] bool panorama_node_opens_layer_context(const PanoramaNode& node);

// True when `node` on its own -- ignoring ancestors, see below -- can
// contribute nothing to the draw list: display:none-equivalent
// (!computed.visible), or a STATIC (non-transition/keyframe-animating)
// opacity <= 0. Mirrors paint()'s own top-of-function early-outs exactly (the
// `!s.visible` check and the tightened opacity<=0 rule -- see
// DrawListBuilder::paint in panorama_paint.cpp), factored out so
// panorama_anim.cpp's advance passes can suppress dirt from a subtree paint()
// will skip anyway WITHOUT re-deriving the condition by hand (which could
// silently drift from what paint() actually does). Deliberately narrower than
// paint()'s full skip-condition set (it does not know about clip:radial's
// full-circle wedge or an empty clip:rect -- see paint()'s other early-
// returns): a caller ORs this with whatever it inherited from ancestors as it
// walks the tree top-down (paint() itself, via its recursive short-circuit,
// and the advance passes threading an inherited flag down their own
// recursion) -- this function only ever answers for THIS node, so the two
// passes computing the same inherited flag can never disagree about a single
// node's own contribution.
[[nodiscard]] bool panorama_node_paint_unreachable(const PanoramaNode& node);

// Recomputes the accumulated PanoramaDrawConstants (2D transform + opacity)
// DrawListBuilder::paint would compute for `node`'s own layer context, given
// `parent_source_node` (the nearest ANCESTOR node that itself opens a layer
// context, or nullptr for the root) and that ancestor's own ALREADY-
// ACCUMULATED constants (root/nullptr = PanoramaDrawConstants{}, the identity
// value). Under promotion-on-animate, a STATIC (non-context-opening) node CAN
// sit between `node` and `parent_source_node` with a non-identity local
// transform/opacity (see panorama_node_opens_layer_context) -- unlike the
// pre-Slice-3-fix invariant, composing `node`'s own local delta directly onto
// `parent_context` is NOT exact in general, so this walks `node`'s ancestor
// chain (PanoramaNode::parent) up to (excluding) `parent_source_node`,
// composing every intervening STATIC node's own local transform/opacity along
// the way -- exactly replaying the painter's local baked-accumulator for a
// promoted node nested under static non-identity ancestors (see paint()'s
// layer-context paragraph in panorama_paint.cpp). When no static node sits in
// between (the common case, and the only case pre-promotion-on-animate could
// produce) this reduces to composing only `node`'s own local delta, same as
// before. Used by a caller that retains the layer-context table
// (PanoramaDrawList::contexts) across frames to patch cached geometry's
// constants without repainting (PanoramaNativeView's recomposite-only fast
// path); factored out of the painter itself so the two computations can never
// drift apart.
[[nodiscard]] PanoramaDrawConstants panorama_recompute_layer_context(
    const PanoramaNode& node, const PanoramaNode* parent_source_node, const PanoramaDrawConstants& parent_context);

// One batch of geometry sharing a texture and scissor. texture == 0 means an
// untextured solid fill (the backend should bind a 1x1 white texture). A
// positive texture id refers either to the glyph atlas (see PanoramaGlyphSource)
// or to an image handle the host registered.
struct PanoramaDrawCommand
{
    std::vector<PanoramaPaintVertex> vertices;
    std::vector<int> indices;
    PanoramaTextureId texture = 0;
    PanoramaBlendMode blend_mode = PanoramaBlendMode::Normal;
    bool scissor = false;
    float scissor_x = 0.0F;
    float scissor_y = 0.0F;
    float scissor_width = 0.0F;
    float scissor_height = 0.0F;
    // Backdrop-blur command (carries NO geometry): gaussian-blur everything
    // already rendered inside the scissor rect — Panorama's `blur:
    // gaussian/fastgaussian(stdX, stdY, passes)` on a panel, emitted right after
    // the panel's subtree painted so blurring the region equals blurring the
    // subtree. std deviations are in design pixels.
    float blur_std_x = 0.0F;
    float blur_std_y = 0.0F;
    int blur_passes = 0;
    // The node whose `computed.blur` this backdrop-blur command's std/passes
    // came from (nullptr for every non-blur command). A blur value change
    // that keeps the command's existence stable (see
    // PanoramaAnimationAdvanceResult::recomposite_changed's blur rule) is
    // mutable per-entry state like `constants` below -- patched via
    // PanoramaGeometryCache::patch_blur instead of a recompile -- and this is
    // how a caller retaining the cache across frames finds which entry a
    // dirty node's blur command is.
    const PanoramaNode* blur_source_node = nullptr;
    // See PanoramaDrawConstants. The painter emits raw (untransformed,
    // undimmed-by-inherited-opacity) vertices for a command and attaches its
    // enclosing layer context here instead of baking transform/opacity into
    // vertex data -- except the legacy-bake fallback (clip:radial and other
    // vertex-generation-depends-on-the-matrix cases), which still bakes and
    // always leaves this identity. See DrawListBuilder::paint's layer-context
    // comment in panorama_paint.cpp.
    PanoramaDrawConstants constants;
    // True when `constants` can be REPLACED in place (a future frame patches
    // just the matrix/opacity and re-issues render_geometry) without touching
    // `scissor_*` or recompiling: the scissor rect was computed entirely from
    // ancestors whose matrix cannot change (untransformed, or transformed only
    // by an ancestor OUTSIDE this command's own layer context chain -- see
    // ClipRect::patchable in panorama_paint.cpp). False for every legacy-baked
    // command (its geometry already has the matrix baked in, so `constants`
    // patching would double-apply it) and for any command whose scissor traces
    // through a non-root layer context. A cache entry mirrors this field so a
    // later slice can gate its fast path on it; ignoring it is always safe
    // (just forces the slower rebuild path).
    bool constants_patchable = true;
    // Index into PanoramaDrawList::contexts identifying which layer context
    // `constants` came from; -1 means the root context or the legacy-bake
    // fallback (constants is always identity in both cases, so there is
    // nothing to look up). Purely informational bookkeeping alongside
    // `constants` -- no PanoramaRenderBackend implementer is required to read
    // it; it exists for a caller that retains PanoramaGeometryCache/
    // PanoramaDrawList::contexts across frames and wants to re-associate a
    // cached entry with the context whose source node changed, without
    // re-walking the paint (see PanoramaGeometryCache::entry_context_index
    // and PanoramaNativeView's recomposite-only fast path).
    int context_index = -1;

    [[nodiscard]] bool is_backdrop_blur() const { return blur_std_x > 0.0F || blur_std_y > 0.0F; }
};

// One entry in PanoramaDrawList::contexts: mirrors DrawListBuilder's internal
// LayerContext (panorama_paint.cpp), but additionally records what PRODUCED
// it -- the node whose own transform/opacity opened it, and which ancestor
// context it composes onto -- instead of just the resolved matrix/opacity
// value. `source_node` lets a caller recompute this context's
// PanoramaDrawConstants after the node's computed style changes (see
// panorama_recompute_layer_context) without repainting;
// `parent_context_index` (-1 = the implicit root context: identity matrix,
// opacity 1, never stored as an explicit entry) lets the recompute walk the
// chain top-down. Entries are always appended in an order where a context's
// parent already precedes it (paint() is a preorder tree walk: a node's own
// context, if any, is pushed before its children can push theirs), so a
// single forward pass over PanoramaDrawList::contexts always has each
// entry's parent already resolved. Under promotion-on-animate, a STATIC
// (non-context-opening) node can sit between `source_node` and its
// `parent_context_index` entry's own `source_node` with a non-identity local
// contribution of its own -- panorama_recompute_layer_context walks that gap
// via PanoramaNode::parent (see its own doc comment), so this table's
// adjacency alone is not the complete ancestor chain.
struct PanoramaLayerContextEntry
{
    const PanoramaNode* source_node = nullptr;
    int parent_context_index = -1; // -1 = root
};

struct PanoramaDrawList
{
    std::vector<PanoramaDrawCommand> commands;
    // Layer contexts this frame's commands reference by
    // PanoramaDrawCommand::context_index (see PanoramaLayerContextEntry
    // above); rebuilt every build_panorama_draw_list() call, same as
    // `commands`. Typically far smaller than `commands` (one entry per
    // transformed/faded element, not per draw call).
    std::vector<PanoramaLayerContextEntry> contexts;

    [[nodiscard]] std::size_t total_vertices() const;
    [[nodiscard]] std::size_t total_indices() const;
};

// Scratch storage for repeated display-list builds. Keeping command buffers alive
// across frames avoids reallocating every vertex/index vector while animated UI is
// repainting.
struct PanoramaPaintScratch
{
    std::vector<PanoramaDrawCommand> reusable_commands;
};

// A positioned, atlased glyph. Coordinates are relative to the pen position on
// the baseline; (u0,v0)-(u1,v1) are atlas texture coordinates in [0,1].
struct PanoramaGlyph
{
    float advance = 0.0F;   // horizontal pen advance after this glyph
    float bearing_x = 0.0F; // left side bearing from the pen
    float bearing_y = 0.0F; // top bearing above the baseline
    float width = 0.0F;     // glyph quad width
    float height = 0.0F;    // glyph quad height
    float u0 = 0.0F;
    float v0 = 0.0F;
    float u1 = 0.0F;
    float v1 = 0.0F;
    bool valid = false;     // false -> whitespace / no geometry, advance only
};

// Supplies glyph metrics + atlas placement for text painting. PanoramaFontAtlas
// provides the built-in FreeType source; custom sources are also supported. The
// layout layer's PanoramaTextMeasure should agree with the same font so measured
// and painted text line up. If `glyph` is null, text is skipped (panels still
// paint their backgrounds/borders).
struct PanoramaGlyphSource
{
    // Returns the glyph for `codepoint` at `font_size` / `font_weight`. Returning
    // false skips it.
    std::function<bool(char32_t codepoint, float font_size, int font_weight, PanoramaGlyph& out)> glyph;
    // Ascent (pixels above baseline) for vertical placement of a text line.
    std::function<float(float font_size, int font_weight)> ascent;
    // The texture id of the glyph atlas (passed through on text draw commands).
    PanoramaTextureId atlas_texture = 0;
};

// Builds the display list for the tree rooted at `root` (already styled + laid
// out). Painter's order: each node's background, then border, then text, then
// its children. `glyphs.glyph` may be null to skip text.
void build_panorama_draw_list(
    PanoramaDrawList& out,
    const PanoramaNode& root,
    const PanoramaGlyphSource& glyphs = {},
    PanoramaPaintScratch* scratch = nullptr);

[[nodiscard]] PanoramaDrawList build_panorama_draw_list(
    const PanoramaNode& root,
    const PanoramaGlyphSource& glyphs = {});
}
