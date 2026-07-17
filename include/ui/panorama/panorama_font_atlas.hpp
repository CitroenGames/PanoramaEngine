#pragma once

#include "ui/panorama/panorama_layout.hpp"
#include "ui/panorama/panorama_paint.hpp"

#include <filesystem>
#include <memory>
#include <vector>

namespace panorama
{
// An explicitly configured font face. Relative paths are resolved against
// resource_root first, then each search_directory. The nearest configured
// weight is used when CSS asks for a weight that is not present.
struct PanoramaFontAtlasFace
{
    std::filesystem::path path;
    int weight = 400;
};

// Deterministic font loading for standalone applications. When faces is empty,
// the atlas discovers common regular/medium/bold filenames under the supplied
// search directories and conventional font folders near resource_root. When
// faces is non-empty, discovery is skipped and at least one configured face
// must load successfully.
struct PanoramaFontAtlasLoadOptions
{
    std::filesystem::path resource_root;
    std::vector<std::filesystem::path> search_directories;
    std::vector<PanoramaFontAtlasFace> faces;
};

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

    // Convenience discovery overload retained for existing integrations.
    [[nodiscard]] bool load(const std::filesystem::path& resource_root);
    [[nodiscard]] bool load(const PanoramaFontAtlasLoadOptions& options);
    void clear();
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
