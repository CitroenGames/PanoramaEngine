#include "ui/panorama/panorama_geometry_cache.hpp"
#include "ui/panorama/panorama_render_backend.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
void expect(bool condition, std::string_view message)
{
    if (!condition)
    {
        throw std::runtime_error(std::string(message));
    }
}

class FakeGeometryBackend : public panorama::PanoramaRenderBackend
{
public:
    panorama::PanoramaTextureId generate_texture(
        std::span<const unsigned char>, int, int) override
    {
        return next_texture_++;
    }

    void release_texture(panorama::PanoramaTextureId) override {}

    panorama::PanoramaCompiledGeometryHandle compile_geometry(
        std::span<const panorama::PanoramaPaintVertex> vertices,
        std::span<const int> indices,
        float) override
    {
        uploads.push_back(vertices.size() * sizeof(panorama::PanoramaPaintVertex) +
            indices.size() * sizeof(int));
        return next_geometry_++;
    }

    void render_geometry(
        panorama::PanoramaCompiledGeometryHandle geometry,
        panorama::PanoramaTextureId,
        const panorama::PanoramaDrawConstants&) override
    {
        rendered.push_back(geometry);
    }

    void release_geometry(
        panorama::PanoramaCompiledGeometryHandle geometry) override
    {
        released.push_back(geometry);
    }

    void set_scissor(bool, int, int, int, int) override { ++scissor_calls; }
    void set_blend_mode(panorama::PanoramaBlendMode) override { ++blend_calls; }

    std::vector<std::size_t> uploads;
    std::vector<panorama::PanoramaCompiledGeometryHandle> rendered;
    std::vector<panorama::PanoramaCompiledGeometryHandle> released;
    int scissor_calls = 0;
    int blend_calls = 0;

private:
    panorama::PanoramaTextureId next_texture_ = 1;
    panorama::PanoramaCompiledGeometryHandle next_geometry_ = 1;
};

panorama::PanoramaDrawCommand make_linear_command(std::size_t index_count)
{
    panorama::PanoramaDrawCommand command;
    command.vertices.resize(index_count);
    command.indices.resize(index_count);
    for (std::size_t index = 0; index < index_count; ++index)
    {
        command.vertices[index].x = static_cast<float>(index);
        command.vertices[index].y = static_cast<float>(index % 17U);
        command.indices[index] = static_cast<int>(index);
    }
    return command;
}

void test_one_edit_has_bounded_compile_amplification()
{
    FakeGeometryBackend backend;
    panorama::set_panorama_render_backend(&backend);
    panorama::PanoramaGeometryCache cache;

    panorama::PanoramaDrawList list;
    list.commands.push_back(make_linear_command(
        panorama::kPanoramaGeometryChunkIndexLimit * 2U));

    panorama::PanoramaGeometrySubmitStats first;
    cache.submit(list, backend, 1.0F, &first);
    expect(cache.valid(), "initial chunked geometry submit failed");
    expect(first.commands == 1 && first.chunks == 2,
        "large painter batch was not split below the command");
    expect(first.recompiled_chunks == 2 && first.reused_chunks == 0,
        "initial submit must compile every chunk");
    expect(first.max_chunk_uploaded_bytes < first.uploaded_bytes,
        "one chunk upload must be smaller than the oversized command");

    panorama::PanoramaGeometrySubmitStats steady;
    cache.submit(list, backend, 1.0F, &steady);
    expect(steady.reused == 1 && steady.reused_chunks == 2,
        "steady replay must retain every chunk");
    expect(steady.recompiled_chunks == 0 && steady.uploaded_bytes == 0,
        "steady replay unexpectedly uploaded geometry");

    list.commands[0].vertices[
        panorama::kPanoramaGeometryChunkIndexLimit + 7U].x += 0.5F;
    panorama::PanoramaGeometrySubmitStats edited;
    cache.submit(list, backend, 1.0F, &edited);
    expect(edited.recompiled == 1 && edited.recompiled_chunks == 1,
        "one-leaf edit must compile exactly one fixed chunk");
    expect(edited.reused_chunks == 1,
        "one-leaf edit discarded the unrelated stable chunk");
    expect(edited.uploaded_bytes == edited.max_chunk_uploaded_bytes,
        "one-leaf edit uploaded more than its changed chunk");
    expect(backend.released.size() == 1,
        "changed chunk retirement count is not bounded");

    cache.release();
    panorama::set_panorama_render_backend(nullptr);
}

