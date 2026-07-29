#include "examples/04_window_raster/raster_view.hpp"
#include "ui/panorama/panorama_paint.hpp"

#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{
void expect(bool condition, std::string_view message)
{
    if (!condition)
    {
        throw std::runtime_error(std::string(message));
    }
}

panorama::PanoramaDrawCommand rectangle(
    float x, float y, float width, float height,
    panorama::PanoramaColor color)
{
    panorama::PanoramaDrawCommand command;
    command.vertices = {
        {x, y, 0.0F, 0.0F, color},
        {x + width, y, 1.0F, 0.0F, color},
        {x + width, y + height, 1.0F, 1.0F, color},
        {x, y + height, 0.0F, 1.0F, color},
    };
    command.indices = {0, 1, 2, 0, 2, 3};
    return command;
}

void set_box(
    panorama::PanoramaNode& node, float x, float y, float width, float height)
{
    node.layout.x = x;
    node.layout.y = y;
    node.layout.width = width;
    node.layout.height = height;
    node.layout.content_x = x;
    node.layout.content_y = y;
    node.layout.content_width = width;
    node.layout.content_height = height;
}

void test_empty_nested_clip_prunes_subtree()
{
    panorama::PanoramaNode root;
    set_box(root, 0.0F, 0.0F, 100.0F, 100.0F);
    root.computed.background_color = {10, 20, 30, 255};
    root.computed.clip.type = panorama::PanoramaClipType::Rect;
    root.computed.clip.rect_right = 40.0F;
    root.computed.clip.rect_bottom = 40.0F;

    auto child = std::make_unique<panorama::PanoramaNode>();
    child->parent = &root;
    set_box(*child, 60.0F, 60.0F, 30.0F, 30.0F);
    child->computed.background_color = {200, 20, 30, 255};
    child->computed.clip.type = panorama::PanoramaClipType::Rect;
    auto grandchild = std::make_unique<panorama::PanoramaNode>();
    grandchild->parent = child.get();
    set_box(*grandchild, 60.0F, 60.0F, 10.0F, 10.0F);
    grandchild->computed.background_color = {20, 200, 30, 255};
    child->children.push_back(std::move(grandchild));
    root.children.push_back(std::move(child));

    panorama::PanoramaPaintScratch scratch;
    panorama::PanoramaDrawList list;
    panorama::build_panorama_draw_list(list, root, {}, &scratch);
    expect(scratch.stats.nodes_visited == 2,
        "empty nested intersection still traversed the grandchild");
    expect(scratch.stats.empty_clip_subtrees_pruned >= 1,
        "empty nested intersection was not recorded");
}

void test_large_gradient_and_repeat_are_clip_bounded()
{
    panorama::PanoramaNode gradient;
    set_box(gradient, 0.0F, 0.0F, 1024.0F, 1024.0F);
    gradient.computed.clip.type = panorama::PanoramaClipType::Rect;
    gradient.computed.clip.rect_right = 25.0F;
    gradient.computed.clip.rect_bottom = 25.0F;
    gradient.computed.background_gradient.type =
        panorama::PanoramaGradientType::Radial;
    gradient.computed.background_gradient.stops = {
        {0.0F, {255, 0, 0, 255}},
        {1.0F, {0, 0, 255, 255}},
    };
    panorama::PanoramaPaintScratch gradient_scratch;
    panorama::PanoramaDrawList gradient_list;
    panorama::build_panorama_draw_list(
        gradient_list, gradient, {}, &gradient_scratch);
    expect(gradient_scratch.stats.gradient_cells_clipped > 0,
        "large clipped gradient emitted its full 64x64 lattice");
    expect(gradient_scratch.stats.gradient_cells_emitted <= 33U * 33U,
        "gradient emitted cells outside the conservative clip halo");

    panorama::PanoramaNode repeated;
    set_box(repeated, 0.0F, 0.0F, 256.0F, 256.0F);
    repeated.computed.clip.type = panorama::PanoramaClipType::Rect;
    repeated.computed.clip.rect_right = 25.0F;
    repeated.computed.clip.rect_bottom = 25.0F;
    repeated.background_texture = 7;
    repeated.background_texture_natural_width = 1.0F;
    repeated.background_texture_natural_height = 1.0F;
    panorama::PanoramaPaintScratch repeat_scratch;
    panorama::PanoramaDrawList repeat_list;
    panorama::build_panorama_draw_list(
        repeat_list, repeated, {}, &repeat_scratch);
    expect(repeat_scratch.stats.background_tiles_considered == 256U * 256U,
        "repeat structural counter lost authored tile complexity");
    expect(repeat_scratch.stats.background_tiles_emitted <
            repeat_scratch.stats.background_tiles_considered / 4U,
        "repeat generation was not narrowed to visible tiles");
}

