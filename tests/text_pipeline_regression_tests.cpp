#include "ui/panorama/panorama_font_atlas.hpp"
#include "ui/panorama/panorama_layout.hpp"
#include "ui/panorama/panorama_paint.hpp"
#include "ui/panorama/panorama_render_backend.hpp"
#include "ui/panorama/panorama_text_break.hpp"
#include "ui/panorama/panorama_text_edit.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
[[noreturn]] void fail(std::string_view message)
{
    std::cerr << "text pipeline regression: " << message << '\n';
    std::exit(1);
}

void expect(bool condition, std::string_view message)
{
    if (!condition)
    {
        fail(message);
    }
}

class TextureBackend final : public panorama::PanoramaRenderBackend
{
public:
    panorama::PanoramaTextureId generate_texture(
        std::span<const unsigned char> pixels, int width, int height) override
    {
        ++generates;
        uploaded_bytes += pixels.size();
        last_pixels.assign(pixels.begin(), pixels.end());
        last_width = width;
        last_height = height;
        return ++next_texture;
    }
    bool update_texture(panorama::PanoramaTextureId texture, std::span<const unsigned char> pixels,
        int width, int height) override
    {
        ++updates;
        uploaded_bytes += pixels.size();
        last_pixels.assign(pixels.begin(), pixels.end());
        return texture != 0 && width == last_width && height == last_height;
    }
    void release_texture(panorama::PanoramaTextureId) override { ++releases; }
    panorama::PanoramaCompiledGeometryHandle compile_geometry(
        std::span<const panorama::PanoramaPaintVertex>, std::span<const int>, float) override
    {
        return 1;
    }
    void render_geometry(panorama::PanoramaCompiledGeometryHandle, panorama::PanoramaTextureId,
        const panorama::PanoramaDrawConstants&) override {}
    void release_geometry(panorama::PanoramaCompiledGeometryHandle) override {}

    std::size_t generates = 0;
    std::size_t updates = 0;
    std::size_t releases = 0;
    std::size_t uploaded_bytes = 0;
    std::vector<unsigned char> last_pixels;
    int last_width = 0;
    int last_height = 0;
    panorama::PanoramaTextureId next_texture = 100;
};

struct BackendScope
{
    explicit BackendScope(panorama::PanoramaRenderBackend* backend)
    {
        previous = panorama::panorama_render_backend();
        panorama::set_panorama_render_backend(backend);
    }
    ~BackendScope() { panorama::set_panorama_render_backend(previous); }
    panorama::PanoramaRenderBackend* previous = nullptr;
};

void text_artifact_checks()
{
    using namespace panorama;
    panorama_set_text_artifact_cache_budgets({32, 256U * 1024U});
    panorama_enable_text_artifact_counters(true);
    panorama_reset_text_artifact_counters();

    PanoramaNode root;
    root.tag = root.tag_lower = "panel";
    root.computed.width = {PanoramaLengthType::Pixels, 96.0F};
    root.computed.height = {PanoramaLengthType::FitChildren, 0.0F};
    root.computed.text_shadow.present = true;
    root.computed.text_shadow.blur = 4.0F;
    root.text = "wrapped shadow text with repeated words";

    const PanoramaTextMeasure measure = default_text_measure();
    layout_panorama_tree(root, 96.0F, 200.0F, measure);
    const PanoramaTextArtifactStats warm = panorama_text_artifact_stats();
    expect(warm.builds >= 2 && warm.shaped_runs == 1 && warm.wrap_passes == 1,
        "warm layout did not create one shaped core plus one wrapped artifact");
    expect(!root.text_lines.empty(), "fixture did not wrap");

    layout_panorama_tree(root, 96.0F, 200.0F, measure);
    const PanoramaTextArtifactStats hot = panorama_text_artifact_stats();
    expect(hot.shaped_runs == warm.shaped_runs && hot.wrap_passes == warm.wrap_passes && hot.hits > warm.hits,
        "unchanged relayout repeated shaping or wrapping");

    std::size_t glyph_calls = 0;
    PanoramaGlyphSource glyphs;
    glyphs.generation = 1;
    glyphs.atlas_texture = 7;
    glyphs.ascent = [](float size, int) { return size * 0.8F; };
    glyphs.glyph = [&](char32_t codepoint, float size, int, PanoramaGlyph& glyph) {
        ++glyph_calls;
        glyph.advance = size * 0.5F;
        glyph.width = size * 0.4F;
        glyph.height = size;
        glyph.valid = codepoint != U' ';
        return true;
    };
    PanoramaDrawList draw;
    build_panorama_draw_list(draw, root, glyphs);
    expect(glyph_calls == root.text.size(),
        "shadow taps performed repeated atlas lookup instead of reusing resolved glyphs");

    PanoramaNode entry;
    entry.tag = "TextEntry";
    entry.tag_lower = "textentry";
    entry.focused = true;
    entry.text = "caret-prefix";
    entry.computed.width = {PanoramaLengthType::Pixels, 160.0F};
    entry.computed.height = {PanoramaLengthType::Pixels, 30.0F};
    panorama_text_entry_set_selection(entry, 2, 8);
    layout_panorama_tree(entry, 160.0F, 30.0F, measure);
    build_panorama_draw_list(draw, entry, glyphs);
    expect(panorama_text_artifact_stats().prefix_queries >= 3,
        "caret/selection did not use cached prefix advances");
}

