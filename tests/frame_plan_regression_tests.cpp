#include "ui/panorama/panorama_resource_provider.hpp"
#include "ui/panorama/panorama_style.hpp"
#include "ui/panorama/panorama_view.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace
{
class MemoryProvider final : public panorama::PanoramaResourceProvider
{
public:
    void add(std::string path, std::string_view value)
    {
        files_[std::move(path)] = std::vector<unsigned char>(value.begin(), value.end());
    }

    [[nodiscard]] bool read(std::string_view path, panorama::PanoramaResource& out) const override
    {
        const auto found = files_.find(std::string(path));
        if (found == files_.end())
        {
            return false;
        }
        out.bytes = found->second;
        out.source = "frame-plan-test";
        return true;
    }

private:
    std::unordered_map<std::string, std::vector<unsigned char>> files_;
};

constexpr std::string_view kLayout = R"xml(<root>
  <styles><include src="file://{resources}/styles/frame_plan.css" /></styles>
  <Panel id="Root" class="root">
    <Panel id="Leaf" class="leaf" />
    <Panel id="Scroller" class="scroller">
      <Panel id="Tall" class="tall" />
    </Panel>
  </Panel>
</root>)xml";

constexpr std::string_view kStyle = R"css(
@keyframes pulse {
  from { opacity: 1.0; }
  to { opacity: 0.4; }
}
.root { width: 640px; height: 480px; flow-children: down; }
.leaf { width: 100px; height: 40px; background-color: #224466ff; }
.leaf.paint { background-color: #aa6633ff; }
.leaf.wide { width: 180px; }
.leaf.fade {
  opacity: 0.2;
  transition-property: opacity;
  transition-duration: 1.0s;
}
.leaf.pulse {
  animation-name: pulse;
  animation-duration: 1.0s;
  animation-iteration-count: infinite;
}
.scroller { width: 120px; height: 80px; overflow: squish scroll; }
.tall { width: 100px; height: 240px; }
)css";

[[noreturn]] void fail(std::string_view message)
{
    std::cerr << "frame plan regression: " << message << '\n';
    std::exit(1);
}

void expect(bool condition, std::string_view message)
{
    if (!condition)
    {
        fail(message);
    }
}

void configure(panorama::PanoramaView& view)
{
    auto provider = std::make_unique<MemoryProvider>();
    provider->add("panorama/layout/frame_plan.xml", kLayout);
    provider->add("panorama/styles/frame_plan.css", kStyle);
    view.resources().add_provider(std::move(provider), 0, "frame-plan");
    panorama::PanoramaViewLoadOptions options;
    options.enable_scripting = false;
    options.document.localize_text = false;
    expect(view.load("panorama/layout/frame_plan.xml", options), "fixture did not load");
}

void add_class(panorama::PanoramaNode& node, std::string value)
{
    node.classes.push_back(std::move(value));
    node.mark_style_dirty();
}

void expect_equivalent(const panorama::PanoramaView& incremental, const panorama::PanoramaView& reference)
{
    const panorama::PanoramaNode* a = incremental.root()->find_by_id("Leaf");
    const panorama::PanoramaNode* b = reference.root()->find_by_id("Leaf");
    expect(a != nullptr && b != nullptr, "leaf disappeared");
    expect(a->computed.width.type == b->computed.width.type && a->computed.width.value == b->computed.width.value,
        "computed width diverged");
    expect(a->computed.opacity == b->computed.opacity, "computed opacity diverged");
    expect(a->computed.background_color.r == b->computed.background_color.r &&
            a->computed.background_color.g == b->computed.background_color.g &&
            a->computed.background_color.b == b->computed.background_color.b &&
            a->computed.background_color.a == b->computed.background_color.a,
        "computed paint color diverged");
    expect(a->layout.x == b->layout.x && a->layout.y == b->layout.y &&
            a->layout.width == b->layout.width && a->layout.height == b->layout.height,
        "layout diverged");

    const auto& ac = incremental.draw_list().commands;
    const auto& bc = reference.draw_list().commands;
    expect(ac.size() == bc.size(), "draw command count diverged");
    for (std::size_t i = 0; i < ac.size(); ++i)
    {
        expect(ac[i].vertices.size() == bc[i].vertices.size() && ac[i].indices == bc[i].indices,
            "draw geometry shape diverged");
        expect(ac[i].constants == bc[i].constants, "draw constants diverged");
        for (std::size_t vertex = 0; vertex < ac[i].vertices.size(); ++vertex)
        {
            const auto& av = ac[i].vertices[vertex];
            const auto& bv = bc[i].vertices[vertex];
            expect(av.x == bv.x && av.y == bv.y && av.u == bv.u && av.v == bv.v &&
                    av.color.r == bv.color.r && av.color.g == bv.color.g &&
                    av.color.b == bv.color.b && av.color.a == bv.color.a,
                "draw vertices diverged");
        }
    }
}

panorama::PanoramaViewUpdateResult update_incremental(
    panorama::PanoramaView& view, float dt, panorama::PanoramaViewWorkStats& stats)
{
    panorama::PanoramaViewUpdateOptions options;
    options.allow_recomposite_only = true;
    options.work_stats = &stats;
    return view.update(dt, options);
}

panorama::PanoramaViewUpdateResult update_reference(
    panorama::PanoramaView& view, float dt, panorama::PanoramaViewWorkStats& stats)
{
    panorama::PanoramaViewUpdateOptions options;
    options.mode = panorama::PanoramaViewUpdateMode::ForcedFull;
    options.work_stats = &stats;
    return view.update(dt, options);
}
}

