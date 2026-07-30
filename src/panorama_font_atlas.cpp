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
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace panorama
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

void append_unique_root(std::vector<std::filesystem::path>& roots, std::filesystem::path root)
{
    if (root.empty())
    {
        return;
    }
    root = root.lexically_normal();
    if (std::find(roots.begin(), roots.end(), root) == roots.end())
    {
        roots.push_back(std::move(root));
    }
}

std::vector<std::filesystem::path> font_roots(const PanoramaFontAtlasLoadOptions& options)
{
    std::vector<std::filesystem::path> roots;
    for (const std::filesystem::path& directory : options.search_directories)
    {
        append_unique_root(roots, directory);
    }

    const std::filesystem::path normalized = options.resource_root.lexically_normal();
    if (!normalized.empty())
    {
        // Support a resource root that is itself a font directory as well as
        // generic standalone layouts and Valve's conventional content tree.
        append_unique_root(roots, normalized);
        append_unique_root(roots, normalized / "fonts");
        append_unique_root(roots, normalized / "ui/fonts");
        append_unique_root(roots, normalized / "resource/ui/fonts");
        append_unique_root(roots, normalized.parent_path() / "resource/ui/fonts");
        append_unique_root(roots, normalized / "csgo/resource/ui/fonts");
        append_unique_root(roots, normalized.parent_path().parent_path() / "csgo/resource/ui/fonts");
    }
    return roots;
}

// One resolved face source: either an on-disk path or bytes a host's
// PanoramaFontBytesProvider supplied. `refused` means the provider handled the face
// definitively and declined it, which must NOT degrade into an on-disk read.
struct ResolvedFaceSource
{
    std::string identity;
    std::filesystem::path path;
    std::vector<unsigned char> bytes;
    bool refused = false;

    [[nodiscard]] bool valid() const { return !path.empty() || !bytes.empty(); }
};

std::filesystem::path find_font_file_from_names(
    const std::vector<std::filesystem::path>& roots,
    const char* const* preferred_names,
    std::size_t preferred_count,
    bool allow_fallback)
{
    for (const std::filesystem::path& root : roots)
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

// The discovery preference order for each CSS weight bucket, shared by the on-disk probe and
// the host byte provider so the two can never resolve to different fonts.
constexpr const char* kRegularFontNames[] = {
    "Stratum2-Regular.ttf",
    "Stratum2-Medium.ttf",
    "NotoSans-Regular.ttf",
    "LatoLatin-Regular.ttf",
};
constexpr const char* kMediumFontNames[] = {
    "Stratum2-Medium.ttf",
    "NotoSans-Regular.ttf",
};
constexpr const char* kBoldFontNames[] = {
    "Stratum2-Bold.ttf",
    "NotoSans-Bold.ttf",
    "LatoLatin-Bold.ttf",
    "NotoSerif-Bold.ttf",
};

// Provider-first resolution of one weight's preferred face names. The provider is asked for
// each preferred name in the SAME preference order the on-disk probe below uses, so a
// package-served face and a loose file can never resolve to two different fonts.
//
// `provider_refused` is sticky for the whole load on purpose: once a host has definitively
// declined a face (its content policy blocks the compatibility source), the atlas must not
// reach the very same content through its own conventional-root discovery for a later name --
// that would silently route around the host's policy.
ResolvedFaceSource resolve_face_from_names(
    const PanoramaFontAtlasLoadOptions& options,
    const std::vector<std::filesystem::path>& roots,
    const char* const* preferred_names,
    std::size_t preferred_count,
    bool allow_fallback,
    bool& provider_refused)
{
    ResolvedFaceSource resolved;
    if (options.font_bytes_provider)
    {
        for (std::size_t i = 0; i < preferred_count; ++i)
        {
            std::optional<std::vector<unsigned char>> bytes =
                options.font_bytes_provider(preferred_names[i]);
            if (!bytes)
            {
                continue; // No opinion: keep looking, then fall through to the disk.
            }
            if (bytes->empty())
            {
                provider_refused = true;
                continue;
            }
            resolved.identity = preferred_names[i];
            resolved.bytes = std::move(*bytes);
            return resolved;
        }
    }
    if (provider_refused)
    {
        resolved.refused = true;
        return resolved;
    }

    const std::filesystem::path path =
        find_font_file_from_names(roots, preferred_names, preferred_count, allow_fallback);
    if (!path.empty())
    {
        resolved.path = path.lexically_normal();
        resolved.identity = resolved.path.generic_string();
    }
    return resolved;
}

std::filesystem::path resolve_configured_face(
    const PanoramaFontAtlasLoadOptions& options,
    const std::vector<std::filesystem::path>& roots,
    const std::filesystem::path& path)
{
    if (path.empty())
    {
        return {};
    }

    std::vector<std::filesystem::path> candidates;
    if (path.is_absolute())
    {
        candidates.push_back(path);
    }
    else
    {
        if (!options.resource_root.empty())
        {
            candidates.push_back(options.resource_root / path);
        }
        for (const std::filesystem::path& root : roots)
        {
            candidates.push_back(root / path);
        }
        // An explicitly supplied relative path with no root remains useful for
        // small command-line applications whose working directory is deliberate.
        candidates.push_back(path);
    }

    for (const std::filesystem::path& candidate : candidates)
    {
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error))
        {
            return candidate.lexically_normal();
        }
    }
    return {};
}

