#pragma once

#include "ui/panorama/panorama_dom.hpp"

#include <functional>

// Panorama layout solver. Implements Panorama's box model on a styled
// PanoramaNode tree: a bottom-up intrinsic (min-content) pass followed by a
// top-down resolve pass that assigns every node an absolute PanoramaLayoutBox.
namespace openstrike
{
// Measures the on-screen size of a run of text at a given font size. The layout
// solver needs this for fit-children labels. The default is a metrics-free
// approximation; PanoramaFontAtlas provides a FreeType-backed measurer for
// pixel-accurate sizing without coupling the layout solver to a font library.
struct PanoramaTextMeasure
{
    // Returns {width, height} in pixels for `text` at `font_size` / `font_weight`,
    // adding `letter_spacing` px after each glyph. `text` is already case-transformed by the
    // caller (see panorama_transform_text), so the measurer must not transform again.
    std::function<std::pair<float, float>(
        std::string_view text, float font_size, int font_weight, float letter_spacing)> measure;
};

// Default approximate text measurer (0.5em advance, 1.2em line height).
[[nodiscard]] PanoramaTextMeasure default_text_measure();

// Lays out the tree rooted at `root` into a viewport of the given size. `root`'s
// border box is the full viewport; its children flow/size per their computed
// style. Styles must already be computed (PanoramaStyleSheet::compute) before
// calling. Safe to call repeatedly (e.g. on resize).
void layout_panorama_tree(
    PanoramaNode& root,
    float viewport_width,
    float viewport_height,
    const PanoramaTextMeasure& text_measure = default_text_measure());
}
