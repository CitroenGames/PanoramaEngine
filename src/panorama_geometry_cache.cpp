#include "ui/panorama/panorama_geometry_cache.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
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

struct PreparedGeometryChunk
{
    std::vector<PanoramaPaintVertex> vertices;
    std::vector<int> indices;
    std::uint64_t signature = 0;
};

bool prepare_geometry_chunks(const PanoramaDrawCommand& command, float ui_scale,
    std::vector<PreparedGeometryChunk>& chunks, std::size_t* hashed_bytes)
{
    chunks.clear();
    chunks.reserve(panorama_geometry_chunk_count(command.indices.size()));

    for (std::size_t first_index = 0; first_index < command.indices.size();
         first_index += kPanoramaGeometryChunkIndexLimit)
    {
        const std::size_t end_index =
            std::min(first_index + kPanoramaGeometryChunkIndexLimit, command.indices.size());
        PreparedGeometryChunk chunk;
        chunk.indices.reserve(end_index - first_index);
        chunk.vertices.reserve(std::min(end_index - first_index, command.vertices.size()));

        std::unordered_map<int, int> local_indices;
        local_indices.reserve(end_index - first_index);
        for (std::size_t index_offset = first_index; index_offset < end_index; ++index_offset)
        {
            const int source_index = command.indices[index_offset];
            if (source_index < 0 ||
                static_cast<std::size_t>(source_index) >= command.vertices.size() ||
                chunk.vertices.size() >= static_cast<std::size_t>(std::numeric_limits<int>::max()))
            {
                chunks.clear();
                return false;
            }

            const auto [it, inserted] =
                local_indices.emplace(source_index, static_cast<int>(chunk.vertices.size()));
            if (inserted)
            {
                chunk.vertices.push_back(command.vertices[static_cast<std::size_t>(source_index)]);
            }
            chunk.indices.push_back(it->second);
        }

        chunk.signature = 1469598103934665603ULL;
        hash_value(chunk.signature, ui_scale);
        hash_value(chunk.signature, command.texture);
        hash_value(chunk.signature, command.blend_mode);
        const std::size_t vertex_count = chunk.vertices.size();
        const std::size_t index_count = chunk.indices.size();
        hash_value(chunk.signature, vertex_count);
        hash_value(chunk.signature, index_count);
        hash_bytes(chunk.signature, chunk.vertices.data(),
            chunk.vertices.size() * sizeof(PanoramaPaintVertex));
        hash_bytes(chunk.signature, chunk.indices.data(),
            chunk.indices.size() * sizeof(int));
        if (hashed_bytes != nullptr)
        {
            *hashed_bytes += chunk.vertices.size() * sizeof(PanoramaPaintVertex) +
                chunk.indices.size() * sizeof(int);
        }
        chunks.push_back(std::move(chunk));
    }
    return !chunks.empty();
}

