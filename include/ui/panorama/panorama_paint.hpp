#pragma once

#include "ui/panorama/panorama_dom.hpp"
#include "ui/panorama/panorama_style.hpp"

#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

// Panorama paint: turns a laid-out PanoramaNode tree into a renderer-agnostic
// display list of textured/coloured quads. The host translates the display list
// into its own backend calls (e.g. RmlDx12RenderInterface CompileGeometry /
// RenderGeometry). Nothing here depends on RmlUi, FreeType, or a GPU — colours
// are straight (non-premultiplied) RGBA; the host premultiplies if it must.
namespace openstrike
{
struct PanoramaPaintVertex
{
    float x = 0.0F;
    float y = 0.0F;
    float u = 0.0F;
    float v = 0.0F;
    PanoramaColor color;
};

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

    [[nodiscard]] bool is_backdrop_blur() const { return blur_std_x > 0.0F || blur_std_y > 0.0F; }
};

struct PanoramaDrawList
{
    std::vector<PanoramaDrawCommand> commands;

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

// Supplies glyph metrics + atlas placement for text painting. The host backs
// this with FreeType; the layout layer's PanoramaTextMeasure should agree with
// the same font so measured and painted text line up. If `glyph` is null, text
// is skipped (panels still paint their backgrounds/borders).
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