// Provider-first resolution of an EXPLICITLY configured face. The provider sees the configured
// spelling first and then the bare filename, because a host resolving through a content package
// keys on a logical resource identity, not on whichever root a loose probe would have hit.
ResolvedFaceSource resolve_configured_face_source(
    const PanoramaFontAtlasLoadOptions& options,
    const std::vector<std::filesystem::path>& roots,
    const std::filesystem::path& path,
    bool& provider_refused)
{
    ResolvedFaceSource resolved;
    if (path.empty())
    {
        return resolved;
    }
    if (options.font_bytes_provider)
    {
        const std::string configured = path.generic_string();
        const std::string filename = path.filename().generic_string();
        const char* const names[] = {configured.c_str(), filename.c_str()};
        const std::size_t name_count = configured == filename ? 1U : 2U;
        for (std::size_t i = 0; i < name_count; ++i)
        {
            std::optional<std::vector<unsigned char>> bytes = options.font_bytes_provider(names[i]);
            if (!bytes)
            {
                continue;
            }
            if (bytes->empty())
            {
                provider_refused = true;
                continue;
            }
            resolved.identity = names[i];
            resolved.bytes = std::move(*bytes);
            return resolved;
        }
    }
    if (provider_refused)
    {
        resolved.refused = true;
        return resolved;
    }

    const std::filesystem::path found = resolve_configured_face(options, roots, path);
    if (!found.empty())
    {
        resolved.path = found;
        resolved.identity = found.generic_string();
    }
    return resolved;
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
        // Stable dedupe/log identity: the normalized path for an on-disk face, the provider's
        // face name for a byte-provided one (which has no path at all).
        std::string identity;
        // Non-empty only for a provider-served face. FreeType does NOT copy the buffer passed
        // to FT_New_Memory_Face — its stream (and every extracted frame, including the whole
        // cmap and glyf tables) points straight into these bytes for the FT_Face's entire
        // lifetime. The two are therefore inseparable: FontFace is move-only, the move
        // transfers face+memory together, and the destructor closes the face. A copy would
        // duplicate the buffer at a NEW address while the FT_Face kept reading the doomed
        // original — exactly the vector-reallocation use-after-free that silently killed the
        // weight-500 face (std::move_if_noexcept fell back to the copy constructor).
        std::vector<unsigned char> memory;
        int weight = 400;
        int current_pixel_size = 0;
        struct MetricEntry
        {
            Metrics value;
            std::uint64_t last_use = 0;
        };
        std::unordered_map<int, MetricEntry> metrics;

        FontFace() = default;
        FontFace(const FontFace&) = delete;
        FontFace& operator=(const FontFace&) = delete;
        // unordered_map's move constructor is not noexcept (MSVC's allocates), which is what
        // pushed vector reallocation onto the copy path in the first place — swap instead,
        // which IS noexcept, so relocation is guaranteed to move.
        FontFace(FontFace&& other) noexcept
            : face(std::exchange(other.face, nullptr)),
              path(std::move(other.path)),
              identity(std::move(other.identity)),
              memory(std::move(other.memory)),
              weight(other.weight),
              current_pixel_size(other.current_pixel_size)
        {
            metrics.swap(other.metrics);
        }
        FontFace& operator=(FontFace&& other) noexcept
        {
            if (this != &other)
            {
                if (face != nullptr)
                {
                    FT_Done_Face(face);
                }
                face = std::exchange(other.face, nullptr);
                path = std::move(other.path);
                identity = std::move(other.identity);
                memory = std::move(other.memory);
                weight = other.weight;
                current_pixel_size = other.current_pixel_size;
                metrics.clear();
                metrics.swap(other.metrics);
            }
            return *this;
        }
        ~FontFace()
        {
            if (face != nullptr)
            {
                FT_Done_Face(face);
            }
        }
    };
    static_assert(!std::is_copy_constructible_v<FontFace> && std::is_nothrow_move_constructible_v<FontFace>,
        "FontFace lends its byte buffer to FreeType; it must never be copied and must relocate by move");

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
    bool dirty_full_page = false;
    bool atlas_full_logged = false;
    PanoramaTextureId texture = 0;
    PanoramaRenderBackend* texture_backend = nullptr;

    std::unordered_map<std::uint64_t, GlyphRecord> glyphs;
    std::vector<PanoramaFontAtlasDirtyRect> dirty_rects;
    PanoramaFontAtlasCacheBudgets budgets;
    bool counters_enabled = false;
    PanoramaFontAtlasStats counters;
    std::uint64_t cache_clock = 0;
    std::uint64_t metric_generation = 1;
    std::uint64_t atlas_page_generation = 1;
    std::uint64_t atlas_content_generation = 0;

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
        std::uint64_t generation = 0;

        bool operator==(const MeasureKey& other) const
        {
            return font_size == other.font_size && font_weight == other.font_weight &&
                   letter_spacing == other.letter_spacing && generation == other.generation && text == other.text;
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
            hash ^= std::hash<std::uint64_t>{}(key.generation) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
            return hash;
        }
    };
    struct MeasureEntry
    {
        std::pair<float, float> value;
        std::size_t bytes = 0;
        std::uint64_t last_use = 0;
    };
    std::unordered_map<MeasureKey, MeasureEntry, MeasureKeyHash> measure_cache;
    std::size_t measure_cache_bytes = 0;
    ~Impl()
    {
        release_texture();
        // FontFace's destructor closes each FT_Face; every face must be gone before the
        // library they were opened against is shut down.
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

    void record_dirty_rect(int x, int y, int width, int height)
    {
        if (width <= 0 || height <= 0)
        {
            return;
        }
        ++atlas_content_generation;
        dirty = true;
        if (counters_enabled)
        {
            counters.dirty_pixels += static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
        }
        if (dirty_full_page)
        {
            return;
        }
        PanoramaFontAtlasDirtyRect incoming{x, y, width, height};
        // Adjacent glyph rectangles in the same packed row become one upload
        // region; keep disjoint rows separate for a future regional backend.
        if (!dirty_rects.empty())
        {
            PanoramaFontAtlasDirtyRect& last = dirty_rects.back();
            const bool same_rows = last.y == incoming.y && last.height == incoming.height;
            if (same_rows && incoming.x <= last.x + last.width + kGlyphPadding)
            {
                const int right = std::max(last.x + last.width, incoming.x + incoming.width);
                last.x = std::min(last.x, incoming.x);
                last.width = right - last.x;
                return;
            }
        }
        dirty_rects.push_back(incoming);
    }

    void mark_full_page_dirty()
    {
        dirty = true;
        dirty_full_page = true;
        dirty_rects.clear();
        dirty_rects.push_back({0, 0, atlas_size, atlas_size});
        ++atlas_content_generation;
        if (counters_enabled)
        {
            counters.dirty_pixels +=
                static_cast<std::uint64_t>(atlas_size) * static_cast<std::uint64_t>(atlas_size);
        }
    }

    void reset_atlas_page()
    {
        std::fill(atlas_rgba.begin(), atlas_rgba.end(), 0);
        glyphs.clear();
        pen_x = kGlyphPadding;
        pen_y = kGlyphPadding;
        row_height = 0;
        atlas_full_logged = false;
        ++atlas_page_generation;
        if (counters_enabled)
        {
            ++counters.atlas_page_resets;
        }
        mark_full_page_dirty();
    }

    void evict_measure_cache_to_budget()
    {
        while ((!measure_cache.empty() && measure_cache.size() > budgets.measure_entries) ||
            (!measure_cache.empty() && measure_cache_bytes > budgets.measure_bytes))
        {
            const auto oldest = std::min_element(measure_cache.begin(), measure_cache.end(),
                [](const auto& a, const auto& b) { return a.second.last_use < b.second.last_use; });
            if (oldest == measure_cache.end())
            {
                break;
            }
            measure_cache_bytes -= oldest->second.bytes;
            measure_cache.erase(oldest);
            if (counters_enabled)
            {
                ++counters.measure_evictions;
            }
        }
    }

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
            it->second.last_use = ++cache_clock;
            return it->second.value;
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
        if (budgets.metrics_per_face == 0)
        {
            return result;
        }
        if (font->metrics.size() >= budgets.metrics_per_face)
        {
            const auto oldest = std::min_element(font->metrics.begin(), font->metrics.end(),
                [](const auto& a, const auto& b) { return a.second.last_use < b.second.last_use; });
            if (oldest != font->metrics.end())
            {
                font->metrics.erase(oldest);
                if (counters_enabled)
                {
                    ++counters.metrics_evictions;
                }
            }
        }
        font->metrics.emplace(size, FontFace::MetricEntry{result, ++cache_clock});
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
        mark_full_page_dirty();
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
        record_dirty_rect(record.atlas_x, record.atlas_y, width, height);
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
            if (counters_enabled)
            {
                ++counters.glyph_hits;
            }
            return &it->second;
        }
        if (counters_enabled)
        {
            ++counters.glyph_misses;
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
        key.generation = metric_generation;
        if (const auto cached = measure_cache.find(key); cached != measure_cache.end())
        {
            cached->second.last_use = ++cache_clock;
            if (counters_enabled)
            {
                ++counters.measure_hits;
            }
            return cached->second.value;
        }
        if (counters_enabled)
        {
            ++counters.measure_misses;
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
        if (budgets.measure_entries != 0 && budgets.measure_bytes != 0)
        {
            const std::size_t entry_bytes = sizeof(MeasureEntry) + sizeof(MeasureKey) + key.text.capacity();
            auto [inserted, was_inserted] =
                measure_cache.emplace(std::move(key), MeasureEntry{result, entry_bytes, ++cache_clock});
            if (was_inserted)
            {
                measure_cache_bytes += inserted->second.bytes;
            }
            evict_measure_cache_to_budget();
        }
        return result;
    }

    float shape_text(std::string_view text, float font_size, int font_weight, float letter_spacing,
        std::vector<PanoramaTextShapedGlyph>& shaped)
    {
        const int size = pixel_size(font_size);
        const Metrics metric = metrics_for(font_size, font_weight);
        FontFace* font = face_for_weight(font_weight);
        const float letter_spacing_px = letter_spacing * ui_scale;
        const float safe_scale = std::max(0.1F, ui_scale);
        FT_UInt previous_index = 0;
        std::size_t offset = 0;
        while (offset < text.size())
        {
            const std::size_t begin = offset;
            const char32_t codepoint = next_codepoint(text, offset);
            if (codepoint == '\n')
            {
                shaped.push_back({codepoint, begin, offset, 0.0F, 0.0F});
                previous_index = 0;
                continue;
            }
            const GlyphRecord* glyph = ensure_glyph_record(codepoint, font_size, font_weight);
            if (glyph == nullptr)
            {
                shaped.push_back({codepoint, begin, offset, 0.0F, 0.0F});
                previous_index = 0;
                continue;
            }
            float kerning_px = 0.0F;
            if (font != nullptr && FT_HAS_KERNING(font->face) && previous_index != 0 &&
                glyph->glyph_index != 0 && set_size(*font, size))
            {
                FT_Vector kern{};
                if (FT_Get_Kerning(font->face, previous_index, glyph->glyph_index, FT_KERNING_DEFAULT, &kern) == 0)
                {
                    kerning_px = static_cast<float>(kern.x) / 64.0F;
                }
            }
            shaped.push_back({codepoint, begin, offset,
                (kerning_px + glyph->advance_px + letter_spacing_px) / safe_scale,
                kerning_px / safe_scale});
            previous_index = glyph->glyph_index;
        }
        return metric.line_height_px / safe_scale;
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
            const std::span<const unsigned char> pixels(atlas_rgba.data(), atlas_rgba.size());
            if (texture != 0 && texture_backend == backend &&
                backend->update_texture(texture, pixels, atlas_size, atlas_size))
            {
                if (counters_enabled)
                {
                    ++counters.in_place_uploads;
                    counters.uploaded_bytes += atlas_rgba.size();
                }
                dirty = false;
                dirty_full_page = false;
                dirty_rects.clear();
                return;
            }

            const PanoramaTextureId new_texture =
                backend->generate_texture(pixels, atlas_size, atlas_size);
            if (new_texture == 0)
            {
                pano_log_warning("Panorama font atlas: backend GenerateTexture failed");
                return;
            }

            if (texture != 0 && counters_enabled)
            {
                ++counters.texture_replacements;
            }
            release_texture();
            texture = new_texture;
            texture_backend = backend;
            if (counters_enabled)
            {
                ++counters.full_uploads;
                counters.uploaded_bytes += atlas_rgba.size();
            }
            dirty = false;
            dirty_full_page = false;
            dirty_rects.clear();
            return;
        }

    }
};

