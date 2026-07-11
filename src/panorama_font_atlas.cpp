#include "ui/panorama/panorama_font_atlas.hpp"

#include "ui/panorama/panorama_log.hpp"
#include "ui/panorama/panorama_render_backend.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace openstrike
{
namespace
{
constexpr int kInitialAtlasSize = 1024;
constexpr int kMaxAtlasSize = 2048;
constexpr int kGlyphPadding = 1;

std::string lower_ascii(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text;
}

bool is_font_file(const std::filesystem::path& path)
{
    const std::string extension = lower_ascii(path.extension().string());
    return extension == ".ttf" || extension == ".otf";
}

std::vector<std::filesystem::path> font_roots_from_resource_root(const std::filesystem::path& resource_root)
{
    std::vector<std::filesystem::path> roots;
    const std::filesystem::path normalized = resource_root.lexically_normal();
    if (!normalized.empty())
    {
        roots.push_back(normalized.parent_path() / "resource/ui/fonts");
        roots.push_back(normalized / "resource/ui/fonts");
        roots.push_back(normalized / "csgo/resource/ui/fonts");
        roots.push_back(normalized.parent_path().parent_path() / "csgo/resource/ui/fonts");
    }
    roots.push_back(std::filesystem::path("openstrike/Content/csgo/resource/ui/fonts"));
    roots.push_back(std::filesystem::path("Content/csgo/resource/ui/fonts"));
    return roots;
}

std::filesystem::path find_font_file_from_names(
    const std::filesystem::path& resource_root,
    const char* const* preferred_names,
    std::size_t preferred_count,
    bool allow_fallback)
{
    for (const std::filesystem::path& root : font_roots_from_resource_root(resource_root))
    {
        std::error_code error;
        if (!std::filesystem::is_directory(root, error))
        {
            continue;
        }

        for (std::size_t i = 0; i < preferred_count; ++i)
        {
            const std::filesystem::path candidate = root / preferred_names[i];
            if (std::filesystem::is_regular_file(candidate, error))
            {
                return candidate;
            }
        }

        if (!allow_fallback)
        {
            continue;
        }

        std::vector<std::filesystem::path> fallback_fonts;
        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(root, error))
        {
            if (error)
            {
                break;
            }
            if (entry.is_regular_file(error) && is_font_file(entry.path()))
            {
                fallback_fonts.push_back(entry.path());
            }
        }
        if (!fallback_fonts.empty())
        {
            std::sort(fallback_fonts.begin(), fallback_fonts.end());
            return fallback_fonts.front();
        }
    }

    return {};
}

std::filesystem::path find_regular_font_file(const std::filesystem::path& resource_root)
{
    static constexpr const char* kPreferredFonts[] = {
        "Stratum2-Regular.ttf",
        "Stratum2-Medium.ttf",
        "NotoSans-Regular.ttf",
        "LatoLatin-Regular.ttf",
    };
    return find_font_file_from_names(
        resource_root, kPreferredFonts, sizeof(kPreferredFonts) / sizeof(kPreferredFonts[0]), true);
}

std::filesystem::path find_medium_font_file(const std::filesystem::path& resource_root)
{
    static constexpr const char* kPreferredFonts[] = {
        "Stratum2-Medium.ttf",
        "NotoSans-Regular.ttf",
    };
    return find_font_file_from_names(
        resource_root, kPreferredFonts, sizeof(kPreferredFonts) / sizeof(kPreferredFonts[0]), false);
}

std::filesystem::path find_bold_font_file(const std::filesystem::path& resource_root)
{
    static constexpr const char* kPreferredFonts[] = {
        "Stratum2-Bold.ttf",
        "NotoSans-Bold.ttf",
        "LatoLatin-Bold.ttf",
        "NotoSerif-Bold.ttf",
    };
    return find_font_file_from_names(
        resource_root, kPreferredFonts, sizeof(kPreferredFonts) / sizeof(kPreferredFonts[0]), false);
}

char32_t next_codepoint(std::string_view text, std::size_t& i)
{
    const auto byte = [&](std::size_t k) { return static_cast<unsigned char>(text[k]); };
    const unsigned char lead = byte(i);
    if (lead < 0x80)
    {
        ++i;
        return lead;
    }

    int extra = 0;
    char32_t cp = 0;
    if ((lead & 0xE0) == 0xC0)
    {
        extra = 1;
        cp = lead & 0x1F;
    }
    else if ((lead & 0xF0) == 0xE0)
    {
        extra = 2;
        cp = lead & 0x0F;
    }
    else if ((lead & 0xF8) == 0xF0)
    {
        extra = 3;
        cp = lead & 0x07;
    }
    else
    {
        ++i;
        return 0xFFFD;
    }

    if (i + static_cast<std::size_t>(extra) >= text.size())
    {
        i = text.size();
        return 0xFFFD;
    }
    for (int k = 0; k < extra; ++k)
    {
        const unsigned char cont = byte(i + 1 + static_cast<std::size_t>(k));
        if ((cont & 0xC0) != 0x80)
        {
            ++i;
            return 0xFFFD;
        }
        cp = (cp << 6) | (cont & 0x3F);
    }
    i += static_cast<std::size_t>(extra) + 1;
    return cp;
}

std::uint64_t glyph_key(char32_t codepoint, int pixel_size, int face_weight)
{
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(face_weight & 0x3FF)) << 54U) |
           (static_cast<std::uint64_t>(static_cast<std::uint32_t>(pixel_size & 0x3FFFFF)) << 32U) |
           static_cast<std::uint32_t>(codepoint);
}

