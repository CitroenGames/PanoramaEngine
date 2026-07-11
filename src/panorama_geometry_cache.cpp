#include "ui/panorama/panorama_geometry_cache.hpp"

#include <cmath>
#include <cstring>
#include <utility>

namespace panorama
{
namespace
{
// Word-at-a-time FNV-style mix: this hash is only an in-process cache key (a
// draw command's content signature), and per-byte FNV over every vertex of
// every draw command each animated frame is measurable. 8-byte chunks + tail.
void hash_bytes(std::uint64_t& hash, const void* data, std::size_t size)
{
    constexpr std::uint64_t kPrime = 1099511628211ULL;
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::size_t i = 0;
    for (; i + sizeof(std::uint64_t) <= size; i += sizeof(std::uint64_t))
    {
        std::uint64_t chunk = 0;
        std::memcpy(&chunk, bytes + i, sizeof(chunk));
        hash ^= chunk;
        hash *= kPrime;
    }
    for (; i < size; ++i)
    {
        hash ^= bytes[i];
        hash *= kPrime;
    }
}

template <typename T>
void hash_value(std::uint64_t& hash, const T& value)
{
    hash_bytes(hash, &value, sizeof(T));
}
}

std::uint64_t panorama_geometry_signature(const PanoramaDrawCommand& command, float ui_scale)
{
    std::uint64_t hash = 1469598103934665603ULL;
    hash_value(hash, ui_scale);
    hash_value(hash, command.texture);
    hash_value(hash, command.blend_mode);
    hash_value(hash, command.scissor);
    hash_value(hash, command.scissor_x);
    hash_value(hash, command.scissor_y);
    hash_value(hash, command.scissor_width);
    hash_value(hash, command.scissor_height);
    const std::size_t vertex_count = command.vertices.size();
    const std::size_t index_count = command.indices.size();
    hash_value(hash, vertex_count);
    hash_value(hash, index_count);
    // The vertex struct is tightly packed (4 floats + 4 colour bytes), so the
    // whole array hashes as one contiguous block -- no padding bytes exist to
    // poison the signature. The assert guards against future layout changes.
    static_assert(sizeof(PanoramaPaintVertex) == 4 * sizeof(float) + 4, "hash assumes no padding in PanoramaPaintVertex");
    if (!command.vertices.empty())
    {
        hash_bytes(hash, command.vertices.data(), command.vertices.size() * sizeof(PanoramaPaintVertex));
    }
    if (!command.indices.empty())
    {
        hash_bytes(hash, command.indices.data(), command.indices.size() * sizeof(int));
    }
    return hash;
}

PanoramaGeometryCache::~PanoramaGeometryCache()
{
    release();
}

bool PanoramaGeometryCache::replay(PanoramaRenderBackend& backend) const
{
    if (!valid_ || backend_ == nullptr || &backend != backend_)
    {
        return false;
    }
    for (const Entry& cached : entries_)
    {
        if (cached.is_blur())
        {
            backend.blur_region(static_cast<float>(cached.scissor_x), static_cast<float>(cached.scissor_y),
                static_cast<float>(cached.scissor_width), static_cast<float>(cached.scissor_height), cached.blur_std_x,
                cached.blur_std_y, cached.blur_passes);
            continue;
        }
        backend.set_scissor(cached.scissor, cached.scissor_x, cached.scissor_y, cached.scissor_width, cached.scissor_height);
        backend.set_blend_mode(cached.blend_mode);
        backend.render_geometry(cached.geometry, cached.texture);
    }
    backend.set_scissor(false, 0, 0, 0, 0);
    backend.set_blend_mode(PanoramaBlendMode::Normal);
    return true;
}

void PanoramaGeometryCache::submit(const PanoramaDrawList& draw_list, PanoramaRenderBackend& backend, float ui_scale)
{
    std::vector<Entry> previous_entries = std::move(entries_);
    PanoramaRenderBackend* previous_backend = backend_;
    const bool previous_valid = valid_;
    entries_.clear();
    entries_.reserve(draw_list.commands.size());
    backend_ = &backend;
    valid_ = false;

    const bool can_reuse_previous_backend = previous_valid && !previous_entries.empty() && previous_backend == &backend;

    bool cache_complete = true;
    std::size_t cache_index = 0;
    for (const PanoramaDrawCommand& command : draw_list.commands)
    {
        if (command.is_backdrop_blur())
        {
            // Backdrop blur: blur what is already rendered in the region (the
            // blurred panel's subtree just drew).
            backend.blur_region(command.scissor_x * ui_scale, command.scissor_y * ui_scale, command.scissor_width * ui_scale,
                command.scissor_height * ui_scale, command.blur_std_x * ui_scale, command.blur_std_y * ui_scale,
                command.blur_passes);
            Entry blur_entry{};
            blur_entry.blend_mode = command.blend_mode;
            blur_entry.scissor = true;
            blur_entry.scissor_x = static_cast<int>(std::lround(command.scissor_x * ui_scale));
            blur_entry.scissor_y = static_cast<int>(std::lround(command.scissor_y * ui_scale));
            blur_entry.scissor_width = static_cast<int>(std::lround(command.scissor_width * ui_scale));
            blur_entry.scissor_height = static_cast<int>(std::lround(command.scissor_height * ui_scale));
            blur_entry.blur_std_x = command.blur_std_x * ui_scale;
            blur_entry.blur_std_y = command.blur_std_y * ui_scale;
            blur_entry.blur_passes = command.blur_passes;
            entries_.push_back(blur_entry);
            ++cache_index; // keep the previous-entries cursor aligned (blur entries cache too)
            continue;
        }
        if (command.vertices.empty() || command.indices.empty())
        {
            continue;
        }

        // Geometry is compiled in framebuffer pixels (design * ui_scale), so the
        // clip rectangle is scaled the same way.
        const int sx = static_cast<int>(std::lround(command.scissor_x * ui_scale));
        const int sy = static_cast<int>(std::lround(command.scissor_y * ui_scale));
        const int sw = static_cast<int>(std::lround(command.scissor_width * ui_scale));
        const int sh = static_cast<int>(std::lround(command.scissor_height * ui_scale));
        backend.set_scissor(command.scissor, sx, sy, sw, sh);
        backend.set_blend_mode(command.blend_mode);
        const std::uint64_t signature = panorama_geometry_signature(command, ui_scale);

        Entry* reusable = nullptr;
        if (can_reuse_previous_backend && cache_index < previous_entries.size())
        {
            Entry& cached = previous_entries[cache_index];
            if (cached.signature == signature && cached.texture == command.texture &&
                cached.blend_mode == command.blend_mode && cached.scissor == command.scissor && cached.scissor_x == sx &&
                cached.scissor_y == sy && cached.scissor_width == sw && cached.scissor_height == sh &&
                cached.geometry != 0)
            {
                reusable = &cached;
            }
        }
        ++cache_index;

        if (reusable != nullptr)
        {
            backend.render_geometry(reusable->geometry, reusable->texture);
            entries_.push_back(*reusable);
            reusable->geometry = 0; // claimed: release_geometry_entries below must not free it
            continue;
        }

        const PanoramaCompiledGeometryHandle geometry = backend.compile_geometry(
            std::span<const PanoramaPaintVertex>(command.vertices.data(), command.vertices.size()),
            std::span<const int>(command.indices.data(), command.indices.size()), ui_scale);
        if (geometry != 0)
        {
            backend.render_geometry(geometry, command.texture);
            entries_.push_back(Entry{geometry, command.texture, command.blend_mode, command.scissor, sx, sy, sw, sh, signature});
            continue;
        }
        cache_complete = false;
    }

    if (!previous_entries.empty() && previous_backend != nullptr && previous_backend == panorama_render_backend())
    {
        for (const Entry& cached : previous_entries)
        {
            if (cached.geometry != 0)
            {
                previous_backend->release_geometry(cached.geometry);
            }
        }
    }
    backend.set_scissor(false, 0, 0, 0, 0); // clear so the clip never leaks into later rendering
    backend.set_blend_mode(PanoramaBlendMode::Normal);

    valid_ = cache_complete;
    if (!valid_)
    {
        release();
    }
}

void PanoramaGeometryCache::release()
{
    if (!entries_.empty() && backend_ != nullptr && backend_ == panorama_render_backend())
    {
        for (const Entry& cached : entries_)
        {
            if (cached.geometry != 0)
            {
                backend_->release_geometry(cached.geometry);
            }
        }
    }
    entries_.clear();
    backend_ = nullptr;
    valid_ = false;
}
}