int main()
{
    using namespace panorama;
    PanoramaView incremental;
    PanoramaView reference;
    configure(incremental);
    configure(reference);

    PanoramaViewWorkStats incremental_stats;
    PanoramaViewWorkStats reference_stats;
    const PanoramaViewUpdateResult idle = update_incremental(incremental, 0.0F, incremental_stats);
    (void)update_reference(reference, 0.0F, reference_stats);
    expect(!idle.style_changed && !idle.layout_changed && !idle.visual_changed, "idle frame did work");
    expect(incremental_stats.transition_nodes_visited == 0 &&
            incremental_stats.keyframe_nodes_visited == 0 &&
            incremental_stats.scroll_nodes_visited == 0,
        "idle incremental frame walked animation trees");
    expect(reference_stats.transition_nodes_visited > 0 &&
            reference_stats.keyframe_nodes_visited > 0 &&
            reference_stats.scroll_nodes_visited > 0,
        "forced-full oracle did not visit animation trees");

    PanoramaNode* incremental_leaf = incremental.root()->find_by_id("Leaf");
    PanoramaNode* reference_leaf = reference.root()->find_by_id("Leaf");
    expect(incremental_leaf != nullptr && reference_leaf != nullptr, "fixture leaf missing");

    add_class(*incremental_leaf, "paint");
    add_class(*reference_leaf, "paint");
    (void)panorama_cascade_stats_take();
    const PanoramaViewUpdateResult paint = update_incremental(incremental, 0.0F, incremental_stats);
    const PanoramaCascadeStats incremental_cascade = panorama_cascade_stats_take();
    const PanoramaViewUpdateResult paint_reference = update_reference(reference, 0.0F, reference_stats);
    const PanoramaCascadeStats full_cascade = panorama_cascade_stats_take();
    expect(paint.style_changed && !paint.layout_changed && paint.draw_list_rebuilt,
        "paint-only style damage ran the wrong stages");
    expect(paint_reference.layout_changed, "forced-full style oracle did not retain conservative layout");
    expect(incremental_cascade.nodes < full_cascade.nodes, "incremental cascade did not reduce node work");
    expect_equivalent(incremental, reference);

    add_class(*incremental_leaf, "wide");
    add_class(*reference_leaf, "wide");
    const PanoramaViewUpdateResult wide = update_incremental(incremental, 0.0F, incremental_stats);
    (void)update_reference(reference, 0.0F, reference_stats);
    expect(wide.layout_changed && wide.draw_list_rebuilt, "layout style damage skipped layout");
    expect_equivalent(incremental, reference);

    add_class(*incremental_leaf, "fade");
    add_class(*reference_leaf, "fade");
    (void)update_incremental(incremental, 0.0F, incremental_stats);
    (void)update_reference(reference, 0.0F, reference_stats);
    const PanoramaViewUpdateResult fade = update_incremental(incremental, 0.25F, incremental_stats);
    const PanoramaViewUpdateResult fade_reference = update_reference(reference, 0.25F, reference_stats);
    expect(fade.recomposite_only && !fade.draw_list_rebuilt &&
            fade.work.visual == PanoramaViewVisualWork::Recomposite &&
            incremental_stats.draw_list_builds == 0 && incremental_stats.recomposite_patches == 1,
        "opacity transition did not take the retained recomposite path");
    expect(fade_reference.draw_list_rebuilt, "forced-full transition oracle did not repaint");
    expect_equivalent(incremental, reference);

    // A newly styled keyframe candidate must open the gate; a later frame must
    // visit the keyframe tree even without new style damage.
    add_class(*incremental_leaf, "pulse");
    add_class(*reference_leaf, "pulse");
    (void)update_incremental(incremental, 0.0F, incremental_stats);
    (void)update_reference(reference, 0.0F, reference_stats);
    (void)update_incremental(incremental, 0.1F, incremental_stats);
    (void)update_reference(reference, 0.1F, reference_stats);
    expect(incremental_stats.keyframe_passes == 1 && incremental_stats.keyframe_nodes_visited > 0,
        "active keyframe gate skipped its tree");
    expect_equivalent(incremental, reference);

    PanoramaNode* incremental_scroller = incremental.root()->find_by_id("Scroller");
    PanoramaNode* reference_scroller = reference.root()->find_by_id("Scroller");
    expect(incremental_scroller != nullptr && reference_scroller != nullptr, "fixture scroller missing");
    expect(panorama_smooth_scroll_to(*incremental_scroller, 0.0F, 60.0F) &&
            panorama_smooth_scroll_to(*reference_scroller, 0.0F, 60.0F),
        "smooth scroll did not start");
    incremental.invalidate_layout();
    reference.invalidate_layout();
    (void)update_incremental(incremental, 0.016F, incremental_stats);
    (void)update_reference(reference, 0.016F, reference_stats);
    expect(incremental_stats.scroll_animation_passes == 1 && incremental_stats.scroll_nodes_visited > 0,
        "active scroll gate skipped its tree");
    expect(incremental_scroller->scroll_offset_y == reference_scroller->scroll_offset_y,
        "scroll advance diverged from forced-full oracle");

    std::cout << "frame plan regression checks passed\n";
    return 0;
}