unsigned char bitmap_alpha(const FT_Bitmap& bitmap, int x, int y)
{
    if (bitmap.buffer == nullptr || x < 0 || y < 0 || x >= static_cast<int>(bitmap.width) ||
        y >= static_cast<int>(bitmap.rows))
    {
        return 0;
    }

    const int pitch = bitmap.pitch;
    const unsigned char* row = pitch >= 0 ? bitmap.buffer + (y * pitch)
                                          : bitmap.buffer + ((static_cast<int>(bitmap.rows) - 1 - y) * -pitch);
    if (bitmap.pixel_mode == FT_PIXEL_MODE_GRAY)
    {
        return row[x];
    }
    if (bitmap.pixel_mode == FT_PIXEL_MODE_MONO)
    {
        return (row[x / 8] & (0x80 >> (x % 8))) != 0 ? 0xFF : 0x00;
    }
    return 0;
}
}

struct PanoramaFontAtlas::Impl
{
    struct GlyphRecord
    {
        float advance_px = 0.0F;
        float bearing_x_px = 0.0F;
        float bearing_y_px = 0.0F;
        float width_px = 0.0F;
        float height_px = 0.0F;
        int atlas_x = 0;
        int atlas_y = 0;
        int atlas_width = 0;
        int atlas_height = 0;
        float u0 = 0.0F;
        float v0 = 0.0F;
        float u1 = 0.0F;
        float v1 = 0.0F;
        FT_UInt glyph_index = 0;
        bool valid = false;
    };

    struct Metrics
    {
        float ascent_px = 0.0F;
        float line_height_px = 0.0F;
    };

    struct FontFace
    {
        FT_Face face = nullptr;
        std::filesystem::path path;
        int weight = 400;
        int current_pixel_size = 0;
        std::unordered_map<int, Metrics> metrics;
    };

    FT_Library library = nullptr;
    float ui_scale = 1.0F;
    std::vector<FontFace> faces;

    int atlas_size = kInitialAtlasSize;
    std::vector<unsigned char> atlas_rgba =
        std::vector<unsigned char>(static_cast<std::size_t>(kInitialAtlasSize) * static_cast<std::size_t>(kInitialAtlasSize) * 4U, 0);
    int pen_x = kGlyphPadding;
    int pen_y = kGlyphPadding;
    int row_height = 0;
    bool dirty = false;
    bool atlas_full_logged = false;
    PanoramaTextureId texture = 0;
    PanoramaRenderBackend* texture_backend = nullptr;

    std::unordered_map<std::uint64_t, GlyphRecord> glyphs;