PanoramaFontAtlas::PanoramaFontAtlas() : impl_(std::make_unique<Impl>()) {}

PanoramaFontAtlas::~PanoramaFontAtlas() = default;

bool PanoramaFontAtlas::load(const std::filesystem::path& resource_root)
{
    PanoramaFontAtlasLoadOptions options;
    options.resource_root = resource_root;
    return load(options);
}

bool PanoramaFontAtlas::load(const PanoramaFontAtlasLoadOptions& options)
{
    if (impl_->loaded())
    {
        return true;
    }

    const std::vector<std::filesystem::path> roots = font_roots(options);
    // Sticky across every face this load resolves -- see resolve_face_from_names.
    bool provider_refused = false;
    struct PlannedFace
    {
        ResolvedFaceSource source;
        int weight = 400;
    };
    std::vector<PlannedFace> faces;
    if (!options.faces.empty())
    {
        faces.reserve(options.faces.size());
        for (const PanoramaFontAtlasFace& configured : options.faces)
        {
            ResolvedFaceSource resolved = resolve_configured_face_source(
                options, roots, configured.path, provider_refused);
            if (!resolved.valid())
            {
                if (resolved.refused)
                {
                    pano_log_warning(
                        "Panorama font atlas: configured font '{}' was refused by the host font "
                        "provider",
                        configured.path.generic_string());
                }
                else
                {
                    pano_log_warning(
                        "Panorama font atlas: configured font '{}' was not found",
                        configured.path.generic_string());
                }
                continue;
            }
            faces.push_back(
                PlannedFace{std::move(resolved), std::clamp(configured.weight, 1, 1000)});
        }
    }
    else
    {
        // Same names, same order, same allow_fallback policy the path-only discovery used:
        // resolve_face_from_names asks the provider first and then runs the identical on-disk
        // probe over kRegular/kMedium/kBoldFontNames.
        ResolvedFaceSource regular = resolve_face_from_names(options, roots, kRegularFontNames,
            sizeof(kRegularFontNames) / sizeof(kRegularFontNames[0]), true, provider_refused);
        if (regular.valid())
        {
            faces.push_back(PlannedFace{std::move(regular), 400});
        }
        ResolvedFaceSource medium = resolve_face_from_names(options, roots, kMediumFontNames,
            sizeof(kMediumFontNames) / sizeof(kMediumFontNames[0]), false, provider_refused);
        if (medium.valid())
        {
            faces.push_back(PlannedFace{std::move(medium), 500});
        }
        ResolvedFaceSource bold = resolve_face_from_names(options, roots, kBoldFontNames,
            sizeof(kBoldFontNames) / sizeof(kBoldFontNames[0]), false, provider_refused);
        if (bold.valid())
        {
            faces.push_back(PlannedFace{std::move(bold), 700});
        }
    }

    if (faces.empty())
    {
        if (provider_refused)
        {
            pano_log_warning(
                "Panorama font atlas: every candidate face was refused by the host font "
                "provider; no on-disk fallback was attempted");
        }
        else
        {
            pano_log_warning(
                "Panorama font atlas: no font found for resource root '{}'",
                options.resource_root.lexically_normal().generic_string());
        }
        return false;
    }

    if (FT_Init_FreeType(&impl_->library) != 0)
    {
        pano_log_warning("Panorama font atlas: FT_Init_FreeType failed");
        return false;
    }

    const auto load_face = [&](ResolvedFaceSource source, int weight) {
        if (!source.valid())
        {
            return false;
        }
        for (const PanoramaFontAtlas::Impl::FontFace& font : impl_->faces)
        {
            if (font.identity == source.identity && font.weight == weight)
            {
                return true;
            }
        }

        PanoramaFontAtlas::Impl::FontFace font;
        font.identity = source.identity;
        font.weight = weight;
        FT_Face face = nullptr;
        if (!source.bytes.empty())
        {
            // Package-served bytes: FreeType borrows the buffer, so the FontFace owns it for
            // the FT_Face's whole lifetime (mirrors FFreeTypeFace::Memory in Unreal's
            // FontCacheFreeType.cpp:445-455).
            font.memory = std::move(source.bytes);
            if (FT_New_Memory_Face(impl_->library, font.memory.data(),
                    static_cast<FT_Long>(font.memory.size()), 0, &face) != 0)
            {
                pano_log_warning(
                    "Panorama font atlas: failed to load provided face '{}'", source.identity);
                return false;
            }
        }
        else
        {
            const std::filesystem::path normalized = source.path.lexically_normal();
            const std::string font_path_string = normalized.string();
            if (FT_New_Face(impl_->library, font_path_string.c_str(), 0, &face) != 0)
            {
                pano_log_warning(
                    "Panorama font atlas: failed to load '{}'", normalized.generic_string());
                return false;
            }
            font.path = normalized;
        }

        font.face = face;
        impl_->faces.push_back(std::move(font));
        // A face can open successfully yet be unusable (no charmap => every lookup returns
        // glyph 0 and its text silently collapses) — surface glyph/charmap counts here so a
        // dead face is visible at load instead of as blank labels later.
        pano_log_info("Panorama font atlas: loaded '{}' for weight {} ({} glyphs, {} charmaps)",
            source.identity, weight, static_cast<long>(face->num_glyphs), face->num_charmaps);
        return true;
    };

    bool loaded_any = false;
    for (PlannedFace& face : faces)
    {
        loaded_any = load_face(std::move(face.source), face.weight) || loaded_any;
    }
    if (!loaded_any)
    {
        FT_Done_FreeType(impl_->library);
        impl_->library = nullptr;
        return false;
    }
    return true;
}

