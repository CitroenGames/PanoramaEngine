#pragma once

#include "ui/panorama/panorama_paint.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

// The GPU contract a host implements to display Panorama draw lists. The engine
// itself never talks to a graphics API: it produces PanoramaDrawList batches and
// the host feeds them through this interface (compile once, render per frame).
// Every hook except texture/geometry management has a safe no-op default, so a
// minimal backend only needs textures + geometry to get pixels on screen.
namespace panorama
{
using PanoramaCompiledGeometryHandle = std::uintptr_t;

// Contract version 2 makes GPU lifetime and draw-state support observable.
// Legacy/CPU backends may keep the default version-1 capability response, but
// adapters that record asynchronous GPU work must expose explicit
// submission/completion tracking before they advertise version 2.
inline constexpr std::uint32_t kPanoramaRenderBackendContractVersion = 2;

enum class PanoramaRenderBackendFeature : std::uint32_t
{
    DrawConstants = 1U << 0U,
    ExplicitSubmissionCompletion = 1U << 1U,
    RegionalTextureUpdates = 1U << 2U,
    BatchedTextureUploads = 1U << 3U,
    CombinedGeometryAllocation = 1U << 4U,
    RedundantStateSuppression = 1U << 5U,
};

struct PanoramaRenderBackendCapabilities
{
    std::uint32_t contract_version = 1;
    std::uint32_t features = 0;

    [[nodiscard]] constexpr bool supports(PanoramaRenderBackendFeature feature) const noexcept
    {
        return (features & static_cast<std::uint32_t>(feature)) != 0;
    }
};

// Canonical GPU packing for PanoramaDrawConstants. Keeping this layout in the
// API contract lets platform-neutral tests verify the exact shader ABI shared
// by D3D12 and Vulkan.
struct PanoramaGpuDrawConstants
{
    float a = 1.0F;
    float b = 0.0F;
    float c = 0.0F;
    float d = 1.0F;
    float e = 0.0F;
    float f = 0.0F;
    float opacity = 1.0F;
    float padding = 0.0F;
};

static_assert(sizeof(PanoramaGpuDrawConstants) == 8 * sizeof(float));

[[nodiscard]] constexpr PanoramaGpuDrawConstants panorama_pack_gpu_draw_constants(
    const PanoramaDrawConstants& constants, float ui_scale) noexcept
{
    return {
        constants.a,
        constants.b,
        constants.c,
        constants.d,
        constants.e * ui_scale,
        constants.f * ui_scale,
        constants.opacity,
        0.0F,
    };
}

// Opaque, monotonically increasing identity for one recorded GPU frame. Zero
// means "no submission": resources retired against zero were never made
// visible to asynchronous GPU work and may be reclaimed immediately.
struct PanoramaSubmissionId
{
    std::uint64_t value = 0;

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return value != 0; }
    friend constexpr bool operator==(PanoramaSubmissionId, PanoramaSubmissionId) noexcept = default;
};

// One tightly-addressed RGBA8 update to an existing texture. `row_pitch` is
// the source distance in bytes between rows (zero means width * 4). Backends
// that defer the upload MUST copy `rgba` before update_texture_regions()
// returns; callers retain no ownership after the call.
struct PanoramaTextureRegionUpdate
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    std::size_t row_pitch = 0;
    std::span<const unsigned char> rgba;
};

// Host-independent draw-state deduplication. The geometry cache uses this
// before virtual calls, and GPU adapters keep a second cache at their command
// recording boundary because host commands may invalidate API state.
class PanoramaDrawStateCache
{
public:
    [[nodiscard]] bool update_scissor(bool enabled, int x, int y, int width, int height) noexcept
    {
        if (scissor_valid_ && enabled == scissor_enabled_ &&
            (!enabled || (x == scissor_x_ && y == scissor_y_ &&
                             width == scissor_width_ && height == scissor_height_)))
        {
            return false;
        }
        scissor_valid_ = true;
        scissor_enabled_ = enabled;
        scissor_x_ = x;
        scissor_y_ = y;
        scissor_width_ = width;
        scissor_height_ = height;
        return true;
    }

    [[nodiscard]] bool update_blend(PanoramaBlendMode mode) noexcept
    {
        if (blend_valid_ && mode == blend_mode_)
        {
            return false;
        }
        blend_valid_ = true;
        blend_mode_ = mode;
        return true;
    }

    void invalidate() noexcept
    {
        scissor_valid_ = false;
        blend_valid_ = false;
    }

    [[nodiscard]] bool scissor_valid() const noexcept { return scissor_valid_; }
    [[nodiscard]] bool blend_valid() const noexcept { return blend_valid_; }

private:
    bool scissor_valid_ = false;
    bool blend_valid_ = false;
    bool scissor_enabled_ = false;
    int scissor_x_ = 0;
    int scissor_y_ = 0;
    int scissor_width_ = 0;
    int scissor_height_ = 0;
    PanoramaBlendMode blend_mode_ = PanoramaBlendMode::Normal;
};

enum class PanoramaDemandFrameAction : std::uint8_t
{
    Skip,
    Render,
    ProbeOcclusion,
};