void default_utf8_measure_checks()
{
    using namespace panorama;
    const PanoramaTextMeasure measure = default_text_measure();
    const std::string text = "\xC3\xA9" "A"; // U+00E9 plus one ASCII glyph.
    const auto [width, height] = measure.measure(text, 10.0F, 400, 1.0F);
    expect(width == 12.0F && height == 12.0F,
        "default text measure counted UTF-8 bytes instead of codepoints");

    std::vector<PanoramaTextShapedGlyph> shaped;
    expect(measure.shape(text, 10.0F, 400, 1.0F, shaped) == 12.0F &&
            shaped.size() == 2 && shaped[0].begin == 0 && shaped[0].end == 2 &&
            shaped[1].begin == 2 && shaped[1].end == 3 &&
            shaped[0].advance == 6.0F && shaped[1].advance == 6.0F,
        "default UTF-8 shaping advance disagreed with codepoint measurement");
}

void kerning_geometry_checks()
{
    using namespace panorama;
    PanoramaTextMeasure measure;
    measure.measure = [](std::string_view, float, int, float) {
        return std::pair<float, float>{18.0F, 12.0F};
    };
    measure.shape = [](std::string_view text, float, int, float,
                        std::vector<PanoramaTextShapedGlyph>& glyphs) {
        expect(text == "AV", "kerning fixture shaped unexpected text");
        glyphs.push_back({U'A', 0, 1, 10.0F, 0.0F});
        // FreeType attaches the -2px A/V kerning to V's leading edge.
        glyphs.push_back({U'V', 1, 2, 8.0F, -2.0F});
        return 12.0F;
    };
    measure.generation = [] { return std::uint64_t{0xA11CE}; };

    PanoramaNode label;
    label.tag = label.tag_lower = "label";
    label.text = "AV";
    label.computed.width = {PanoramaLengthType::Pixels, 40.0F};
    label.computed.height = {PanoramaLengthType::Pixels, 20.0F};
    label.computed.font_size = 10.0F;
    label.computed.color = {255, 255, 255, 255};
    layout_panorama_tree(label, 40.0F, 20.0F, measure);

    PanoramaGlyphSource glyphs;
    glyphs.generation = 0xA11CE;
    glyphs.atlas_texture = 77;
    glyphs.ascent = [](float, int) { return 8.0F; };
    glyphs.glyph = [](char32_t, float, int, PanoramaGlyph& glyph) {
        glyph.advance = 10.0F;
        glyph.width = 2.0F;
        glyph.height = 4.0F;
        glyph.valid = true;
        return true;
    };

    PanoramaDrawList draw;
    build_panorama_draw_list(draw, label, glyphs);
    const auto command = std::find_if(draw.commands.begin(), draw.commands.end(),
        [](const PanoramaDrawCommand& candidate) { return candidate.texture == 77; });
    expect(command != draw.commands.end() && command->vertices.size() == 8,
        "kerning fixture did not emit two glyph quads");
    expect(command->vertices[0].x == 0.0F && command->vertices[4].x == 8.0F,
        "leading kerning changed measured width but did not position the current glyph quad");
}

