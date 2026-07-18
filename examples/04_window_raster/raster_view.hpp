#pragma once

// Shared by the Win32 (win32_main.cpp) and POSIX/X11 (posix_main.cpp) window
// hosts: loads a Panorama layout from a real XML file on disk, lays it out,
// and rasterizes the resulting draw list into an RGBA framebuffer entirely on
// the CPU. Its triangle fill is the optimized live-window counterpart to
// examples/02_software_raster, shared so both platform mains blit the same
// framebuffer instead of each hand-rolling their own.
#include "ui/panorama/panorama_font_atlas.hpp"
#include "ui/panorama/panorama_paint.hpp"
#include "ui/panorama/panorama_render_backend.hpp"
#include "ui/panorama/panorama_resource_provider.hpp"
#include "ui/panorama/panorama_view.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
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
        if (width == new_width && height == new_height)
        {
            return;
        }
        width = new_width;
        height = new_height;
        rgba.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
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
    void blend_unchecked(int x, int y, float r, float g, float b, float a)
    {
        if (a <= 0.0F)
        {
            return;
        }
        std::uint8_t* px =
            &rgba[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4];
        if (a >= 1.0F)
        {
            px[0] = to_byte(r);
            px[1] = to_byte(g);
            px[2] = to_byte(b);
            return;
        }
        const float inv = 1.0F - a;
        px[0] = to_byte(r * a + static_cast<float>(px[0]) * inv);
        px[1] = to_byte(g * a + static_cast<float>(px[1]) * inv);
        px[2] = to_byte(b * a + static_cast<float>(px[2]) * inv);
    }

