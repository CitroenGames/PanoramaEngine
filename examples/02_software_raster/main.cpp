// PanoramaEngine example 02 — software rasterizer.
//
// Demonstrates that the engine's output is a fully renderer-agnostic display
// list: build_panorama_draw_list emits batches of coloured/textured triangles
// (PanoramaDrawCommand) that any backend can consume. Here the "backend" is a
// ~60-line CPU triangle rasterizer that alpha-blends the list into an RGBA
// buffer and writes hello_panorama.bmp — no GPU, no window, no host engine.
//
// A real host implements PanoramaRenderBackend instead (compile_geometry /
// render_geometry / generate_texture) and replays the same commands on the GPU.
#include "ui/panorama/panorama_document_session.hpp"
#include "ui/panorama/panorama_input.hpp"
#include "ui/panorama/panorama_layout.hpp"
#include "ui/panorama/panorama_paint.hpp"
#include "ui/panorama/panorama_resource_provider.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string_view>
#include <vector>

namespace
{
constexpr std::string_view kLayoutXml = R"xml(<root>
    <styles>
        <include src="file://{resources}/styles/raster.css" />
    </styles>
    <Panel id="Screen" class="screen">
        <Panel id="NavBar" class="nav">
            <Panel class="nav-button selected" />
            <Panel class="nav-button" />
            <Panel class="nav-button" />
        </Panel>
        <Panel id="Body" class="body">
            <Panel id="Sidebar" class="sidebar">
                <Panel class="tile" />
                <Panel class="tile" />
                <Panel class="tile accent" />
            </Panel>
            <Panel id="Card" class="card" />
        </Panel>
    </Panel>
</root>
)xml";

constexpr std::string_view kStyleCss = R"css(
.screen
{
    flow-children: down;
    width: 100%;
    height: 100%;
    background-color: #181b20ff;
}

.nav
{
    flow-children: right;
    width: 100%;
    height: 72px;
    padding: 12px;
    background-color: #101216ff;
}

.nav-button
{
    width: 120px;
    height: fill-parent-flow( 1.0 );
    margin-right: 10px;
    background-color: #262b33ff;
    border-radius: 6px;
}

.nav-button.selected
{
    background-color: #4a90d9ff;
}

.body
{
    flow-children: right;
    width: 100%;
    height: fill-parent-flow( 1.0 );
    padding: 16px;
}

.sidebar
{
    flow-children: down;
    width: 220px;
    height: 100%;
    margin-right: 16px;
}

.tile
{
    width: 100%;
    height: 90px;
    margin-bottom: 12px;
    background-color: #20242bff;
    border: 1px solid #303743ff;
    border-radius: 8px;
}

.tile.accent
{
    background-color: #2f7a4dff;
}

.card
{
    width: fill-parent-flow( 1.0 );
    height: 100%;
    background-color: #1d2027ff;
    border: 1px solid #343c49ff;
    border-radius: 12px;
    box-shadow: #00000066 4px 4px 12px 0px;
}
)css";

struct Framebuffer
{
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba; // 4 bytes per pixel

    void clear(std::uint8_t r, std::uint8_t g, std::uint8_t b)
    {
        for (int i = 0; i < width * height; ++i)
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
        std::uint8_t* px = &rgba[(y * width + x) * 4];
        const float inv = 1.0F - a;
        px[0] = static_cast<std::uint8_t>(r * a + static_cast<float>(px[0]) * inv);
        px[1] = static_cast<std::uint8_t>(g * a + static_cast<float>(px[1]) * inv);
        px[2] = static_cast<std::uint8_t>(b * a + static_cast<float>(px[2]) * inv);
    }
};

// Fills one triangle with per-vertex colour interpolation (barycentric).
void rasterize_triangle(
    Framebuffer& fb,
    const panorama::PanoramaPaintVertex& v0,
    const panorama::PanoramaPaintVertex& v1,
    const panorama::PanoramaPaintVertex& v2)
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
            const float r = w0 * v0.color.r + w1 * v1.color.r + w2 * v2.color.r;
            const float g = w0 * v0.color.g + w1 * v1.color.g + w2 * v2.color.g;
            const float b = w0 * v0.color.b + w1 * v1.color.b + w2 * v2.color.b;
            const float a = (w0 * v0.color.a + w1 * v1.color.a + w2 * v2.color.a) / 255.0F;
            fb.blend(x, y, r, g, b, a);
        }
    }
}