    // Text-width cache (WebCore platform/graphics/WidthCache.h): the same label
    // strings are re-measured every layout pass (continuously while transitions
    // animate), but glyph advances/kerning only change with the UI scale — which
    // clears the cache (see set_ui_scale). Bounded by clear-on-cap.
    struct MeasureKey
    {
        std::string text;
        float font_size = 0.0F;
        int font_weight = 0;
        float letter_spacing = 0.0F;

        bool operator==(const MeasureKey& other) const
        {
            return font_size == other.font_size && font_weight == other.font_weight &&
                   letter_spacing == other.letter_spacing && text == other.text;
        }
    };
    struct MeasureKeyHash
    {
        std::size_t operator()(const MeasureKey& key) const
        {
            std::size_t hash = std::hash<std::string>{}(key.text);
            hash ^= std::hash<float>{}(key.font_size) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
            hash ^= std::hash<int>{}(key.font_weight) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
            hash ^= std::hash<float>{}(key.letter_spacing) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
            return hash;
        }
    };
    std::unordered_map<MeasureKey, std::pair<float, float>, MeasureKeyHash> measure_cache;
    ~Impl()
    {
        release_texture();
        for (FontFace& font : faces)
        {
            if (font.face != nullptr)
            {
                FT_Done_Face(font.face);
                font.face = nullptr;
            }
        }
        faces.clear();
        if (library != nullptr)
        {
            FT_Done_FreeType(library);
            library = nullptr;
        }
    }

    void release_texture()
    {
        if (texture == 0)
        {
            return;
        }
        if (texture_backend != nullptr && panorama_render_backend() == texture_backend)
        {
            texture_backend->release_texture(texture);
        }
        texture = 0;
        texture_backend = nullptr;
    }

    [[nodiscard]] bool loaded() const { return !faces.empty(); }

    [[nodiscard]] int pixel_size(float font_size) const
    {
        return std::max(1, static_cast<int>(std::lround(std::max(1.0F, font_size) * ui_scale)));
    }

    [[nodiscard]] FontFace* face_for_weight(int font_weight)
    {
        if (faces.empty())
        {
            return nullptr;
        }

        FontFace* best = &faces.front();
        int best_distance = std::abs(best->weight - font_weight);
        for (FontFace& font : faces)
        {
            const int distance = std::abs(font.weight - font_weight);
            if (distance < best_distance ||
                (distance == best_distance && font_weight >= 600 && font.weight > best->weight))
            {
                best = &font;
                best_distance = distance;
            }
        }
        return best;
    }

    [[nodiscard]] const FontFace* face_for_weight(int font_weight) const
    {
        if (faces.empty())
        {
            return nullptr;
        }

        const FontFace* best = &faces.front();
        int best_distance = std::abs(best->weight - font_weight);
        for (const FontFace& font : faces)
        {
            const int distance = std::abs(font.weight - font_weight);
            if (distance < best_distance ||
                (distance == best_distance && font_weight >= 600 && font.weight > best->weight))
            {
                best = &font;
                best_distance = distance;
            }
        }
        return best;
    }

    [[nodiscard]] bool set_size(FontFace& font, int size)
    {
        if (font.face == nullptr)
        {
            return false;
        }
        if (font.current_pixel_size == size)
        {
            return true;
        }
        if (FT_Set_Pixel_Sizes(font.face, 0, static_cast<FT_UInt>(size)) != 0)
        {
            return false;
        }
        font.current_pixel_size = size;
        return true;
    }

    [[nodiscard]] Metrics metrics_for(float font_size, int font_weight)
    {
        const int size = pixel_size(font_size);
        FontFace* font = face_for_weight(font_weight);
        if (font == nullptr)
        {
            Metrics result;
            result.ascent_px = static_cast<float>(size) * 0.8F;
            result.line_height_px = static_cast<float>(size) * 1.2F;
            return result;
        }
        if (const auto it = font->metrics.find(size); it != font->metrics.end())
        {
            return it->second;
        }

        Metrics result;
        if (set_size(*font, size) && font->face->size != nullptr)
        {
            result.ascent_px = static_cast<float>(font->face->size->metrics.ascender) / 64.0F;
            result.line_height_px = static_cast<float>(font->face->size->metrics.height) / 64.0F;
        }
        if (result.ascent_px <= 0.0F)
        {
            result.ascent_px = static_cast<float>(size) * 0.8F;
        }
        if (result.line_height_px <= 0.0F)
        {
            result.line_height_px = static_cast<float>(size) * 1.2F;
        }
        font->metrics.emplace(size, result);
        return result;
    }

