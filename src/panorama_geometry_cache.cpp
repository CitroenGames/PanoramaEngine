#include "ui/panorama/panorama_geometry_cache.hpp"

#include <chrono>
#include <cmath>
#include <cstring>
#include <unordered_map>
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

// Accumulates elapsed microseconds into *out on destruction; a null `out`
// (submit()'s `stats` parameter defaults to null) skips both clock reads, so a
// caller that never profiles pays nothing. Local to this file rather than a
// shared utility: this library stays host-independent (see
// PanoramaGeometrySubmitStats), so it cannot use a host's own timer type.
class ScopedMicroTimer
{
public:
    explicit ScopedMicroTimer(double* out) : out_(out)
    {
        if (out_ != nullptr)
        {
            start_ = std::chrono::steady_clock::now();
        }
    }

    ~ScopedMicroTimer()
    {
        if (out_ != nullptr)
        {
            *out_ += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start_).count();
        }
    }

    ScopedMicroTimer(const ScopedMicroTimer&) = delete;
    ScopedMicroTimer& operator=(const ScopedMicroTimer&) = delete;

private:
    double* out_;
    std::chrono::steady_clock::time_point start_{};
};
}

std::uint64_t panorama_geometry_signature(const PanoramaDrawCommand& command, float ui_scale)
{
    std::uint64_t hash = 1469598103934665603ULL;
    hash_value(hash, ui_scale);
    hash_value(hash, command.texture);
    hash_value(hash, command.blend_mode);
    // Scissor rect, blur params and PanoramaDrawConstants are deliberately NOT
    // hashed -- see this function's header comment.
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
            // Skip a blur whose enclosing layer context has faded to opacity
            // <= 0 (see submit()'s identical gate, PanoramaDrawCommand::
            // constants's comment, and paint()'s emit_blur_rect, which now
            // stamps the enclosing context's constants onto every blur
            // command): a fade-to-0 completion is classified visual_changed
            // (see PanoramaAnimationAdvanceResult's opacity_dead_on_completion
            // rule in panorama_anim.cpp), so this entry's constants reflect
            // the CURRENT opacity as of the repaint that produced it, not a
            // stale patched-through-the-fast-path value -- without this, a
            // frosted-glass panel that fades out keeps blurring forever.
            if (cached.constants.opacity <= 0.0F)
            {
                continue;
            }
            backend.blur_region(static_cast<float>(cached.scissor_x), static_cast<float>(cached.scissor_y),
                static_cast<float>(cached.scissor_width), static_cast<float>(cached.scissor_height), cached.blur_std_x,
                cached.blur_std_y, cached.blur_passes);
            continue;
        }
        backend.set_scissor(cached.scissor, cached.scissor_x, cached.scissor_y, cached.scissor_width, cached.scissor_height);
        backend.set_blend_mode(cached.blend_mode);
        backend.render_geometry(cached.geometry, cached.texture, cached.constants);
    }
    backend.set_scissor(false, 0, 0, 0, 0);
    backend.set_blend_mode(PanoramaBlendMode::Normal);
    return true;
}