bool write_bmp(const Framebuffer& fb, const char* path)
{
    const int row_bytes = ((fb.width * 3 + 3) / 4) * 4; // rows padded to 4 bytes
    const std::uint32_t pixel_bytes = static_cast<std::uint32_t>(row_bytes) * static_cast<std::uint32_t>(fb.height);
    const std::uint32_t file_size = 54 + pixel_bytes;

    std::ofstream out(path, std::ios::binary);
    if (!out)
    {
        return false;
    }

    const auto put16 = [&out](std::uint16_t v) { out.put(static_cast<char>(v & 0xFF)).put(static_cast<char>(v >> 8)); };
    const auto put32 = [&out](std::uint32_t v) {
        for (int i = 0; i < 4; ++i)
        {
            out.put(static_cast<char>((v >> (i * 8)) & 0xFF));
        }
    };

    out.put('B').put('M');
    put32(file_size);
    put32(0);
    put32(54); // pixel data offset
    put32(40); // BITMAPINFOHEADER
    put32(static_cast<std::uint32_t>(fb.width));
    put32(static_cast<std::uint32_t>(fb.height));
    put16(1);
    put16(24);
    put32(0); // BI_RGB
    put32(pixel_bytes);
    put32(2835);
    put32(2835);
    put32(0);
    put32(0);

    std::vector<char> row(static_cast<std::size_t>(row_bytes), 0);
    for (int y = fb.height - 1; y >= 0; --y) // BMP rows are bottom-up
    {
        for (int x = 0; x < fb.width; ++x)
        {
            const std::uint8_t* px = &fb.rgba[(y * fb.width + x) * 4];
            row[x * 3 + 0] = static_cast<char>(px[2]); // B
            row[x * 3 + 1] = static_cast<char>(px[1]); // G
            row[x * 3 + 2] = static_cast<char>(px[0]); // R
        }
        out.write(row.data(), row_bytes);
    }
    return out.good();
}
}

int main()
{
    using namespace panorama;

    constexpr int kWidth = 960;
    constexpr int kHeight = 540;

    // Load + style + lay out, exactly as in example 01.
    PanoramaDocumentSession session;
    auto memory = std::make_unique<PanoramaMemoryResourceProvider>();
    memory->add_text("panorama/layout/raster.xml", kLayoutXml);
    memory->add_text("panorama/styles/raster.css", kStyleCss);
    session.resources().add_provider(std::move(memory));
    if (!session.load("panorama/layout/raster.xml"))
    {
        std::fprintf(stderr, "failed to load panorama/layout/raster.xml\n");
        return 1;
    }

    PanoramaNode& root = *session.document().root;
    session.style_sheet().compute(root);
    panorama_apply_visibility_overrides(root);
    panorama_apply_control_presentation(root);
    layout_panorama_tree(root, static_cast<float>(kWidth), static_cast<float>(kHeight));

    // Build the renderer-agnostic display list. Without a PanoramaGlyphSource,
    // text is skipped (panels still paint backgrounds/borders/shadows).
    const PanoramaDrawList draw_list = build_panorama_draw_list(root);
    std::printf(
        "draw list: %zu command(s), %zu vertices, %zu indices\n",
        draw_list.commands.size(),
        draw_list.total_vertices(),
        draw_list.total_indices());

    // "Render": replay the commands through the CPU rasterizer. Textured quads
    // (texture != 0) and scissor/blend state are ignored for brevity — a real
    // backend honours command.texture / blend_mode / scissor_*.
    Framebuffer fb{kWidth, kHeight, std::vector<std::uint8_t>(static_cast<std::size_t>(kWidth) * kHeight * 4)};
    fb.clear(0, 0, 0);
    for (const PanoramaDrawCommand& command : draw_list.commands)
    {
        if (command.texture != 0)
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
        // No PanoramaRenderBackend here (see the file header comment) -- this
        // walks command.vertices directly, so it must apply PanoramaDrawConstants
        // itself. Identity (an untransformed, fully-opaque command, or one that
        // went through the painter's legacy-bake fallback) is fast-pathed to
        // avoid a per-vertex transform on every frame.
        const bool identity_constants = command.constants.is_identity();
        for (std::size_t i = 0; i + 2 < command.indices.size(); i += 3)
        {
            const PanoramaPaintVertex& v0 = command.vertices[static_cast<std::size_t>(command.indices[i + 0])];
            const PanoramaPaintVertex& v1 = command.vertices[static_cast<std::size_t>(command.indices[i + 1])];
            const PanoramaPaintVertex& v2 = command.vertices[static_cast<std::size_t>(command.indices[i + 2])];
            if (identity_constants)
            {
                rasterize_triangle(fb, v0, v1, v2);
            }
            else
            {
                rasterize_triangle(fb, panorama_apply_draw_constants(v0, command.constants),
                    panorama_apply_draw_constants(v1, command.constants),
                    panorama_apply_draw_constants(v2, command.constants));
            }
        }
    }

    if (!write_bmp(fb, "hello_panorama.bmp"))
    {
        std::fprintf(stderr, "failed to write hello_panorama.bmp\n");
        return 1;
    }
    std::printf("wrote hello_panorama.bmp (%dx%d)\n", kWidth, kHeight);
    return 0;
}