    void update_record_uvs(GlyphRecord& record) const
    {
        if (!record.valid || atlas_size <= 0)
        {
            return;
        }
        const float scale = static_cast<float>(atlas_size);
        record.u0 = static_cast<float>(record.atlas_x) / scale;
        record.v0 = static_cast<float>(record.atlas_y) / scale;
        record.u1 = static_cast<float>(record.atlas_x + record.atlas_width) / scale;
        record.v1 = static_cast<float>(record.atlas_y + record.atlas_height) / scale;
    }

    bool grow_atlas()
    {
        if (atlas_size >= kMaxAtlasSize)
        {
            return false;
        }

        const int old_size = atlas_size;
        const int new_size = std::min(kMaxAtlasSize, atlas_size * 2);
        std::vector<unsigned char> resized(
            static_cast<std::size_t>(new_size) * static_cast<std::size_t>(new_size) * 4U, 0);
        const std::size_t old_row_bytes = static_cast<std::size_t>(old_size) * 4U;
        const std::size_t new_row_bytes = static_cast<std::size_t>(new_size) * 4U;
        for (int y = 0; y < old_size; ++y)
        {
            std::memcpy(resized.data() + static_cast<std::size_t>(y) * new_row_bytes,
                atlas_rgba.data() + static_cast<std::size_t>(y) * old_row_bytes,
                old_row_bytes);
        }

        atlas_size = new_size;
        atlas_rgba = std::move(resized);
        for (auto& [_, record] : glyphs)
        {
            update_record_uvs(record);
        }
        dirty = true;
        return true;
    }

