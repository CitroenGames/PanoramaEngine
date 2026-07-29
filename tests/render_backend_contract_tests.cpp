#include "ui/panorama/panorama_render_backend.hpp"

#include <cstddef>
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

class LegacyBackend final : public panorama::PanoramaRenderBackend
{
public:
    panorama::PanoramaTextureId generate_texture(
        std::span<const unsigned char>, int, int) override
    {
        return 1;
    }
    void release_texture(panorama::PanoramaTextureId) override {}
    panorama::PanoramaCompiledGeometryHandle compile_geometry(
        std::span<const panorama::PanoramaPaintVertex>,
        std::span<const int>,
        float) override
    {
        return 1;
    }
    void render_geometry(
        panorama::PanoramaCompiledGeometryHandle,
        panorama::PanoramaTextureId,
        const panorama::PanoramaDrawConstants&) override
    {
    }
    void release_geometry(panorama::PanoramaCompiledGeometryHandle) override {}
};

class FakeAsyncBackend final : public panorama::PanoramaRenderBackend
{
public:
    [[nodiscard]] panorama::PanoramaRenderBackendCapabilities capabilities() const noexcept override
    {
        return {
            panorama::kPanoramaRenderBackendContractVersion,
            static_cast<std::uint32_t>(panorama::PanoramaRenderBackendFeature::DrawConstants) |
                static_cast<std::uint32_t>(
                    panorama::PanoramaRenderBackendFeature::ExplicitSubmissionCompletion),
        };
    }

    [[nodiscard]] panorama::PanoramaSubmissionId begin_frame()
    {
        return submissions_.begin_submission();
    }

    bool submit_frame(panorama::PanoramaSubmissionId submission)
    {
        return submissions_.mark_submitted(submission);
    }

    bool complete_frame(panorama::PanoramaSubmissionId submission)
    {
        const bool accepted = submissions_.mark_completed(submission);
        reclaim();
        return accepted;
    }

    bool abandon_frame(panorama::PanoramaSubmissionId submission)
    {
        if (!submission || submission != submissions_.recording())
        {
            return false;
        }
        const panorama::PanoramaSubmissionId fallback =
            submissions_.abandon_submission(submission);
        for (Retirement& retirement : retired_)
        {
            if (retirement.after == submission)
            {
                retirement.after = fallback;
            }
        }
        reclaim();
        return true;
    }

    panorama::PanoramaTextureId generate_texture(
        std::span<const unsigned char>, int, int) override
    {
        return next_texture_++;
    }

    void release_texture(panorama::PanoramaTextureId texture) override
    {
        retire(static_cast<std::uintptr_t>(texture));
    }

    panorama::PanoramaCompiledGeometryHandle compile_geometry(
        std::span<const panorama::PanoramaPaintVertex>,
        std::span<const int>,
        float) override
    {
        return next_geometry_++;
    }

    void render_geometry(
        panorama::PanoramaCompiledGeometryHandle,
        panorama::PanoramaTextureId,
        const panorama::PanoramaDrawConstants&) override
    {
    }

    void release_geometry(panorama::PanoramaCompiledGeometryHandle geometry) override
    {
        retire(geometry);
    }

    [[nodiscard]] std::size_t retired_count() const noexcept { return retired_.size(); }
    [[nodiscard]] std::size_t reclaimed_count() const noexcept { return reclaimed_; }

private:
    struct Retirement
    {
        panorama::PanoramaSubmissionId after{};
        std::uintptr_t handle = 0;
    };

    void retire(std::uintptr_t handle)
    {
        retired_.push_back(Retirement{submissions_.retirement_horizon(), handle});
        reclaim();
    }

    void reclaim()
    {
        std::size_t write = 0;
        for (std::size_t read = 0; read < retired_.size(); ++read)
        {
            if (submissions_.can_reclaim(retired_[read].after))
            {
                ++reclaimed_;
            }
            else
            {
                if (write != read)
                {
                    retired_[write] = retired_[read];
                }
                ++write;
            }
        }
        retired_.resize(write);
    }

    panorama::PanoramaSubmissionTracker submissions_;
    std::vector<Retirement> retired_;
    panorama::PanoramaTextureId next_texture_ = 1;
    panorama::PanoramaCompiledGeometryHandle next_geometry_ = 1;
    std::size_t reclaimed_ = 0;
};

void test_capability_reporting_is_conservative_and_explicit()
{
    const LegacyBackend legacy;
    const panorama::PanoramaRenderBackendCapabilities legacy_caps = legacy.capabilities();
    expect(legacy_caps.contract_version == 1, "legacy contract version must remain conservative");
    expect(
        !legacy_caps.supports(panorama::PanoramaRenderBackendFeature::DrawConstants),
        "legacy backend must not claim draw-constant conformance");
    expect(
        !legacy_caps.supports(
            panorama::PanoramaRenderBackendFeature::ExplicitSubmissionCompletion),
        "legacy backend must not claim explicit completion");

    const FakeAsyncBackend asynchronous;
    const panorama::PanoramaRenderBackendCapabilities async_caps = asynchronous.capabilities();
    expect(
        async_caps.contract_version == panorama::kPanoramaRenderBackendContractVersion,
        "asynchronous backend must report the current contract version");
    expect(
        async_caps.supports(panorama::PanoramaRenderBackendFeature::DrawConstants),
        "conforming GPU backend must report draw constants");
    expect(
        async_caps.supports(
            panorama::PanoramaRenderBackendFeature::ExplicitSubmissionCompletion),
        "conforming GPU backend must report explicit completion");
}