void test_radial_clip_reuses_scratch_and_removes_orphans()
{
    panorama::PanoramaNode node;
    set_box(node, 0.0F, 0.0F, 80.0F, 80.0F);
    node.computed.background_color = {255, 255, 255, 255};
    node.computed.clip.type = panorama::PanoramaClipType::Radial;
    node.computed.clip.radial_sweep = 180.0F;

    panorama::PanoramaPaintScratch scratch;
    panorama::PanoramaDrawList first;
    panorama::build_panorama_draw_list(first, node, {}, &scratch);
    expect(scratch.stats.radial_input_triangles == 2,
        "radial clip input triangle count changed");
    expect(scratch.stats.radial_output_triangles > 0,
        "radial clip produced no visible geometry");
    expect(scratch.stats.radial_orphan_vertices_removed == 4,
        "radial source vertices were left orphaned");

    panorama::PanoramaDrawList second;
    panorama::build_panorama_draw_list(second, node, {}, &scratch);
    expect(scratch.stats.radial_scratch_growths == 0,
        "steady radial rebuild grew transient scratch");
    expect(first.total_vertices() == second.total_vertices() &&
            first.total_indices() == second.total_indices(),
        "radial scratch reuse changed geometry shape");
}

void test_local_damage_matches_forced_full_oracle()
{
    panorama::PanoramaDrawList original;
    original.commands.push_back(
        rectangle(2.0F, 2.0F, 20.0F, 20.0F, {255, 0, 0, 255}));
    original.commands.push_back(
        rectangle(40.0F, 40.0F, 6.0F, 12.0F, {255, 255, 255, 255}));

    panorama_example::CpuTextureStore textures;
    panorama_example::Framebuffer partial;
    partial.resize(64, 64);
    partial.clear(24, 27, 32);
    panorama_example::rasterize_draw_list(partial, original, textures);

    panorama_example::RasterDamageTracker tracker;
    expect(tracker.update(original, 64, 64).full,
        "first damage frame must be forced full");
    expect(tracker.update(original, 64, 64).empty(),
        "unchanged draw list produced damage");

    panorama::PanoramaDrawList changed = original;
    changed.commands[1] =
        rectangle(42.0F, 40.0F, 6.0F, 12.0F, {255, 255, 255, 255});
    const panorama_example::RasterDamage damage =
        tracker.update(changed, 64, 64);
    expect(!damage.empty() && !damage.full && damage.area() < 64U * 64U / 4U,
        "localized caret-like edit fell back to full damage");
    partial.clear_rect(
        damage.left, damage.top, damage.right, damage.bottom, 24, 27, 32);
    panorama_example::rasterize_draw_list(
        partial, changed, textures, &damage);

    panorama_example::Framebuffer oracle;
    oracle.resize(64, 64);
    oracle.clear(24, 27, 32);
    panorama_example::rasterize_draw_list(oracle, changed, textures);
    expect(partial.rgba == oracle.rgba,
        "partial damage result differs from forced-full oracle");

    expect(tracker.update(changed, 80, 64).full,
        "resize did not force full damage");
    panorama::PanoramaDrawCommand blur;
    blur.blur_std_x = 2.0F;
    blur.blur_passes = 1;
    changed.commands.push_back(blur);
    expect(tracker.update(changed, 80, 64).full,
        "uncertain backdrop dependency did not force full damage");
    expect(tracker.stats().uncertain_fallbacks > 0,
        "uncertain fallback was not counted");
}
}

int main()
{
    try
    {
        test_empty_nested_clip_prunes_subtree();
        test_large_gradient_and_repeat_are_clip_bounded();
        test_radial_clip_reuses_scratch_and_removes_orphans();
        test_local_damage_matches_forced_full_oracle();
    }
    catch (const std::exception& error)
    {
        std::fprintf(
            stderr, "paint/damage regression test failed: %s\n",
            error.what());
        return 1;
    }
    std::puts("paint/damage regression tests passed");
    return 0;
}