    bool pack_bitmap(const FT_Bitmap& bitmap, GlyphRecord& record)
    {
        const int width = static_cast<int>(bitmap.width);
        const int height = static_cast<int>(bitmap.rows);
        if (width <= 0 || height <= 0)
        {
            return true;
        }
        while ((width + (2 * kGlyphPadding) > atlas_size || height + (2 * kGlyphPadding) > atlas_size) &&
            grow_atlas())
        {
        }
        if (width + (2 * kGlyphPadding) > atlas_size || height + (2 * kGlyphPadding) > atlas_size)
        {
            return false;
        }
        if (pen_x + width + kGlyphPadding > atlas_size)
        {
            pen_x = kGlyphPadding;
            pen_y += row_height + kGlyphPadding;
            row_height = 0;
        }
        while (pen_y + height + kGlyphPadding > atlas_size && grow_atlas())
        {
        }
        if (pen_y + height + kGlyphPadding > atlas_size)
        {
            return false;
        }

        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                const unsigned char a = bitmap_alpha(bitmap, x, y);
                const std::size_t dst =
                    (static_cast<std::size_t>(pen_y + y) * static_cast<std::size_t>(atlas_size) +
                        static_cast<std::size_t>(pen_x + x)) *
                    4U;
                atlas_rgba[dst + 0] = a;
                atlas_rgba[dst + 1] = a;
                atlas_rgba[dst + 2] = a;
                atlas_rgba[dst + 3] = a;
            }
        }

        record.atlas_x = pen_x;
        record.atlas_y = pen_y;
        record.atlas_width = width;
        record.atlas_height = height;
        update_record_uvs(record);
        pen_x += width + kGlyphPadding;
        row_height = std::max(row_height, height);
        dirty = true;
        return true;
    }

    const GlyphRecord* ensure_glyph_record(char32_t codepoint, float font_size, int font_weight)
    {
        FontFace* font = face_for_weight(font_weight);
        if (font == nullptr)
        {
            return nullptr;
        }

        const int size = pixel_size(font_size);
        const std::uint64_t key = glyph_key(codepoint, size, font->weight);
        if (const auto it = glyphs.find(key); it != glyphs.end())
        {
            return &it->second;
        }

        if (codepoint == '\n' || codepoint == '\r')
        {
            GlyphRecord record;
            const auto [it, _] = glyphs.emplace(key, record);
            return &it->second;
        }

        if (!set_size(*font, size))
        {
            return nullptr;
        }

        char32_t load_codepoint = codepoint;
        FT_UInt glyph_index = FT_Get_Char_Index(font->face, static_cast<FT_ULong>(load_codepoint));
        if (glyph_index == 0 && codepoint != U'?')
        {
            load_codepoint = U'?';
            glyph_index = FT_Get_Char_Index(font->face, static_cast<FT_ULong>(load_codepoint));
        }

        if (FT_Load_Char(font->face, static_cast<FT_ULong>(load_codepoint), FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL) != 0)
        {
            return nullptr;
        }

        FT_GlyphSlot slot = font->face->glyph;
        GlyphRecord record;
        record.advance_px = static_cast<float>(slot->advance.x) / 64.0F;
        record.bearing_x_px = static_cast<float>(slot->bitmap_left);
        record.bearing_y_px = static_cast<float>(slot->bitmap_top);
        record.width_px = static_cast<float>(slot->bitmap.width);
        record.height_px = static_cast<float>(slot->bitmap.rows);
        record.glyph_index = glyph_index;
        record.valid = slot->bitmap.width > 0 && slot->bitmap.rows > 0 && glyph_index != 0;

        if (record.valid && !pack_bitmap(slot->bitmap, record))
        {
            if (!atlas_full_logged)
            {
                pano_log_warning("Panorama font atlas is full; skipping additional glyphs");
                atlas_full_logged = true;
            }
            record.valid = false;
        }

        const auto [it, _] = glyphs.emplace(key, record);
        return &it->second;
    }

    const GlyphRecord* find_glyph_record(char32_t codepoint, float font_size, int font_weight) const
    {
        const FontFace* font = face_for_weight(font_weight);
        if (font == nullptr)
        {
            return nullptr;
        }
        const int size = pixel_size(font_size);
        const auto it = glyphs.find(glyph_key(codepoint, size, font->weight));
        return it != glyphs.end() ? &it->second : nullptr;
    }

    void ensure_text(std::string_view text, float font_size, int font_weight)
    {
        std::size_t i = 0;
        while (i < text.size())
        {
            ensure_glyph_record(next_codepoint(text, i), font_size, font_weight);
        }
    }

    std::pair<float, float> measure_text(std::string_view text, float font_size, int font_weight, float letter_spacing)
    {
        if (!loaded())
        {
            const float width = static_cast<float>(text.size()) * (font_size * 0.5F + letter_spacing);
            return {width, font_size * 1.2F};
        }

        // Width-cache fast path: identical (text, size, weight, spacing) repeats
        // every layout pass; advances and kerning only change with the UI scale,
        // which clears this cache.
        MeasureKey key;
        key.text.assign(text);
        key.font_size = font_size;
        key.font_weight = font_weight;
        key.letter_spacing = letter_spacing;
        if (const auto cached = measure_cache.find(key); cached != measure_cache.end())
        {
            return cached->second;
        }

        const float letter_spacing_px = letter_spacing * ui_scale;

        const int size = pixel_size(font_size);
        const Metrics metric = metrics_for(font_size, font_weight);
        FontFace* font = face_for_weight(font_weight);
        float line_width_px = 0.0F;
        float max_width_px = 0.0F;
        int lines = 1;
        FT_UInt previous_index = 0;

        std::size_t i = 0;
        while (i < text.size())
        {
            const char32_t cp = next_codepoint(text, i);
            if (cp == '\n')
            {
                max_width_px = std::max(max_width_px, line_width_px);
                line_width_px = 0.0F;
                previous_index = 0;
                ++lines;
                continue;
            }

            const GlyphRecord* glyph = ensure_glyph_record(cp, font_size, font_weight);
            if (glyph == nullptr)
            {
                continue;
            }
            if (font != nullptr && FT_HAS_KERNING(font->face) && previous_index != 0 && glyph->glyph_index != 0 &&
                set_size(*font, size))
            {
                FT_Vector kern{};
                if (FT_Get_Kerning(font->face, previous_index, glyph->glyph_index, FT_KERNING_DEFAULT, &kern) == 0)
                {
                    line_width_px += static_cast<float>(kern.x) / 64.0F;
                }
            }
            line_width_px += glyph->advance_px + letter_spacing_px;
            previous_index = glyph->glyph_index;
        }
        max_width_px = std::max(max_width_px, line_width_px);

        const float safe_scale = std::max(0.1F, ui_scale);
        const std::pair<float, float> result{
            max_width_px / safe_scale, (metric.line_height_px * static_cast<float>(lines)) / safe_scale};
        constexpr std::size_t kMeasureCacheMaxEntries = 8192;
        if (measure_cache.size() >= kMeasureCacheMaxEntries)
        {
            measure_cache.clear();
        }
        measure_cache.emplace(std::move(key), result);
        return result;
    }

    void ensure_tree_text(PanoramaNode& node)
    {
        if (!node.computed.visible)
        {
            return;
        }
        if (!node.text.empty())
        {
            std::string transformed_storage;
            const std::string_view display =
                panorama_transform_text_view(node.text, node.computed.text_transform, transformed_storage);
            node.shrink_font_size = 0.0F;
            if (node.is_html_text())
            {
                // html="true" labels render styled runs; rasterize each at the weight
                // the paint path will request (bold spans need the bold face glyphs in
                // the atlas, or find_glyph_record would miss them).
                for (const PanoramaTextRun& run : panorama_parse_inline_markup(display))
                {
                    ensure_text(run.text, node.computed.font_size,
                        panorama_run_font_weight(node.computed.font_weight, run.bold));
                }
            }
            else
            {
                float font = node.computed.font_size;
                // text-overflow: shrink — reduce the font so the text fits, then store the
                // exact size so paint renders at the very glyphs we rasterize here.
                if (node.computed.text_overflow == PanoramaTextOverflow::Shrink && node.layout.content_width > 0.0F)
                {
                    const auto [width, height] =
                        measure_text(display, font, node.computed.font_weight, node.computed.letter_spacing);
                    if (width > node.layout.content_width && width > 0.0F)
                    {
                        font = std::max(1.0F, font * node.layout.content_width / width);
                        node.shrink_font_size = font;
                    }
                }
                ensure_text(display, font, node.computed.font_weight);
                if (node.computed.text_overflow == PanoramaTextOverflow::Ellipsis)
                {
                    ensure_text("\xE2\x80\xA6", font, node.computed.font_weight); // U+2026, for truncation
                }
            }
        }
        for (auto& child : node.children)
        {
            ensure_tree_text(*child);
        }
    }

    void fill_panorama_glyph(const GlyphRecord& record, PanoramaGlyph& out) const
    {
        const float safe_scale = std::max(0.1F, ui_scale);
        out.advance = record.advance_px / safe_scale;
        out.bearing_x = record.bearing_x_px / safe_scale;
        out.bearing_y = record.bearing_y_px / safe_scale;
        out.width = record.width_px / safe_scale;
        out.height = record.height_px / safe_scale;
        out.u0 = record.u0;
        out.v0 = record.v0;
        out.u1 = record.u1;
        out.v1 = record.v1;
        out.valid = record.valid;
    }

    void upload_if_dirty()
    {
        if (!dirty)
        {
            return;
        }

        if (PanoramaRenderBackend* backend = panorama_render_backend())
        {
            const PanoramaTextureId new_texture =
                backend->generate_texture(std::span<const unsigned char>(atlas_rgba.data(), atlas_rgba.size()),
                    atlas_size,
                    atlas_size);
            if (new_texture == 0)
            {
                pano_log_warning("Panorama font atlas: backend GenerateTexture failed");
                return;
            }

            release_texture();
            texture = new_texture;
            texture_backend = backend;
            dirty = false;
            return;
        }

    }
};