std::uint64_t prepared_entry_signature(
    std::span<const PreparedGeometryChunk> chunks,
    PanoramaTextureId texture,
    PanoramaBlendMode blend_mode)
{
    std::uint64_t signature = 1469598103934665603ULL;
    hash_value(signature, texture);
    hash_value(signature, blend_mode);
    const std::size_t chunk_count = chunks.size();
    hash_value(signature, chunk_count);
    for (const PreparedGeometryChunk& chunk : chunks)
    {
        hash_value(signature, chunk.signature);
    }
    return signature;
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
    PanoramaDrawStateCache state;
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
        if (state.update_scissor(
                cached.scissor, cached.scissor_x, cached.scissor_y,
                cached.scissor_width, cached.scissor_height))
        {
            backend.set_scissor(
                cached.scissor, cached.scissor_x, cached.scissor_y,
                cached.scissor_width, cached.scissor_height);
        }
        if (state.update_blend(cached.blend_mode))
        {
            backend.set_blend_mode(cached.blend_mode);
        }
        for (const Chunk& chunk : cached.chunks)
        {
            backend.render_geometry(chunk.geometry, cached.texture, cached.constants);
        }
    }
    if (state.scissor_valid() && state.update_scissor(false, 0, 0, 0, 0))
    {
        backend.set_scissor(false, 0, 0, 0, 0);
    }
    if (state.blend_valid() && state.update_blend(PanoramaBlendMode::Normal))
    {
        backend.set_blend_mode(PanoramaBlendMode::Normal);
    }
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

    // Local accumulators, folded into *stats at the end.
    double hash_us = 0.0;
    double compile_us = 0.0;
    double* const hash_us_out = stats != nullptr ? &hash_us : nullptr;
    double* const compile_us_out = stats != nullptr ? &compile_us : nullptr;
    int stat_commands = 0;
    int stat_reused = 0;
    int stat_recompiled = 0;
    int stat_chunks = 0;
    int stat_reused_chunks = 0;
    int stat_recompiled_chunks = 0;
    std::size_t stat_hashed_bytes = 0;
    std::size_t stat_uploaded_bytes = 0;
    std::size_t stat_max_chunk_uploaded_bytes = 0;

    std::vector<bool> previous_claimed(previous_entries.size(), false);
    std::unordered_multimap<std::uint64_t, std::size_t> previous_by_signature;
    if (can_reuse_previous_backend)
    {
        previous_by_signature.reserve(previous_entries.size());
        for (std::size_t index = 0; index < previous_entries.size(); ++index)
        {
            if (!previous_entries[index].chunks.empty())
            {
                previous_by_signature.emplace(previous_entries[index].signature, index);
            }
        }
    }

    bool cache_complete = true;
    std::size_t cache_index = 0;
    PanoramaDrawStateCache state;
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
            if (cache_index < previous_claimed.size() && previous_entries[cache_index].is_blur())
            {
                previous_claimed[cache_index] = true;
            }
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
        std::vector<PreparedGeometryChunk> prepared_chunks;
        bool chunks_valid = false;
        {
            const ScopedMicroTimer timer(hash_us_out);
            chunks_valid = prepare_geometry_chunks(
                command, ui_scale, prepared_chunks,
                stats != nullptr ? &stat_hashed_bytes : nullptr);
        }
        ++stat_commands;
        stat_chunks += static_cast<int>(prepared_chunks.size());
        if (!chunks_valid)
        {
            cache_complete = false;
            ++cache_index;
            continue;
        }

        const std::uint64_t signature =
            prepared_entry_signature(prepared_chunks, command.texture, command.blend_mode);
        constexpr std::size_t kNoEntry = std::numeric_limits<std::size_t>::max();
        std::size_t donor_index = kNoEntry;

        // First prefer an exact positional entry, then an exact entry shifted by
        // insertion/removal. If neither exists, the positional entry remains a
        // useful partial donor: unchanged fixed chunks can still be retained.
        if (can_reuse_previous_backend && cache_index < previous_entries.size() &&
            !previous_claimed[cache_index])
        {
            const Entry& candidate = previous_entries[cache_index];
            if (!candidate.chunks.empty() && candidate.texture == command.texture &&
                candidate.blend_mode == command.blend_mode &&
                candidate.signature == signature)
            {
                donor_index = cache_index;
            }
        }
        if (donor_index == kNoEntry && can_reuse_previous_backend)
        {
            const auto [first, last] = previous_by_signature.equal_range(signature);
            for (auto it = first; it != last; ++it)
            {
                const Entry& candidate = previous_entries[it->second];
                if (!previous_claimed[it->second] && !candidate.chunks.empty() &&
                    candidate.texture == command.texture &&
                    candidate.blend_mode == command.blend_mode)
                {
                    donor_index = it->second;
                    break;
                }
            }
        }
        if (donor_index == kNoEntry && can_reuse_previous_backend &&
            cache_index < previous_entries.size() && !previous_claimed[cache_index])
        {
            const Entry& candidate = previous_entries[cache_index];
            if (!candidate.chunks.empty() && candidate.texture == command.texture &&
                candidate.blend_mode == command.blend_mode)
            {
                donor_index = cache_index;
            }
        }
        Entry* donor = nullptr;
        if (donor_index != kNoEntry)
        {
            previous_claimed[donor_index] = true;
            donor = &previous_entries[donor_index];
        }
        ++cache_index;

        Entry entry;
        entry.texture = command.texture;
        entry.blend_mode = command.blend_mode;
        entry.scissor = command.scissor;
        entry.scissor_x = sx;
        entry.scissor_y = sy;
        entry.scissor_width = sw;
        entry.scissor_height = sh;
        entry.signature = signature;
        entry.constants = command.constants;
        entry.constants_patchable = command.constants_patchable;
        entry.context_index = command.context_index;
        entry.chunks.reserve(prepared_chunks.size());

        std::unordered_multimap<std::uint64_t, std::size_t> donor_by_signature;
        if (donor != nullptr)
        {
            donor_by_signature.reserve(donor->chunks.size());
            for (std::size_t index = 0; index < donor->chunks.size(); ++index)
            {
                if (donor->chunks[index].geometry != 0)
                {
                    donor_by_signature.emplace(donor->chunks[index].signature, index);
                }
            }
        }

        bool command_compiled = false;
        bool command_failed = false;
        for (std::size_t chunk_index = 0; chunk_index < prepared_chunks.size(); ++chunk_index)
        {
            const PreparedGeometryChunk& prepared = prepared_chunks[chunk_index];
            PanoramaCompiledGeometryHandle geometry = 0;

            if (donor != nullptr && chunk_index < donor->chunks.size() &&
                donor->chunks[chunk_index].geometry != 0 &&
                donor->chunks[chunk_index].signature == prepared.signature)
            {
                geometry = donor->chunks[chunk_index].geometry;
                donor->chunks[chunk_index].geometry = 0;
            }
            else if (donor != nullptr)
            {
                const auto [first, last] = donor_by_signature.equal_range(prepared.signature);
                for (auto it = first; it != last; ++it)
                {
                    Chunk& candidate = donor->chunks[it->second];
                    if (candidate.geometry != 0)
                    {
                        geometry = candidate.geometry;
                        candidate.geometry = 0;
                        break;
                    }
                }
            }

            if (geometry != 0)
            {
                ++stat_reused_chunks;
            }
            else
            {
                const ScopedMicroTimer timer(compile_us_out);
                geometry = backend.compile_geometry(prepared.vertices, prepared.indices, ui_scale);
                if (geometry == 0)
                {
                    command_failed = true;
                    break;
                }
                command_compiled = true;
                ++stat_recompiled_chunks;
                const std::size_t uploaded_bytes =
                    prepared.vertices.size() * sizeof(PanoramaPaintVertex) +
                    prepared.indices.size() * sizeof(int);
                stat_uploaded_bytes += uploaded_bytes;
                stat_max_chunk_uploaded_bytes =
                    std::max(stat_max_chunk_uploaded_bytes, uploaded_bytes);
            }
            entry.chunks.push_back(Chunk{geometry, prepared.signature});
        }

        if (command_failed)
        {
            for (Chunk& chunk : entry.chunks)
            {
                backend.release_geometry(chunk.geometry);
                chunk.geometry = 0;
            }
            cache_complete = false;
            continue;
        }

        if (state.update_scissor(command.scissor, sx, sy, sw, sh))
        {
            backend.set_scissor(command.scissor, sx, sy, sw, sh);
        }
        if (state.update_blend(command.blend_mode))
        {
            backend.set_blend_mode(command.blend_mode);
        }
        for (const Chunk& chunk : entry.chunks)
        {
            backend.render_geometry(chunk.geometry, command.texture, command.constants);
        }
        entries_.push_back(std::move(entry));
        if (command_compiled)
        {
            ++stat_recompiled;
        }
        else
        {
            ++stat_reused;
        }
    }

    if (!previous_entries.empty() && previous_backend != nullptr && previous_backend == panorama_render_backend())
    {
        for (const Entry& cached : previous_entries)
        {
            for (const Chunk& chunk : cached.chunks)
            {
                if (chunk.geometry != 0)
                {
                    previous_backend->release_geometry(chunk.geometry);
                }
            }
        }
    }
    if (state.scissor_valid() && state.update_scissor(false, 0, 0, 0, 0))
    {
        backend.set_scissor(false, 0, 0, 0, 0);
    }
    if (state.blend_valid() && state.update_blend(PanoramaBlendMode::Normal))
    {
        backend.set_blend_mode(PanoramaBlendMode::Normal);
    }

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
        stats->chunks = stat_chunks;
        stats->reused_chunks = stat_reused_chunks;
        stats->recompiled_chunks = stat_recompiled_chunks;
        stats->hashed_bytes = stat_hashed_bytes;
        stats->uploaded_bytes = stat_uploaded_bytes;
        stats->max_chunk_uploaded_bytes = stat_max_chunk_uploaded_bytes;
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
            for (const Chunk& chunk : cached.chunks)
            {
                if (chunk.geometry != 0)
                {
                    backend_->release_geometry(chunk.geometry);
                }
            }
        }
    }
    entries_.clear();
    backend_ = nullptr;
    valid_ = false;
}
}