void PanoramaGeometryCache::submit(
    const PanoramaDrawList& draw_list, PanoramaRenderBackend& backend, float ui_scale, PanoramaGeometrySubmitStats* stats)
{
    std::vector<Entry> previous_entries = std::move(entries_);
    PanoramaRenderBackend* previous_backend = backend_;
    const bool previous_valid = valid_;
    entries_.clear();
    entries_.reserve(draw_list.commands.size());
    backend_ = &backend;
    valid_ = false;

    const bool can_reuse_previous_backend = previous_valid && !previous_entries.empty() && previous_backend == &backend;

    // Local accumulators, folded into *stats at the end -- cheaper than
    // branching on `stats != nullptr` at every counter increment, and the
    // ScopedMicroTimer pointers below already make the actual clock reads free
    // when stats is null.
    double hash_us = 0.0;
    double compile_us = 0.0;
    double* const hash_us_out = stats != nullptr ? &hash_us : nullptr;
    double* const compile_us_out = stats != nullptr ? &compile_us : nullptr;
    int stat_commands = 0;
    int stat_reused = 0;
    int stat_recompiled = 0;
    std::size_t stat_uploaded_bytes = 0;

    // Signature -> previous_entries index, for commands that miss the
    // positional check below (a command inserted/removed earlier in the list
    // shifts every later command's position). Built lazily from whatever is
    // still unclaimed in previous_entries the FIRST time a positional check
    // misses -- most frames never touch it at all. Claim-once: an index is
    // erased the moment something reuses it, so two commands with an
    // identical signature (duplicate icon quads, say) never both claim the
    // same GPU handle.
    std::unordered_multimap<std::uint64_t, std::size_t> leftover_by_signature;
    bool leftover_indexed = false;

    bool cache_complete = true;
    std::size_t cache_index = 0;
    for (const PanoramaDrawCommand& command : draw_list.commands)
    {
        if (command.is_backdrop_blur())
        {
            // Backdrop blur: blur what is already rendered in the region (the
            // blurred panel's subtree just drew) -- unless its enclosing
            // layer context has faded to opacity <= 0 (see replay()'s
            // identical gate above): the geometry under it already skips
            // rendering via constants.opacity <= 0, so blurring nothing over
            // an invisible panel would only cost time.
            if (command.constants.opacity > 0.0F)
            {
                backend.blur_region(command.scissor_x * ui_scale, command.scissor_y * ui_scale,
                    command.scissor_width * ui_scale, command.scissor_height * ui_scale,
                    command.blur_std_x * ui_scale, command.blur_std_y * ui_scale, command.blur_passes);
            }
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
            blur_entry.blur_source_node = command.blur_source_node;
            // Mirror the enclosing layer context onto the entry (Slice 3
            // parity with geometry entries below) so a caller reading
            // entry_constants()/entry_context_index() -- and this class's own
            // opacity<=0 gates above -- see the same context data a geometry
            // command from the same paint() call would.
            blur_entry.constants = command.constants;
            blur_entry.constants_patchable = command.constants_patchable;
            blur_entry.context_index = command.context_index;
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
        std::uint64_t signature = 0;
        {
            const ScopedMicroTimer timer(hash_us_out);
            signature = panorama_geometry_signature(command, ui_scale);
        }
        ++stat_commands;

        Entry* reusable = nullptr;
        if (can_reuse_previous_backend && cache_index < previous_entries.size())
        {
            Entry& cached = previous_entries[cache_index];
            if (cached.signature == signature && cached.texture == command.texture &&
                cached.blend_mode == command.blend_mode && cached.geometry != 0)
            {
                reusable = &cached;
            }
        }
        ++cache_index;

        // Positional miss: the command's content still might match something
        // ELSEWHERE in the previous frame (a command earlier in the list was
        // inserted/removed, shifting everything after it by one slot). Index
        // whatever previous_entries are still unclaimed by signature -- lazily,
        // once -- and look this one up there before giving up and recompiling.
        if (reusable == nullptr && can_reuse_previous_backend)
        {
            if (!leftover_indexed)
            {
                leftover_indexed = true;
                leftover_by_signature.reserve(previous_entries.size());
                for (std::size_t i = 0; i < previous_entries.size(); ++i)
                {
                    if (previous_entries[i].geometry != 0)
                    {
                        leftover_by_signature.emplace(previous_entries[i].signature, i);
                    }
                }
            }
            const auto [first, last] = leftover_by_signature.equal_range(signature);
            for (auto it = first; it != last; ++it)
            {
                Entry& candidate = previous_entries[it->second];
                if (candidate.geometry != 0 && candidate.texture == command.texture &&
                    candidate.blend_mode == command.blend_mode)
                {
                    reusable = &candidate;
                    leftover_by_signature.erase(it);
                    break;
                }
            }
        }

        if (reusable != nullptr)
        {
            // Vertex/index content is unchanged (the signature match already
            // proved that), but scissor and PanoramaDrawConstants are mutable
            // per-entry state, not part of the signature -- update them in place
            // instead of recompiling (see panorama_geometry_signature's comment).
            reusable->scissor = command.scissor;
            reusable->scissor_x = sx;
            reusable->scissor_y = sy;
            reusable->scissor_width = sw;
            reusable->scissor_height = sh;
            reusable->constants = command.constants;
            reusable->constants_patchable = command.constants_patchable;
            reusable->context_index = command.context_index;
            backend.render_geometry(reusable->geometry, reusable->texture, reusable->constants);
            entries_.push_back(*reusable);
            reusable->geometry = 0; // claimed: release_geometry_entries below must not free it
            ++stat_reused;
            continue;
        }

        PanoramaCompiledGeometryHandle geometry = 0;
        {
            const ScopedMicroTimer timer(compile_us_out);
            geometry = backend.compile_geometry(
                std::span<const PanoramaPaintVertex>(command.vertices.data(), command.vertices.size()),
                std::span<const int>(command.indices.data(), command.indices.size()), ui_scale);
        }
        if (geometry != 0)
        {
            backend.render_geometry(geometry, command.texture, command.constants);
            entries_.push_back(Entry{
                .geometry = geometry,
                .texture = command.texture,
                .blend_mode = command.blend_mode,
                .scissor = command.scissor,
                .scissor_x = sx,
                .scissor_y = sy,
                .scissor_width = sw,
                .scissor_height = sh,
                .signature = signature,
                .constants = command.constants,
                .constants_patchable = command.constants_patchable,
                .context_index = command.context_index,
            });
            ++stat_recompiled;
            stat_uploaded_bytes += command.vertices.size() * sizeof(PanoramaPaintVertex) + command.indices.size() * sizeof(int);
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

    if (stats != nullptr)
    {
        stats->hash_us = hash_us;
        stats->compile_us = compile_us;
        stats->commands = stat_commands;
        stats->reused = stat_reused;
        stats->recompiled = stat_recompiled;
        stats->uploaded_bytes = stat_uploaded_bytes;
    }
}

int PanoramaGeometryCache::entry_context_index(std::size_t index) const
{
    return index < entries_.size() ? entries_[index].context_index : -1;
}

bool PanoramaGeometryCache::entry_constants_patchable(std::size_t index) const
{
    return index < entries_.size() && entries_[index].constants_patchable;
}

const PanoramaDrawConstants& PanoramaGeometryCache::entry_constants(std::size_t index) const
{
    static const PanoramaDrawConstants kIdentity{};
    return index < entries_.size() ? entries_[index].constants : kIdentity;
}

const PanoramaNode* PanoramaGeometryCache::entry_blur_source_node(std::size_t index) const
{
    return index < entries_.size() ? entries_[index].blur_source_node : nullptr;
}

void PanoramaGeometryCache::patch_constants(std::size_t index, const PanoramaDrawConstants& constants)
{
    if (index < entries_.size())
    {
        entries_[index].constants = constants;
    }
}

void PanoramaGeometryCache::patch_blur(std::size_t index, float std_x, float std_y, int passes)
{
    if (index < entries_.size() && entries_[index].is_blur())
    {
        entries_[index].blur_std_x = std_x;
        entries_[index].blur_std_y = std_y;
        entries_[index].blur_passes = passes;
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