struct PanoramaDemandFrameInput
{
    bool invalidated = false;
    bool visual_changed = false;
    bool animation_active = false;
    bool exposed = false;
    bool resized = false;
    bool continuous = false;
    bool occluded = false;
};

// Deterministic window-host policy: static visible content produces no
// recurring render/present, while an occluded swap chain is probed without
// recording throw-away UI work.
[[nodiscard]] constexpr PanoramaDemandFrameAction panorama_choose_demand_frame_action(
    const PanoramaDemandFrameInput& input) noexcept
{
    if (input.occluded)
    {
        return PanoramaDemandFrameAction::ProbeOcclusion;
    }
    if (input.invalidated || input.visual_changed || input.animation_active ||
        input.exposed || input.resized || input.continuous)
    {
        return PanoramaDemandFrameAction::Render;
    }
    return PanoramaDemandFrameAction::Skip;
}

// Platform-neutral state machine shared by asynchronous adapters. It prevents
// the unsafe ordering where a resource retired while the current frame is
// merely being recorded gets attached to a signal issued before that frame was
// submitted.
//
// Required host order:
//   1. begin_submission(), then record Panorama draws;
//   2. submit the command list/buffer to the GPU queue;
//   3. call mark_submitted() only after step 2 succeeded;
//   4. call mark_completed() only after the post-submit fence for that identity
//      has completed.
//
// A retirement takes retirement_horizon() at the time of release. If recording
// is abandoned, abandon_submission() returns the last real submission so the
// owner can rebase retirements from the abandoned identity. `can_reclaim()` is
// the only valid steady-state reclamation predicate.
class PanoramaSubmissionTracker
{
public:
    [[nodiscard]] PanoramaSubmissionId begin_submission() noexcept
    {
        if (recording_ || next_.value == ~std::uint64_t{0})
        {
            return {};
        }
        ++next_.value;
        recording_ = next_;
        return recording_;
    }

    [[nodiscard]] bool mark_submitted(PanoramaSubmissionId submission) noexcept
    {
        if (!submission || submission != recording_ || submission.value <= last_submitted_.value)
        {
            return false;
        }
        last_submitted_ = submission;
        recording_ = {};
        return true;
    }

    [[nodiscard]] bool mark_completed(PanoramaSubmissionId submission) noexcept
    {
        if (!submission || submission.value < completed_.value ||
            submission.value > last_submitted_.value)
        {
            return false;
        }
        completed_ = submission;
        return true;
    }

    [[nodiscard]] PanoramaSubmissionId abandon_submission(PanoramaSubmissionId submission) noexcept
    {
        if (!submission || submission != recording_)
        {
            return {};
        }
        recording_ = {};
        return last_submitted_;
    }

    [[nodiscard]] PanoramaSubmissionId retirement_horizon() const noexcept
    {
        return recording_ ? recording_ : last_submitted_;
    }

    [[nodiscard]] bool can_reclaim(PanoramaSubmissionId retirement) const noexcept
    {
        return !retirement || retirement.value <= completed_.value;
    }

    [[nodiscard]] PanoramaSubmissionId recording() const noexcept { return recording_; }
    [[nodiscard]] PanoramaSubmissionId last_submitted() const noexcept { return last_submitted_; }
    [[nodiscard]] PanoramaSubmissionId completed() const noexcept { return completed_; }

private:
    PanoramaSubmissionId next_{};
    PanoramaSubmissionId recording_{};
    PanoramaSubmissionId last_submitted_{};
    PanoramaSubmissionId completed_{};
};

class PanoramaRenderBackend
{
public:
    virtual ~PanoramaRenderBackend() = default;

    // Capability reporting is deliberately conservative for legacy backends:
    // callers must not assume non-identity draw constants or asynchronous
    // lifetime safety unless the corresponding bit is advertised.
    [[nodiscard]] virtual PanoramaRenderBackendCapabilities capabilities() const noexcept { return {}; }

    virtual PanoramaTextureId generate_texture(std::span<const unsigned char> rgba, int width, int height) = 0;
    virtual PanoramaTextureId load_texture(std::string_view, int& width, int& height)
    {
        width = 0;
        height = 0;
        return 0;
    }
    // Same contract, but the host has ALREADY resolved the resource's container bytes and
    // `source` is only its stable cache identity (not a filesystem path). This is the entry
    // point the Panorama host uses for animated <Movie>/background-image video: the resource
    // system, not the render backend, owns where content comes from, so a backend never has to
    // open a file. `bytes` may be empty when the host could not resolve the identity, which a
    // backend that needs data must treat as a failed load. The default forwards to the
    // identity-only overload above so existing backends behave exactly as before.
    virtual PanoramaTextureId load_texture(
        std::string_view source, std::span<const std::uint8_t> /*bytes*/, int& width, int& height)
    {
        return load_texture(source, width, height);
    }
    virtual void release_texture(PanoramaTextureId texture) = 0;