void PanoramaFontAtlas::clear()
{
    const float ui_scale = impl_->ui_scale;
    impl_ = std::make_unique<Impl>();
    impl_->ui_scale = ui_scale;
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
        // Pixel sizes, advances, hinting and UV residency all change with the
        // scale. Retain old width entries under their metric generation until
        // the LRU budget evicts them; reclaim the single atlas page immediately
        // so old pixel-size glyphs cannot strand its fixed memory.
        impl_->ui_scale = clamped;
        ++impl_->metric_generation;
        impl_->reset_atlas_page();
    }
}

void PanoramaFontAtlas::set_cache_budgets(PanoramaFontAtlasCacheBudgets budgets)
{
    impl_->budgets = budgets;
    impl_->evict_measure_cache_to_budget();
    for (Impl::FontFace& face : impl_->faces)
    {
        while (face.metrics.size() > budgets.metrics_per_face)
        {
            const auto oldest = std::min_element(face.metrics.begin(), face.metrics.end(),
                [](const auto& a, const auto& b) { return a.second.last_use < b.second.last_use; });
            if (oldest == face.metrics.end())
            {
                break;
            }
            face.metrics.erase(oldest);
            if (impl_->counters_enabled)
            {
                ++impl_->counters.metrics_evictions;
            }
        }
    }
}

