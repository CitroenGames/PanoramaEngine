#pragma once

// Shared by the Win32 (win32_main.cpp) and POSIX/X11 (posix_main.cpp) window
// hosts: loads a Panorama layout from a real XML file on disk, lays it out,
// and rasterizes the resulting draw list into an RGBA framebuffer entirely on
// the CPU. The rasterizer itself is the same ~60-line triangle fill used by
// examples/02_software_raster, factored out here so both platform mains blit
// the same framebuffer instead of each hand-rolling their own.
#include "ui/panorama/panorama_document_session.hpp"
#include "ui/panorama/panorama_font_atlas.hpp"
#include "ui/panorama/panorama_input.hpp"
#include "ui/panorama/panorama_layout.hpp"
#include "ui/panorama/panorama_paint.hpp"
#include "ui/panorama/panorama_render_backend.hpp"
#include "ui/panorama/panorama_resource_provider.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace panorama_example
{
// Straight-alpha RGBA8 buffer, row-major top-down. This matches the colour
// space PanoramaDrawList already uses, so no channel conversion happens here
// -- each platform main packs it into its own native pixel format at blit
// time instead.
struct Framebuffer
{
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;

    void resize(int new_width, int new_height)
    {
        width = new_width;
        height = new_height;
        rgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4, 0);
    }

    void clear(std::uint8_t r, std::uint8_t g, std::uint8_t b)
    {
        for (std::size_t i = 0; i < rgba.size() / 4; ++i)
        {
            rgba[i * 4 + 0] = r;
            rgba[i * 4 + 1] = g;
            rgba[i * 4 + 2] = b;
            rgba[i * 4 + 3] = 255;
        }
    }

    // Straight-alpha source-over blend, matching the draw list's colour space.
    void blend(int x, int y, float r, float g, float b, float a)
    {
        if (x < 0 || y < 0 || x >= width || y >= height || a <= 0.0F)
        {
            return;
        }
        std::uint8_t* px =
            &rgba[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4];
        const float inv = 1.0F - a;
        px[0] = static_cast<std::uint8_t>(r * a + static_cast<float>(px[0]) * inv);
        px[1] = static_cast<std::uint8_t>(g * a + static_cast<float>(px[1]) * inv);
        px[2] = static_cast<std::uint8_t>(b * a + static_cast<float>(px[2]) * inv);
    }
};

// A texture as generate_texture() handed it to us: plain RGBA8, no mipmaps,
// no filtering beyond nearest-neighbour -- plenty for glyph atlas lookups.
struct CpuTexture
{
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
};

// The minimal PanoramaRenderBackend a CPU rasterizer needs: just enough
// texture bookkeeping for PanoramaFontAtlas to upload its glyph atlas into
// something rasterize_draw_list() can sample from. Geometry compile/render/
// release are pure virtual on PanoramaRenderBackend but never actually used
// -- this example walks PanoramaDrawList itself instead of going through
// PanoramaGeometryCache, so they're no-ops.
class CpuTextureStore final : public panorama::PanoramaRenderBackend
{
public:
    panorama::PanoramaTextureId generate_texture(std::span<const unsigned char> rgba, int width, int height) override
    {
        const panorama::PanoramaTextureId id = next_texture_id_++;
        textures_[id] = CpuTexture{width, height, std::vector<std::uint8_t>(rgba.begin(), rgba.end())};
        return id;
    }

    void release_texture(panorama::PanoramaTextureId texture) override { textures_.erase(texture); }

    bool update_texture(panorama::PanoramaTextureId texture, std::span<const unsigned char> rgba, int width, int height) override
    {
        const auto it = textures_.find(texture);
        if (it == textures_.end() || it->second.width != width || it->second.height != height)
        {
            return false;
        }
        it->second.rgba.assign(rgba.begin(), rgba.end());
        return true;
    }

    panorama::PanoramaCompiledGeometryHandle compile_geometry(
        std::span<const panorama::PanoramaPaintVertex>, std::span<const int>, float) override
    {
        return ++next_geometry_id_;
    }
    void render_geometry(panorama::PanoramaCompiledGeometryHandle, panorama::PanoramaTextureId) override {}
    void release_geometry(panorama::PanoramaCompiledGeometryHandle) override {}