    // Re-uploads `rgba` into an EXISTING texture returned by generate_texture,
    // in place, instead of the caller doing release_texture+generate_texture.
    // For a host that repeatedly recomposites a texture of unchanging size (the
    // CS:GO radar disc), this avoids a GPU resource create/destroy every dirty
    // frame. Returns false when the backend cannot service the request in place
    // (unknown/stale id, size mismatch, no update support) — the caller must
    // then fall back to release_texture + generate_texture. Default no-op
    // backend always returns false, so hosts without in-place update support
    // (the software rasterizer, examples) keep today's create/destroy behavior
    // with no code change.
    virtual bool update_texture(PanoramaTextureId /*texture*/, std::span<const unsigned char> /*rgba*/,
        int /*width*/, int /*height*/)
    {
        return false;
    }

    // Queues one or more in-place RGBA8 regions for an existing texture.
    // Conforming asynchronous adapters batch queued regions into one GPU
    // submission/recording point and retain staging memory until that work
    // completes. The default only services one tightly packed full-texture
    // region through the legacy update_texture hook.
    virtual bool update_texture_regions(PanoramaTextureId texture, int texture_width, int texture_height,
        std::span<const PanoramaTextureRegionUpdate> regions)
    {
        if (regions.size() != 1)
        {
            return false;
        }
        const PanoramaTextureRegionUpdate& region = regions.front();
        if (texture == 0 || texture_width <= 0 || texture_height <= 0 ||
            region.width <= 0 || region.height <= 0)
        {
            return false;
        }
        const std::size_t tight_pitch =
            static_cast<std::size_t>(region.width) * 4U;
        const std::size_t pitch = region.row_pitch == 0 ? tight_pitch : region.row_pitch;
        if (region.x != 0 || region.y != 0 || region.width != texture_width ||
            region.height != texture_height || pitch != tight_pitch ||
            region.rgba.size() < tight_pitch * static_cast<std::size_t>(region.height))
        {
            return false;
        }
        return update_texture(texture, region.rgba.first(
            tight_pitch * static_cast<std::size_t>(region.height)), texture_width, texture_height);
    }

    // Records/submits all queued regional updates. GPU adapters also call this
    // automatically at their frame boundary; it is exposed for hosts that
    // upload assets without drawing a frame. Returns the number of regions
    // flushed, or zero when unsupported/nothing was queued.
    virtual std::size_t flush_texture_updates() { return 0; }

    // Sets (or clears) the scissor rectangle applied to subsequent render_geometry
    // calls, in framebuffer pixels. Used for Panorama `overflow` clipping. Default
    // no-op so backends without scissor support simply do not clip.
    virtual void set_scissor(bool /*enabled*/, int /*x*/, int /*y*/, int /*width*/, int /*height*/) {}

    // Selects the compositing mode for subsequent render_geometry calls (Panorama
    // -mix-blend-mode). Default no-op so backends without blend-mode support render
    // everything with their normal (alpha-over) blend.
    virtual void set_blend_mode(PanoramaBlendMode /*mode*/) {}

    // Backdrop blur: gaussian-blurs everything already rendered this frame inside
    // the given framebuffer-pixel rect (Panorama `blur: gaussian/fastgaussian` on
    // a panel — CS:GO's frosted submenu backgrounds). std deviations are in
    // framebuffer pixels. Default no-op for backends without blur support.
    virtual void blur_region(float /*x*/, float /*y*/, float /*width*/, float /*height*/,
        float /*std_x*/, float /*std_y*/, int /*passes*/) {}

    virtual PanoramaCompiledGeometryHandle compile_geometry(
        std::span<const PanoramaPaintVertex> vertices,
        std::span<const int> indices,
        float ui_scale) = 0;
    // `constants` is the issuing command's PanoramaDrawConstants (see
    // panorama_paint.hpp): a 2x3 affine transform (design px) + opacity the
    // backend must apply on top of `geometry`'s already-compiled vertices,
    // instead of the painter baking it in. Identity for an untransformed,
    // fully-opaque command (or one that went through the painter's
    // legacy-bake fallback, which still bakes), so an implementer that
    // ignores it renders those unchanged but WILL render animated
    // transform/opacity content wrong; a GPU backend folds it into per-draw
    // shader state (see RhiUiRenderInterface::render_geometry), a CPU
    // rasterizer applies it to vertex positions/colour before rasterizing
    // (see panorama_apply_draw_constants).
    virtual void render_geometry(
        PanoramaCompiledGeometryHandle geometry, PanoramaTextureId texture, const PanoramaDrawConstants& constants) = 0;
    // Releases a handle previously returned by compile_geometry. Must tolerate
    // being called for a handle whose draw was already recorded into the
    // CURRENT frame's command list (PanoramaGeometryCache::submit() does this
    // on a partial-failure rollback -- see its `release()` call -- and a normal
    // submit() releases geometry that fell out of this frame's list right after
    // recording whatever DID stay in it). An asynchronous implementation must
    // retire against the current recording identity and reclaim only after a
    // post-submit completion for that identity; a frame-count ring or a signal
    // issued before command submission does not satisfy this contract.
    virtual void release_geometry(PanoramaCompiledGeometryHandle geometry) = 0;
};

PanoramaRenderBackend* panorama_render_backend();
void set_panorama_render_backend(PanoramaRenderBackend* backend);
}
