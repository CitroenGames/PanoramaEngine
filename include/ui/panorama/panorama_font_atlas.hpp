#pragma once

#include "ui/panorama/panorama_layout.hpp"
#include "ui/panorama/panorama_paint.hpp"

#include <filesystem>
#include <memory>

namespace openstrike
{
// FreeType-backed text provider for the native Panorama renderer. It keeps glyph
// metrics in Panorama design units and uploads a premultiplied glyph atlas through
// the active Panorama render backend.
class PanoramaFontAtlas
{
public:
    PanoramaFontAtlas();
    ~PanoramaFontAtlas();

    PanoramaFontAtlas(const PanoramaFontAtlas&) = delete;
    PanoramaFontAtlas& operator=(const PanoramaFontAtlas&) = delete;

    [[nodiscard]] bool load(const std::filesystem::path& resource_root);
    [[nodiscard]] bool loaded() const;

    void set_ui_scale(float ui_scale);

    [[nodiscard]] PanoramaTextMeasure text_measure();
    void ensure_tree_text(PanoramaNode& root);
    void upload_if_dirty();

    [[nodiscard]] PanoramaGlyphSource glyph_source() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