private:
    static std::uint8_t to_byte(float value)
    {
        if (value >= 0.0F && value <= 255.0F)
        {
            return static_cast<std::uint8_t>(value);
        }
        return value < 0.0F ? 0 : 255;
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

struct RasterClip
{
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

struct RasterBenchmarkResult
{
    std::size_t frames = 0;
    double total_milliseconds = 0.0;
    double average_milliseconds = 0.0;
    double megapixels_per_second = 0.0;
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
    void render_geometry(panorama::PanoramaCompiledGeometryHandle, panorama::PanoramaTextureId,
        const panorama::PanoramaDrawConstants&) override
    {
    }
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
    const CpuTexture* texture,
    const RasterClip& clip)
{
    const float min_x = std::min({v0.x, v1.x, v2.x});
    const float max_x = std::max({v0.x, v1.x, v2.x});
    const float min_y = std::min({v0.y, v1.y, v2.y});
    const float max_y = std::max({v0.y, v1.y, v2.y});

    const float w0_dx_numerator = v1.y - v2.y;
    const float w0_dy_numerator = v2.x - v1.x;
    const float w1_dx_numerator = v2.y - v0.y;
    const float w1_dy_numerator = v0.x - v2.x;
    const float denom = w0_dx_numerator * (v0.x - v2.x) + w0_dy_numerator * (v0.y - v2.y);
    if (denom == 0.0F)
    {
        return; // degenerate
    }

    const int x0 = std::max(clip.left, static_cast<int>(min_x));
    const int x1 = std::min(clip.right, static_cast<int>(max_x));
    const int y0 = std::max(clip.top, static_cast<int>(min_y));
    const int y1 = std::min(clip.bottom, static_cast<int>(max_y));
    if (x0 > x1 || y0 > y1)
    {
        return;
    }

    // Edge functions are affine, so calculate their per-pixel increments once
    // instead of doing two barycentric divisions for every covered pixel.
    const float inv_denom = 1.0F / denom;
    const float w0_dx = w0_dx_numerator * inv_denom;
    const float w0_dy = w0_dy_numerator * inv_denom;
    const float w1_dx = w1_dx_numerator * inv_denom;
    const float w1_dy = w1_dy_numerator * inv_denom;
    const float start_px = static_cast<float>(x0) + 0.5F;
    const float start_py = static_cast<float>(y0) + 0.5F;
    float row_w0 = (w0_dx_numerator * (start_px - v2.x) + w0_dy_numerator * (start_py - v2.y)) * inv_denom;
    float row_w1 = (w1_dx_numerator * (start_px - v2.x) + w1_dy_numerator * (start_py - v2.y)) * inv_denom;

    const bool textured = texture != nullptr && texture->width > 0 && texture->height > 0;
    const float texture_width = textured ? static_cast<float>(texture->width) : 0.0F;
    const float texture_height = textured ? static_cast<float>(texture->height) : 0.0F;
    constexpr float kByteToUnit = 1.0F / 255.0F;

    // Interpolated attributes are affine too. Step colour/alpha/UV alongside
    // the edge functions rather than rebuilding each value from three weighted
    // vertices for every pixel.
    const auto attribute_dx = [w0_dx, w1_dx](float a0, float a1, float a2) {
        return w0_dx * (a0 - a2) + w1_dx * (a1 - a2);
    };
    const auto attribute_dy = [w0_dy, w1_dy](float a0, float a1, float a2) {
        return w0_dy * (a0 - a2) + w1_dy * (a1 - a2);
    };
    const auto attribute_start = [row_w0, row_w1](float a0, float a1, float a2) {
        return a2 + row_w0 * (a0 - a2) + row_w1 * (a1 - a2);
    };

    const float r_dx = attribute_dx(v0.color.r, v1.color.r, v2.color.r);
    const float g_dx = attribute_dx(v0.color.g, v1.color.g, v2.color.g);
    const float b_dx = attribute_dx(v0.color.b, v1.color.b, v2.color.b);
    const float a_dx = attribute_dx(v0.color.a, v1.color.a, v2.color.a) * kByteToUnit;
    const float u_dx = attribute_dx(v0.u, v1.u, v2.u);
    const float v_dx = attribute_dx(v0.v, v1.v, v2.v);
    const float r_dy = attribute_dy(v0.color.r, v1.color.r, v2.color.r);
    const float g_dy = attribute_dy(v0.color.g, v1.color.g, v2.color.g);
    const float b_dy = attribute_dy(v0.color.b, v1.color.b, v2.color.b);
    const float a_dy = attribute_dy(v0.color.a, v1.color.a, v2.color.a) * kByteToUnit;
    const float u_dy = attribute_dy(v0.u, v1.u, v2.u);
    const float v_dy = attribute_dy(v0.v, v1.v, v2.v);
    float row_r = attribute_start(v0.color.r, v1.color.r, v2.color.r);
    float row_g = attribute_start(v0.color.g, v1.color.g, v2.color.g);
    float row_b = attribute_start(v0.color.b, v1.color.b, v2.color.b);
    float row_a = attribute_start(v0.color.a, v1.color.a, v2.color.a) * kByteToUnit;
    float row_u = attribute_start(v0.u, v1.u, v2.u);
    float row_v = attribute_start(v0.v, v1.v, v2.v);

    for (int y = y0; y <= y1; ++y)
    {
        float w0 = row_w0;
        float w1 = row_w1;
        float r = row_r;
        float g = row_g;
        float b = row_b;
        float a = row_a;
        float u = row_u;
        float v = row_v;
        for (int x = x0; x <= x1; ++x)
        {
            const float w2 = 1.0F - w0 - w1;
            if (w0 >= 0.0F && w1 >= 0.0F && w2 >= 0.0F)
            {
                float sample_r = r;
                float sample_g = g;
                float sample_b = b;
                float sample_a = a;

                if (textured)
                {
                    const int tx = std::clamp(static_cast<int>(u * texture_width), 0, texture->width - 1);
                    const int ty = std::clamp(static_cast<int>(v * texture_height), 0, texture->height - 1);
                    const std::uint8_t* texel =
                        &texture->rgba[(static_cast<std::size_t>(ty) * static_cast<std::size_t>(texture->width) +
                                            static_cast<std::size_t>(tx)) *
                                        4];
                    sample_r *= static_cast<float>(texel[0]) * kByteToUnit;
                    sample_g *= static_cast<float>(texel[1]) * kByteToUnit;
                    sample_b *= static_cast<float>(texel[2]) * kByteToUnit;
                    sample_a *= static_cast<float>(texel[3]) * kByteToUnit;
                }

                fb.blend_unchecked(x, y, sample_r, sample_g, sample_b, sample_a);
            }
            w0 += w0_dx;
            w1 += w1_dx;
            r += r_dx;
            g += g_dx;
            b += b_dx;
            a += a_dx;
            u += u_dx;
            v += v_dx;
        }
        row_w0 += w0_dy;
        row_w1 += w1_dy;
        row_r += r_dy;
        row_g += g_dy;
        row_b += b_dy;
        row_a += a_dy;
        row_u += u_dy;
        row_v += v_dy;
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
        RasterClip clip{0, 0, fb.width - 1, fb.height - 1};
        if (command.scissor)
        {
            clip.left = std::max(clip.left, static_cast<int>(std::floor(command.scissor_x)));
            clip.top = std::max(clip.top, static_cast<int>(std::floor(command.scissor_y)));
            clip.right = std::min(
                clip.right, static_cast<int>(std::ceil(command.scissor_x + command.scissor_width)) - 1);
            clip.bottom = std::min(
                clip.bottom, static_cast<int>(std::ceil(command.scissor_y + command.scissor_height)) - 1);
        }
        if (clip.left > clip.right || clip.top > clip.bottom)
        {
            continue;
        }

        // Fully-faded (opacity <= 0, e.g. an animating-to-zero subtree the
        // painter keeps emitting so the draw list's shape holds steady across
        // the fade) draws nothing -- skip it instead of rasterizing invisible
        // triangles.
        if (command.constants.opacity <= 0.0F)
        {
            continue;
        }

        const CpuTexture* texture = command.texture != 0 ? textures.find(command.texture) : nullptr;
        // PanoramaGeometryCache is not used here (see CpuTextureStore's comment),
        // so this walks command.vertices directly and must apply its
        // PanoramaDrawConstants itself -- a GPU backend instead folds it into
        // per-draw shader state. Identity (an untransformed, fully-opaque
        // command, or one that went through the painter's legacy-bake
        // fallback) is fast-pathed to avoid a per-vertex transform on every
        // frame.
        const bool identity_constants = command.constants.is_identity();
        for (std::size_t i = 0; i + 2 < command.indices.size(); i += 3)
        {
            const panorama::PanoramaPaintVertex& v0 = command.vertices[static_cast<std::size_t>(command.indices[i + 0])];
            const panorama::PanoramaPaintVertex& v1 = command.vertices[static_cast<std::size_t>(command.indices[i + 1])];
            const panorama::PanoramaPaintVertex& v2 = command.vertices[static_cast<std::size_t>(command.indices[i + 2])];
            if (identity_constants)
            {
                rasterize_triangle(fb, v0, v1, v2, texture, clip);
            }
            else
            {
                rasterize_triangle(fb,
                    panorama::panorama_apply_draw_constants(v0, command.constants),
                    panorama::panorama_apply_draw_constants(v1, command.constants),
                    panorama::panorama_apply_draw_constants(v2, command.constants),
                    texture,
                    clip);
            }
        }
    }
}

// Owns the high-level PanoramaView and its current CPU-rasterized frame. The
// view performs dirty-stage tracking, so a host can pump scripts and animations
// at a steady cadence without rebuilding or rasterizing an unchanged surface.
class RasterDocument
{
public:
    ~RasterDocument()
    {
        view_.unload();
        font_atlas_.clear();
        if (panorama::panorama_render_backend() == &textures_)
        {
            panorama::set_panorama_render_backend(nullptr);
        }
    }

    // Loads `xml_path` off the real filesystem: the session's resource
    // provider is a PanoramaDirectoryResourceProvider rooted at the file's
    // parent directory, so a `<styles><include src="..."/>` in the document
    // resolves against sibling files next to it (see
    // examples/04_window_raster/sample/ for the layout this loads by
    // default). Returns false and logs to stderr if the document fails to
    // load or parses empty.
    [[nodiscard]] bool load(const std::filesystem::path& xml_path, int width, int height)
    {
        std::error_code error;
        const std::filesystem::path absolute = std::filesystem::absolute(xml_path, error);
        if (error)
        {
            std::fprintf(stderr, "invalid path: %s\n", xml_path.string().c_str());
            return false;
        }

        width = std::max(width, 1);
        height = std::max(height, 1);
        const std::filesystem::path resource_root = absolute.parent_path();

        // Text rendering needs an actual font file on disk; the engine
        // itself intentionally does not vendor one (see docs/architecture.md's
        // "Text" extension point), but this example bundles Lato (SIL OFL,
        // see sample/resource/ui/fonts/OFL.txt) under
        // `<resource_root>/resource/ui/fonts/` so it renders real glyphs out
        // of the box. Point
        // load() at a resource_root without a font under that path (e.g. a
        // custom layout.xml elsewhere) and it logs a warning instead. Production
        // callers can use PanoramaFontAtlasLoadOptions to name their own font
        // folders or weighted face files explicitly. Leaving the view unbound
        // from an atlas degrades to the same "panels paint, text is skipped"
        // behaviour as examples/02_software_raster.
        panorama::set_panorama_render_backend(&textures_);
        const bool font_loaded = font_atlas_.load(resource_root);
        if (!font_loaded)
        {
            std::fprintf(
                stderr,
                "no font found under %s -- text will not render (see raster_view.hpp)\n",
                (resource_root / "resource/ui/fonts").string().c_str());
        }

        view_.set_viewport(static_cast<float>(width), static_cast<float>(height));
        view_.set_font_atlas(font_loaded ? &font_atlas_ : nullptr);
        view_.resources().add_provider(
            std::make_unique<panorama::PanoramaDirectoryResourceProvider>(resource_root));

        panorama::PanoramaViewLoadOptions options;
        options.document.resource_root = resource_root;
        if (!view_.load(absolute.filename().string(), std::move(options)))
        {
            std::fprintf(stderr, "failed to load %s\n", absolute.string().c_str());
            return false;
        }

        rasterize(width, height);
        return true;
    }

    bool update_pointer(float mouse_x, float mouse_y, bool mouse_down)
    {
        return view_.update_pointer(mouse_x, mouse_y, mouse_down);
    }

    bool update_wheel(float mouse_x, float mouse_y, float wheel_ticks_y)
    {
        return view_.update_wheel(mouse_x, mouse_y, wheel_ticks_y);
    }

    bool handle_key_down(const panorama::PanoramaKeyEvent& event) { return view_.handle_key_down(event); }

    bool handle_text_input(std::string_view text) { return view_.handle_text_input(text); }

    // Pump runtime work every host frame, but only rebuild the CPU surface when
    // PanoramaView reports that style/layout/animation changed the draw list.
    [[nodiscard]] bool update_frame(int width, int height, float dt_seconds)
    {
        width = std::max(width, 1);
        height = std::max(height, 1);
        view_.set_viewport(static_cast<float>(width), static_cast<float>(height));
        const panorama::PanoramaViewUpdateResult result = view_.update(dt_seconds);
        if (!result.draw_list_rebuilt && framebuffer_.width == width && framebuffer_.height == height)
        {
            return false;
        }

        rasterize(width, height);
        return true;
    }

    [[nodiscard]] const Framebuffer& framebuffer() const noexcept { return framebuffer_; }

    // Headless, deterministic measurement for the example's CPU hot path. It
    // replays the already-built draw list so results cover clear + raster only,
    // independent of native window presentation and event-loop timing.
    [[nodiscard]] RasterBenchmarkResult benchmark_rasterizer(std::size_t frames)
    {
        frames = std::max<std::size_t>(frames, 1);
        const auto start = std::chrono::steady_clock::now();
        for (std::size_t frame = 0; frame < frames; ++frame)
        {
            rasterize(framebuffer_.width, framebuffer_.height);
        }
        const auto end = std::chrono::steady_clock::now();
        const double total_ms = std::chrono::duration<double, std::milli>(end - start).count();
        const double pixels = static_cast<double>(framebuffer_.width) * static_cast<double>(framebuffer_.height) *
            static_cast<double>(frames);
        return RasterBenchmarkResult{
            frames,
            total_ms,
            total_ms / static_cast<double>(frames),
            total_ms > 0.0 ? pixels / (total_ms * 1000.0) : 0.0,
        };
    }

private:
    void rasterize(int width, int height)
    {
        framebuffer_.resize(width, height);
        framebuffer_.clear(24, 27, 32);
        rasterize_draw_list(framebuffer_, view_.draw_list(), textures_);
    }

    CpuTextureStore textures_;
    panorama::PanoramaFontAtlas font_atlas_;
    panorama::PanoramaView view_;
    Framebuffer framebuffer_;
};
}
