#pragma once

#include "ui/panorama/panorama_layout.hpp"
#include "ui/panorama/panorama_paint.hpp"

#include <filesystem>
#include <functional>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
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

// Resolves one font face's file bytes without the atlas ever touching a filesystem.
// Consulted once per candidate face NAME (the bare filename for a discovered face, the
// configured spelling for an explicitly configured one) BEFORE any on-disk probe for that
// name, in exactly the shape sourceloader::SourceSoundLibrary::ScriptLoaderOverride already
// established for the same problem (soundscript.hpp:98-104):
//
//   * a populated optional with bytes -- the provider served this face definitively; the
//     atlas opens it with FT_New_Memory_Face and never looks at the disk for that name;
//   * a populated optional that is EMPTY -- the provider handled this face definitively and
//     refuses it (an engine host enforcing a native-only content policy); the atlas must not
//     fall back to reading the name from disk, and this face simply does not load;
//   * std::nullopt -- the provider has no opinion; the atlas continues with its ordinary
//     on-disk discovery for that name.
//
// This is what lets an engine host serve faces out of a content package while PanoramaEngine
// stays standalone: the library never learns what a package, an asset store, or a mount is.
using PanoramaFontBytesProvider =
    std::function<std::optional<std::vector<unsigned char>>(std::string_view face_name)>;

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
    // Optional byte source consulted ahead of every filesystem probe. See above.
    PanoramaFontBytesProvider font_bytes_provider;
};

struct PanoramaFontAtlasDirtyRect
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct PanoramaFontAtlasCacheBudgets
{
    std::size_t measure_entries = 8192;
    std::size_t measure_bytes = 2U * 1024U * 1024U;
    std::size_t metrics_per_face = 32;
};

struct PanoramaFontAtlasStats
{
    std::uint64_t glyph_hits = 0;
    std::uint64_t glyph_misses = 0;
    std::uint64_t measure_hits = 0;
    std::uint64_t measure_misses = 0;
    std::uint64_t measure_evictions = 0;
    std::uint64_t metrics_evictions = 0;
    std::uint64_t atlas_page_resets = 0;
    std::uint64_t full_uploads = 0;
    std::uint64_t in_place_uploads = 0;
    std::uint64_t texture_replacements = 0;
    std::uint64_t uploaded_bytes = 0;
    std::uint64_t dirty_pixels = 0;
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
    void set_cache_budgets(PanoramaFontAtlasCacheBudgets budgets);
    [[nodiscard]] PanoramaFontAtlasCacheBudgets cache_budgets() const;

    void enable_counters(bool enabled);
    void reset_counters();
    [[nodiscard]] PanoramaFontAtlasStats stats() const;

    // Metric generation changes when scale/font metrics invalidate shaped
    // text. Page generation changes when existing UVs cease to be valid.
    [[nodiscard]] std::uint64_t metric_generation() const;
    [[nodiscard]] std::uint64_t atlas_page_generation() const;
    [[nodiscard]] std::uint64_t atlas_content_generation() const;
    [[nodiscard]] int atlas_width() const;
    [[nodiscard]] int atlas_height() const;
    [[nodiscard]] std::span<const PanoramaFontAtlasDirtyRect> dirty_rects() const;

    [[nodiscard]] PanoramaTextMeasure text_measure();
    void ensure_tree_text(PanoramaNode& root);
    void upload_if_dirty();

    [[nodiscard]] PanoramaGlyphSource glyph_source() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