    [[nodiscard]] const CpuTexture* find(panorama::PanoramaTextureId id) const
    {
        const auto it = textures_.find(id);
        return it == textures_.end() ? nullptr : &it->second;
    }

private:
    std::unordered_map<panorama::PanoramaTextureId, CpuTexture> textures_;
    panorama::PanoramaTextureId next_texture_id_ = 1;
    panorama::PanoramaCompiledGeometryHandle next_geometry_id_ = 0;
};

// Fills one triangle with per-vertex colour interpolation (barycentric). When
// `texture` is non-null, each pixel's interpolated vertex colour is further
// modulated by a nearest-neighbour texture sample -- this is how glyph quads
// (solid-white-times-coverage atlas texels, straight-alpha vertex colour)
// turn into coloured, anti-aliased text.
inline void rasterize_triangle(
    Framebuffer& fb,
    const panorama::PanoramaPaintVertex& v0,
    const panorama::PanoramaPaintVertex& v1,
    const panorama::PanoramaPaintVertex& v2,
    const CpuTexture* texture = nullptr)
{
    const float min_x = std::min({v0.x, v1.x, v2.x});
    const float max_x = std::max({v0.x, v1.x, v2.x});
    const float min_y = std::min({v0.y, v1.y, v2.y});
    const float max_y = std::max({v0.y, v1.y, v2.y});

    const float denom = (v1.y - v2.y) * (v0.x - v2.x) + (v2.x - v1.x) * (v0.y - v2.y);
    if (denom == 0.0F)
    {
        return; // degenerate
    }

    const int x0 = std::max(0, static_cast<int>(min_x));
    const int x1 = std::min(fb.width - 1, static_cast<int>(max_x));
    const int y0 = std::max(0, static_cast<int>(min_y));
    const int y1 = std::min(fb.height - 1, static_cast<int>(max_y));

    for (int y = y0; y <= y1; ++y)
    {
        for (int x = x0; x <= x1; ++x)
        {
            const float px = static_cast<float>(x) + 0.5F;
            const float py = static_cast<float>(y) + 0.5F;
            const float w0 = ((v1.y - v2.y) * (px - v2.x) + (v2.x - v1.x) * (py - v2.y)) / denom;
            const float w1 = ((v2.y - v0.y) * (px - v2.x) + (v0.x - v2.x) * (py - v2.y)) / denom;
            const float w2 = 1.0F - w0 - w1;
            if (w0 < 0.0F || w1 < 0.0F || w2 < 0.0F)
            {
                continue;
            }
            float r = w0 * v0.color.r + w1 * v1.color.r + w2 * v2.color.r;
            float g = w0 * v0.color.g + w1 * v1.color.g + w2 * v2.color.g;
            float b = w0 * v0.color.b + w1 * v1.color.b + w2 * v2.color.b;
            float a = (w0 * v0.color.a + w1 * v1.color.a + w2 * v2.color.a) / 255.0F;

            if (texture != nullptr && texture->width > 0 && texture->height > 0)
            {
                const float u = w0 * v0.u + w1 * v1.u + w2 * v2.u;
                const float v = w0 * v0.v + w1 * v1.v + w2 * v2.v;
                const int tx = std::clamp(static_cast<int>(u * static_cast<float>(texture->width)), 0, texture->width - 1);
                const int ty = std::clamp(static_cast<int>(v * static_cast<float>(texture->height)), 0, texture->height - 1);
                const std::uint8_t* texel =
                    &texture->rgba[(static_cast<std::size_t>(ty) * static_cast<std::size_t>(texture->width) +
                                        static_cast<std::size_t>(tx)) *
                                    4];
                r *= static_cast<float>(texel[0]) / 255.0F;
                g *= static_cast<float>(texel[1]) / 255.0F;
                b *= static_cast<float>(texel[2]) / 255.0F;
                a *= static_cast<float>(texel[3]) / 255.0F;
            }

            fb.blend(x, y, r, g, b, a);
        }
    }
}