void test_retirement_waits_for_the_post_submit_completion()
{
    FakeAsyncBackend backend;

    const panorama::PanoramaSubmissionId first = backend.begin_frame();
    expect(static_cast<bool>(first), "first frame must begin");
    expect(backend.submit_frame(first), "first frame must submit");
    expect(backend.complete_frame(first), "first frame must complete");

    const panorama::PanoramaSubmissionId second = backend.begin_frame();
    expect(second.value > first.value, "submission identities must increase");
    const panorama::PanoramaCompiledGeometryHandle geometry =
        backend.compile_geometry({}, {}, 1.0F);
    backend.release_geometry(geometry);

    expect(backend.retired_count() == 1, "current-frame geometry must remain retired");
    expect(backend.reclaimed_count() == 0, "current-frame geometry reclaimed before submission");

    // A pre-submit signal can at most repeat completion of the previous frame.
    // It must not satisfy a retirement associated with `second`.
    expect(backend.complete_frame(first), "repeated prior completion must be accepted");
    expect(backend.retired_count() == 1, "pre-submit completion reclaimed current-frame geometry");
    expect(backend.reclaimed_count() == 0, "pre-submit completion advanced reclamation");

    expect(backend.submit_frame(second), "second frame must submit");
    expect(backend.retired_count() == 1, "submission alone must not reclaim geometry");
    expect(backend.complete_frame(second), "second frame must complete");
    expect(backend.retired_count() == 0, "completed geometry retirement must drain");
    expect(backend.reclaimed_count() == 1, "completed geometry must be reclaimed exactly once");
}

void test_abandoned_recording_rebases_to_the_last_real_submission()
{
    FakeAsyncBackend backend;
    const panorama::PanoramaSubmissionId submitted = backend.begin_frame();
    expect(backend.submit_frame(submitted), "baseline frame must submit");
    expect(backend.complete_frame(submitted), "baseline frame must complete");

    const panorama::PanoramaSubmissionId abandoned = backend.begin_frame();
    const panorama::PanoramaTextureId texture = backend.generate_texture({}, 1, 1);
    backend.release_texture(texture);
    expect(backend.retired_count() == 1, "abandoned-frame texture must initially remain retired");

    expect(backend.abandon_frame(abandoned), "recording must abandon");
    expect(backend.retired_count() == 0, "abandoned retirement must rebase to completed work");
    expect(backend.reclaimed_count() == 1, "abandoned retirement must reclaim once");
}

void test_tracker_rejects_invalid_transitions()
{
    panorama::PanoramaSubmissionTracker tracker;
    const panorama::PanoramaSubmissionId first = tracker.begin_submission();
    expect(static_cast<bool>(first), "tracker must begin");
    expect(!tracker.begin_submission(), "tracker must reject overlapping recordings");
    expect(
        !tracker.mark_submitted(panorama::PanoramaSubmissionId{first.value + 1}),
        "tracker must reject a mismatched submission");
    expect(
        !tracker.mark_completed(first),
        "tracker must reject completion before submission");
    expect(tracker.mark_submitted(first), "tracker must accept the recording identity");
    expect(!tracker.can_reclaim(first), "submitted work must not be complete implicitly");
    expect(tracker.mark_completed(first), "tracker must accept post-submit completion");
    expect(tracker.can_reclaim(first), "completed work must become reclaimable");
}

void test_draw_constant_shader_abi()
{
    panorama::PanoramaDrawConstants constants;
    constants.a = 2.0F;
    constants.b = 3.0F;
    constants.c = 4.0F;
    constants.d = 5.0F;
    constants.e = 6.0F;
    constants.f = 7.0F;
    constants.opacity = 0.25F;

    const panorama::PanoramaGpuDrawConstants packed =
        panorama::panorama_pack_gpu_draw_constants(constants, 1.5F);
    expect(packed.a == 2.0F && packed.b == 3.0F, "linear transform AB packing changed");
    expect(packed.c == 4.0F && packed.d == 5.0F, "linear transform CD packing changed");
    expect(packed.e == 9.0F && packed.f == 10.5F, "translation must scale with UI scale");
    expect(packed.opacity == 0.25F, "opacity packing changed");
    expect(packed.padding == 0.0F, "shader ABI padding must be deterministic");

    constexpr float x = 11.0F;
    constexpr float y = 13.0F;
    const float transformed_x = packed.a * x + packed.c * y + packed.e;
    const float transformed_y = packed.b * x + packed.d * y + packed.f;
    expect(transformed_x == 83.0F, "shader ABI x transform changed");
    expect(transformed_y == 108.5F, "shader ABI y transform changed");
}
}

int main()
{
    try
    {
        test_capability_reporting_is_conservative_and_explicit();
        test_retirement_waits_for_the_post_submit_completion();
        test_abandoned_recording_rebases_to_the_last_real_submission();
        test_tracker_rejects_invalid_transitions();
        test_draw_constant_shader_abi();
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "render backend contract test failed: %s\n", error.what());
        return 1;
    }

    std::puts("render backend contract tests passed");
    return 0;
}