PanoramaFontAtlas::PanoramaFontAtlas() : impl_(std::make_unique<Impl>()) {}

PanoramaFontAtlas::~PanoramaFontAtlas() = default;

bool PanoramaFontAtlas::load(const std::filesystem::path& resource_root)
{
    if (impl_->loaded())
    {
        return true;
    }

    const std::filesystem::path regular_path = find_regular_font_file(resource_root);
    if (regular_path.empty())
    {
        pano_log_warning("Panorama font atlas: no font found near '{}'", resource_root.lexically_normal().generic_string());
        return false;
    }

    if (FT_Init_FreeType(&impl_->library) != 0)
    {
        pano_log_warning("Panorama font atlas: FT_Init_FreeType failed");
        return false;
    }

    const auto load_face = [&](const std::filesystem::path& path, int weight, bool required) {
        if (path.empty())
        {
            return !required;
        }
        const std::filesystem::path normalized = path.lexically_normal();
        for (const PanoramaFontAtlas::Impl::FontFace& font : impl_->faces)
        {
            if (font.path == normalized)
            {
                return true;
            }
        }

        FT_Face face = nullptr;
        const std::string font_path_string = normalized.string();
        if (FT_New_Face(impl_->library, font_path_string.c_str(), 0, &face) != 0)
        {
            if (required)
            {
                pano_log_warning("Panorama font atlas: failed to load '{}'", normalized.generic_string());
            }
            return !required;
        }

        PanoramaFontAtlas::Impl::FontFace font;
        font.face = face;
        font.path = normalized;
        font.weight = weight;
        impl_->faces.push_back(std::move(font));
        pano_log_info("Panorama font atlas: loaded '{}' for weight {}", normalized.generic_string(), weight);
        return true;
    };

    if (!load_face(regular_path, 400, true))
    {
        FT_Done_FreeType(impl_->library);
        impl_->library = nullptr;
        return false;
    }
    (void)load_face(find_medium_font_file(resource_root), 500, false);
    (void)load_face(find_bold_font_file(resource_root), 700, false);
    return true;
}