void test_adjacent_draw_state_is_not_rebound()
{
    FakeGeometryBackend backend;
    panorama::set_panorama_render_backend(&backend);
    panorama::PanoramaGeometryCache cache;
    panorama::PanoramaDrawList list;
    list.commands.push_back(make_linear_command(3));
    list.commands.push_back(make_linear_command(3));
    for (panorama::PanoramaDrawCommand& command : list.commands)
    {
        command.scissor = true;
        command.scissor_x = 4.0F;
        command.scissor_y = 5.0F;
        command.scissor_width = 100.0F;
        command.scissor_height = 80.0F;
        command.blend_mode = panorama::PanoramaBlendMode::Additive;
    }

    cache.submit(list, backend, 1.0F);
    expect(cache.valid(), "state suppression fixture failed to submit");
    expect(backend.scissor_calls == 2,
        "identical adjacent scissor was rebound (expected set plus final clear)");
    expect(backend.blend_calls == 2,
        "identical adjacent blend was rebound (expected set plus final reset)");
    expect(backend.rendered.size() == 2, "state suppression dropped a draw");

    backend.scissor_calls = 0;
    backend.blend_calls = 0;
    backend.rendered.clear();
    expect(cache.replay(backend), "cached replay failed");
    expect(backend.scissor_calls == 2 && backend.blend_calls == 2,
        "replay did not suppress identical adjacent state");
    expect(backend.rendered.size() == 2, "replay state suppression dropped a draw");

    cache.release();
    panorama::set_panorama_render_backend(nullptr);
}

class FakeUploadBackend final : public FakeGeometryBackend
{
public:
    [[nodiscard]] panorama::PanoramaRenderBackendCapabilities capabilities() const noexcept override
    {
        return {
            panorama::kPanoramaRenderBackendContractVersion,
            static_cast<std::uint32_t>(
                panorama::PanoramaRenderBackendFeature::ExplicitSubmissionCompletion) |
                static_cast<std::uint32_t>(
                    panorama::PanoramaRenderBackendFeature::RegionalTextureUpdates) |
                static_cast<std::uint32_t>(
                    panorama::PanoramaRenderBackendFeature::BatchedTextureUploads),
        };
    }

    bool update_texture_regions(panorama::PanoramaTextureId texture,
        int texture_width, int texture_height,
        std::span<const panorama::PanoramaTextureRegionUpdate> regions) override
    {
        for (const panorama::PanoramaTextureRegionUpdate& region : regions)
        {
            const std::size_t pitch =
                region.row_pitch == 0
                    ? static_cast<std::size_t>(region.width) * 4U
                    : region.row_pitch;
            if (texture == 0 || region.x < 0 || region.y < 0 ||
                region.width <= 0 || region.height <= 0 ||
                region.x + region.width > texture_width ||
                region.y + region.height > texture_height ||
                pitch < static_cast<std::size_t>(region.width) * 4U ||
                region.rgba.size() <
                    pitch * static_cast<std::size_t>(region.height))
            {
                return false;
            }
        }
        for (const panorama::PanoramaTextureRegionUpdate& region : regions)
        {
            queued_bytes_.emplace_back(region.rgba.begin(), region.rgba.end());
        }
        queued_regions_ += regions.size();
        stable_texture_ = texture;
        return true;
    }

    std::size_t flush_texture_updates() override
    {
        const std::size_t flushed = queued_regions_;
        if (flushed != 0)
        {
            ++upload_submissions_;
        }
        queued_regions_ = 0;
        queued_bytes_.clear();
        return flushed;
    }