void atlas_checks()
{
    using namespace panorama;
    PanoramaFontAtlas atlas;
    PanoramaFontAtlasLoadOptions options;
    const std::filesystem::path source_root = std::filesystem::path(__FILE__).parent_path().parent_path();
    options.faces.push_back(
        {source_root / "examples/04_window_raster/sample/resource/ui/fonts/LatoLatin-Regular.ttf", 400});
    expect(atlas.load(options), "font fixture did not load");
    atlas.set_cache_budgets({2, 512, 2});
    atlas.enable_counters(true);
    atlas.reset_counters();

    TextureBackend backend;
    BackendScope backend_scope(&backend);
    PanoramaTextMeasure measure = atlas.text_measure();
    (void)measure.measure("ASCII", 18.0F, 400, 0.0F);
    expect(!atlas.dirty_rects().empty() && atlas.dirty_rects().front().width < atlas.atlas_width(),
        "incremental ASCII arrival did not expose bounded dirty rectangles");
    atlas.upload_if_dirty();
    const PanoramaTextureId identity = atlas.glyph_source().atlas_texture;
    expect(identity != 0 && backend.generates == 1, "initial atlas upload failed");
    bool found_antialiased_texel = false;
    for (std::size_t i = 0; i + 3 < backend.last_pixels.size(); i += 4)
    {
        const unsigned char alpha = backend.last_pixels[i + 3];
        if (alpha > 0 && alpha < 255)
        {
            found_antialiased_texel = true;
            expect(backend.last_pixels[i + 0] == alpha &&
                    backend.last_pixels[i + 1] == alpha &&
                    backend.last_pixels[i + 2] == alpha,
                "glyph atlas coverage is not premultiplied RGBA");
            break;
        }
    }
    expect(found_antialiased_texel, "font fixture produced no antialiased atlas texel");

    (void)measure.measure("\xE4\xB8\xAD", 18.0F, 400, 0.0F);
    expect(!atlas.dirty_rects().empty(), "CJK/fallback glyph arrival did not dirty the atlas");
    atlas.upload_if_dirty();
    expect(atlas.glyph_source().atlas_texture == identity && backend.updates == 1 && backend.releases == 0,
        "same-size glyph arrival replaced atlas texture identity");

    (void)measure.measure("hot", 18.0F, 400, 0.0F);
    (void)measure.measure("cold-a", 19.0F, 400, 0.0F);
    (void)measure.measure("hot", 18.0F, 400, 0.0F);
    (void)measure.measure("cold-b", 20.0F, 400, 0.0F);
    (void)measure.measure("hot", 18.0F, 400, 0.0F);
    expect(atlas.stats().measure_hits >= 2 && atlas.stats().measure_evictions > 0,
        "budgeted width cache did not retain the hot entry");

    const std::uint64_t old_page = atlas.atlas_page_generation();
    atlas.set_ui_scale(1.5F);
    expect(atlas.atlas_page_generation() > old_page && atlas.dirty_rects().size() == 1 &&
            atlas.dirty_rects().front().width == atlas.atlas_width(),
        "DPI churn did not version and reclaim the atlas page");
    (void)measure.measure("ASCII", 18.0F, 400, 0.0F);
    atlas.upload_if_dirty();
    expect(atlas.glyph_source().atlas_texture == identity,
        "same-sized DPI page refresh did not preserve texture identity");
}

std::string make_text(std::size_t bytes)
{
    std::string value(bytes, 'a');
    if (bytes >= 3)
    {
        value.replace(0, 3, "\xE4\xB8\xAD");
    }
    return value;
}

void edit_checks()
{
    using namespace panorama;
    panorama_enable_text_edit_counters(true);
    for (const std::size_t size : {16U, 4096U, 1024U * 1024U})
    {
        PanoramaNode field;
        field.tag = "TextEntry";
        field.tag_lower = "textentry";
        const std::string initial = make_text(size);
        expect(panorama_text_entry_set_value(field, initial), "initial edit value was rejected");
        panorama_reset_text_edit_counters();

        const int middle = static_cast<int>(field.text.size() / 2U);
        panorama_text_entry_set_selection(field, middle, middle);
        std::string expected = field.text;
        expected.insert(static_cast<std::size_t>(middle), "Z");
        expect(panorama_text_entry_insert(field, "Z") && field.text == expected &&
                field.text_caret == middle + 1 && field.text_selection_anchor == middle + 1,
            "in-place insertion diverged from the reference trace");
        const PanoramaTextEditStats insert_stats = panorama_text_edit_stats();
        expect(insert_stats.in_place_edits == 1 && insert_stats.metadata_hits == 1 &&
                insert_stats.retained_bytes_scanned == 0,
            "ordinary insertion rescanned retained text");

        panorama_reset_text_edit_counters();
        expect(panorama_text_entry_delete(
                   field, PanoramaTextDirection::Backward, PanoramaTextGranularity::Character) &&
                field.text == initial && field.text_caret == middle,
            "in-place delete diverged from the reference trace");
        const PanoramaTextEditStats delete_stats = panorama_text_edit_stats();
        expect(delete_stats.in_place_edits == 1 && delete_stats.metadata_hits == 1 &&
                delete_stats.retained_bytes_scanned == 1,
            "ordinary delete scanned beyond the removed UTF-8 unit");
    }

    PanoramaNode capped;
    capped.tag = "TextEntry";
    capped.tag_lower = "textentry";
    capped.attributes["maxchars"] = "4";
    panorama_text_entry_set_value(capped, "\xE4\xB8\xADx");
    panorama_text_entry_insert(capped, "\r\nYZ");
    expect(capped.text == "\xE4\xB8\xADx Y" && capped.text_caret == 6,
        "normalization/maxchars UTF-8 semantics changed");
}
}

int main()
{
    text_artifact_checks();
    default_utf8_measure_checks();
    kerning_geometry_checks();
    atlas_checks();
    edit_checks();
    std::cout << "text pipeline regression checks passed\n";
    return 0;
}