bool PanoramaFontAtlas::loaded() const
{
    return impl_->loaded();
}

void PanoramaFontAtlas::set_ui_scale(float ui_scale)
{
    const float clamped = std::clamp(ui_scale, 0.1F, 8.0F);
    if (clamped != impl_->ui_scale)
    {
        // Pixel sizes, advances, and kerning all change with the scale.
        impl_->measure_cache.clear();
    }
    impl_->ui_scale = clamped;
}

PanoramaTextMeasure PanoramaFontAtlas::text_measure()
{
    PanoramaTextMeasure measure;
    measure.measure = [this](std::string_view text, float font_size, int font_weight, float letter_spacing) {
        return impl_->measure_text(text, font_size, font_weight, letter_spacing);
    };
    return measure;
}

void PanoramaFontAtlas::ensure_tree_text(PanoramaNode& root)
{
    impl_->ensure_tree_text(root);
}

void PanoramaFontAtlas::upload_if_dirty()
{
    impl_->upload_if_dirty();
}

PanoramaGlyphSource PanoramaFontAtlas::glyph_source() const
{
    PanoramaGlyphSource source;
    if (!impl_->loaded() || impl_->texture == 0)
    {
        return source;
    }

    source.atlas_texture = impl_->texture;
    source.ascent = [this](float font_size, int font_weight) {
        const Impl::Metrics metric = impl_->metrics_for(font_size, font_weight);
        return metric.ascent_px / std::max(0.1F, impl_->ui_scale);
    };
    source.glyph = [this](char32_t codepoint, float font_size, int font_weight, PanoramaGlyph& out) {
        const Impl::GlyphRecord* record = impl_->find_glyph_record(codepoint, font_size, font_weight);
        if (record == nullptr)
        {
            return false;
        }
        impl_->fill_panorama_glyph(*record, out);
        return true;
    };
    return source;
}
}