PanoramaFontAtlasCacheBudgets PanoramaFontAtlas::cache_budgets() const
{
    return impl_->budgets;
}

void PanoramaFontAtlas::enable_counters(bool enabled)
{
    impl_->counters_enabled = enabled;
}

void PanoramaFontAtlas::reset_counters()
{
    impl_->counters = {};
}

PanoramaFontAtlasStats PanoramaFontAtlas::stats() const
{
    return impl_->counters;
}

std::uint64_t PanoramaFontAtlas::metric_generation() const
{
    return impl_->metric_generation;
}

std::uint64_t PanoramaFontAtlas::atlas_page_generation() const
{
    return impl_->atlas_page_generation;
}

std::uint64_t PanoramaFontAtlas::atlas_content_generation() const
{
    return impl_->atlas_content_generation;
}

int PanoramaFontAtlas::atlas_width() const
{
    return impl_->atlas_size;
}

int PanoramaFontAtlas::atlas_height() const
{
    return impl_->atlas_size;
}

std::span<const PanoramaFontAtlasDirtyRect> PanoramaFontAtlas::dirty_rects() const
{
    return impl_->dirty_rects;
}

PanoramaTextMeasure PanoramaFontAtlas::text_measure()
{
    PanoramaTextMeasure measure;
    measure.measure = [this](std::string_view text, float font_size, int font_weight, float letter_spacing) {
        return impl_->measure_text(text, font_size, font_weight, letter_spacing);
    };
    measure.shape = [this](std::string_view text, float font_size, int font_weight, float letter_spacing,
                        std::vector<PanoramaTextShapedGlyph>& glyphs) {
        return impl_->shape_text(text, font_size, font_weight, letter_spacing, glyphs);
    };
    measure.generation = [this] { return impl_->metric_generation; };
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
    source.generation = impl_->metric_generation;
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