    [[nodiscard]] std::size_t upload_submissions() const noexcept
    {
        return upload_submissions_;
    }
    [[nodiscard]] panorama::PanoramaTextureId stable_texture() const noexcept
    {
        return stable_texture_;
    }

private:
    std::vector<std::vector<unsigned char>> queued_bytes_;
    std::size_t queued_regions_ = 0;
    std::size_t upload_submissions_ = 0;
    panorama::PanoramaTextureId stable_texture_ = 0;
};

void test_regional_updates_batch_and_keep_texture_identity()
{
    FakeUploadBackend backend;
    const panorama::PanoramaRenderBackendCapabilities capabilities =
        backend.capabilities();
    expect(capabilities.supports(
               panorama::PanoramaRenderBackendFeature::RegionalTextureUpdates),
        "regional upload capability missing");
    expect(capabilities.supports(
               panorama::PanoramaRenderBackendFeature::BatchedTextureUploads),
        "batched upload capability missing");

    std::vector<unsigned char> first(8U * 4U, 1U);
    std::vector<unsigned char> second(4U * 4U, 2U);
    const panorama::PanoramaTextureId texture = 17;
    const std::vector<panorama::PanoramaTextureRegionUpdate> updates{
        {2, 3, 2, 4, 0, first},
        {8, 1, 1, 4, 0, second},
    };
    expect(backend.update_texture_regions(texture, 16, 16, updates),
        "valid regional update batch was rejected");
    std::fill(first.begin(), first.end(), static_cast<unsigned char>(9));
    expect(backend.flush_texture_updates() == 2,
        "queued regions did not flush as one batch");
    expect(backend.upload_submissions() == 1,
        "N regional updates created more than one submission");
    expect(backend.stable_texture() == texture,
        "regional update changed the public texture identity");
}

void test_submission_order_and_release_horizon()
{
    panorama::PanoramaSubmissionTracker tracker;
    const panorama::PanoramaSubmissionId first = tracker.begin_submission();
    expect(static_cast<bool>(first), "first recording did not begin");
    const panorama::PanoramaSubmissionId retirement =
        tracker.retirement_horizon();
    expect(retirement == first,
        "record-time release was not attached to current submission");
    expect(!tracker.mark_completed(first),
        "completion was accepted before queue submission");
    expect(!tracker.can_reclaim(retirement),
        "record-time resource reclaimed before submission");
    expect(tracker.mark_submitted(first), "post-submit transition failed");
    expect(!tracker.can_reclaim(retirement),
        "submission alone reclaimed the resource");
    expect(tracker.mark_completed(first), "completion transition failed");
    expect(tracker.can_reclaim(retirement),
        "post-submit completion did not release the horizon");
}

void test_demand_frame_policy()
{
    panorama::PanoramaDemandFrameInput input;
    expect(panorama::panorama_choose_demand_frame_action(input) ==
            panorama::PanoramaDemandFrameAction::Skip,
        "static visible UI must not recur render/present");

    input.exposed = true;
    expect(panorama::panorama_choose_demand_frame_action(input) ==
            panorama::PanoramaDemandFrameAction::Render,
        "expose must request a frame");
    input = {};
    input.visual_changed = true;
    expect(panorama::panorama_choose_demand_frame_action(input) ==
            panorama::PanoramaDemandFrameAction::Render,
        "visual damage must request a frame");
    input = {};
    input.animation_active = true;
    expect(panorama::panorama_choose_demand_frame_action(input) ==
            panorama::PanoramaDemandFrameAction::Render,
        "active animation must remain continuous");
    input.occluded = true;
    expect(panorama::panorama_choose_demand_frame_action(input) ==
            panorama::PanoramaDemandFrameAction::ProbeOcclusion,
        "occluded UI must probe without recording a frame");
}
}

int main()
{
    try
    {
        test_one_edit_has_bounded_compile_amplification();
        test_adjacent_draw_state_is_not_rebound();
        test_regional_updates_batch_and_keep_texture_identity();
        test_submission_order_and_release_horizon();
        test_demand_frame_policy();
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "geometry/submission regression test failed: %s\n",
            error.what());
        return 1;
    }
    std::puts("geometry/submission regression tests passed");
    return 0;
}