// Replays every triangle in `draw_list` into `fb`, sampling `textures` for
// commands whose texture id is non-zero (glyph runs from a PanoramaGlyphSource
// -- this example paints no images, so glyphs are the only textured geometry
// it ever emits). A texture id with no matching entry in `textures` (nothing
// generate_texture()'d it, e.g. no font was loaded) rasterizes as untextured,
// same as examples/02_software_raster's fully-untextured path.
inline void rasterize_draw_list(Framebuffer& fb, const panorama::PanoramaDrawList& draw_list, const CpuTextureStore& textures)
{
    for (const panorama::PanoramaDrawCommand& command : draw_list.commands)
    {
        const CpuTexture* texture = command.texture != 0 ? textures.find(command.texture) : nullptr;
        for (std::size_t i = 0; i + 2 < command.indices.size(); i += 3)
        {
            rasterize_triangle(
                fb,
                command.vertices[static_cast<std::size_t>(command.indices[i + 0])],
                command.vertices[static_cast<std::size_t>(command.indices[i + 1])],
                command.vertices[static_cast<std::size_t>(command.indices[i + 2])],
                texture);
        }
    }
}

// Owns the loaded document and its current CPU-rasterized frame. Both
// platform mains construct one, call load() once against a file path on
// startup, then call layout_and_rasterize() once for the initial window size
// and again whenever the window is resized.
class RasterDocument
{
public:
    // Loads `xml_path` off the real filesystem: the session's resource
    // provider is a PanoramaDirectoryResourceProvider rooted at the file's
    // parent directory, so a `<styles><include src="..."/>` in the document
    // resolves against sibling files next to it (see
    // examples/04_window_raster/sample/ for the layout this loads by
    // default). Returns false and logs to stderr if the document fails to
    // load or parses empty.
    [[nodiscard]] bool load(const std::filesystem::path& xml_path)
    {
        std::error_code error;
        const std::filesystem::path absolute = std::filesystem::absolute(xml_path, error);
        if (error)
        {
            std::fprintf(stderr, "invalid path: %s\n", xml_path.string().c_str());
            return false;
        }

        const std::filesystem::path resource_root = absolute.parent_path();
        panorama::PanoramaDocumentSessionOptions options;
        options.resource_root = resource_root;
        session_.resources().add_provider(std::make_unique<panorama::PanoramaDirectoryResourceProvider>(resource_root));

        if (!session_.load(absolute.filename().string(), options))
        {
            std::fprintf(stderr, "failed to load %s\n", absolute.string().c_str());
            return false;
        }

        // Text rendering needs an actual font file on disk; the engine
        // itself intentionally does not vendor one (see docs/architecture.md's
        // "Text" extension point), but this example bundles Lato (SIL OFL,
        // see sample/resource/ui/fonts/OFL.txt) under
        // `<resource_root>/resource/ui/fonts/` (matching CS:GO's own content
        // layout) so it renders real glyphs out of the box. Point --
        // load() at a resource_root without a font under that path (e.g. a
        // custom layout.xml elsewhere) and it logs a warning instead;
        // glyph_source()/text_measure() below then degrade to the same
        // "panels paint, text is skipped" behaviour as
        // examples/02_software_raster.
        panorama::set_panorama_render_backend(&textures_);
        if (!font_atlas_.load(resource_root))
        {
            std::fprintf(
                stderr,
                "no font found under %s -- text will not render (see raster_view.hpp)\n",
                (resource_root / "resource/ui/fonts").string().c_str());
        }

        panorama::PanoramaNode& root = *session_.document().root;
        session_.style_sheet().compute(root);
        panorama_apply_visibility_overrides(root);
        panorama_apply_control_presentation(root);
        return true;
    }

    // Re-runs layout at the given viewport size and rasterizes the resulting
    // draw list into framebuffer(). Cheap enough to call on every resize
    // event -- this is a CPU rasterizer over one static document, not an
    // animated per-frame loop, so there is no geometry cache to keep warm.
    void layout_and_rasterize(int width, int height)
    {
        width = std::max(width, 1);
        height = std::max(height, 1);

        panorama::PanoramaNode& root = *session_.document().root;
        layout_panorama_tree(root, static_cast<float>(width), static_cast<float>(height), font_atlas_.text_measure());

        font_atlas_.ensure_tree_text(root);
        font_atlas_.upload_if_dirty();
        const panorama::PanoramaDrawList draw_list = build_panorama_draw_list(root, font_atlas_.glyph_source());

        framebuffer_.resize(width, height);
        framebuffer_.clear(24, 27, 32);
        rasterize_draw_list(framebuffer_, draw_list, textures_);
    }

    [[nodiscard]] const Framebuffer& framebuffer() const noexcept { return framebuffer_; }

private:
    panorama::PanoramaDocumentSession session_;
    panorama::PanoramaFontAtlas font_atlas_;
    CpuTextureStore textures_;
    Framebuffer framebuffer_;
};
}
