#include "ui/panorama/panorama_view.hpp"

#include "ui/panorama/panorama_log.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace panorama
{
namespace
{
bool same_length(const PanoramaLength& a, const PanoramaLength& b)
{
    return a.type == b.type && a.value == b.value;
}

bool same_edges(const PanoramaEdges& a, const PanoramaEdges& b)
{
    return a.top == b.top && a.right == b.right && a.bottom == b.bottom && a.left == b.left;
}

struct LayoutStyleSignature
{
    PanoramaLength width;
    PanoramaLength height;
    PanoramaLength min_width;
    PanoramaLength max_width;
    PanoramaLength min_height;
    PanoramaLength max_height;
    PanoramaEdges margin;
    PanoramaEdges margin_pct;
    std::uint8_t margin_pct_mask = 0;
    PanoramaEdges padding;
    PanoramaFlow flow = PanoramaFlow::None;
    PanoramaHAlign halign = PanoramaHAlign::Left;
    PanoramaVAlign valign = PanoramaVAlign::Top;
    bool has_position = false;
    float pos_x = 0.0F;
    float pos_y = 0.0F;
    bool pos_x_percent = false;
    bool pos_y_percent = false;
    bool border_per_side = false;
    float border_width = 0.0F;
    float border_width_top = 0.0F;
    float border_width_right = 0.0F;
    float border_width_bottom = 0.0F;
    float border_width_left = 0.0F;
    float font_size = 0.0F;
    int font_weight = 0;
    bool font_italic = false;
    PanoramaTextTransform text_transform = PanoramaTextTransform::None;
    PanoramaHAlign text_align = PanoramaHAlign::Left;
    bool white_space_nowrap = false;
    float letter_spacing = 0.0F;
    float line_height = 0.0F;
    PanoramaTextOverflow text_overflow = PanoramaTextOverflow::Clip;
    bool visible = true;
    bool overflow_squish_x = true;
    bool overflow_squish_y = true;
    bool overflow_scroll_x = false;
    bool overflow_scroll_y = false;
};

LayoutStyleSignature layout_signature(const PanoramaNode& node, bool cascade_target)
{
    const PanoramaComputedStyle& style = node.computed;
    LayoutStyleSignature out;
    out.width = cascade_target || !node.anim.initialized ? style.width : node.anim.width_tgt;
    out.height = cascade_target || !node.anim.initialized ? style.height : node.anim.height_tgt;
    out.min_width = style.min_width;
    out.max_width = style.max_width;
    out.min_height = style.min_height;
    out.max_height = style.max_height;
    out.margin = style.margin;
    out.margin_pct = style.margin_pct;
    out.margin_pct_mask = style.margin_pct_mask;
    out.padding = style.padding;
    out.flow = style.flow;
    out.halign = style.halign;
    out.valign = style.valign;
    out.has_position = cascade_target || !node.anim.initialized ? style.has_position : node.anim.pos_has;
    out.pos_x = cascade_target || !node.anim.initialized ? style.pos_x : node.anim.pos_x_tgt;
    out.pos_y = cascade_target || !node.anim.initialized ? style.pos_y : node.anim.pos_y_tgt;
    out.pos_x_percent =
        cascade_target || !node.anim.initialized ? style.pos_x_percent : node.anim.pos_x_percent;
    out.pos_y_percent =
        cascade_target || !node.anim.initialized ? style.pos_y_percent : node.anim.pos_y_percent;
    out.border_per_side = style.border_per_side;
    out.border_width = style.border_width;
    out.border_width_top = style.border_width_top;
    out.border_width_right = style.border_width_right;
    out.border_width_bottom = style.border_width_bottom;
    out.border_width_left = style.border_width_left;
    out.font_size = style.font_size;
    out.font_weight = style.font_weight;
    out.font_italic = style.font_italic;
    out.text_transform = style.text_transform;
    out.text_align = style.text_align;
    out.white_space_nowrap = style.white_space_nowrap;
    out.letter_spacing = style.letter_spacing;
    out.line_height = style.line_height;
    out.text_overflow = style.text_overflow;
    out.visible = style.visible;
    out.overflow_squish_x = style.overflow_squish_x;
    out.overflow_squish_y = style.overflow_squish_y;
    out.overflow_scroll_x = style.overflow_scroll_x;
    out.overflow_scroll_y = style.overflow_scroll_y;
    return out;
}

bool same_layout_signature(const LayoutStyleSignature& a, const LayoutStyleSignature& b)
{
    return same_length(a.width, b.width) && same_length(a.height, b.height) &&
        same_length(a.min_width, b.min_width) && same_length(a.max_width, b.max_width) &&
        same_length(a.min_height, b.min_height) && same_length(a.max_height, b.max_height) &&
        same_edges(a.margin, b.margin) && same_edges(a.margin_pct, b.margin_pct) &&
        a.margin_pct_mask == b.margin_pct_mask && same_edges(a.padding, b.padding) && a.flow == b.flow &&
        a.halign == b.halign && a.valign == b.valign && a.has_position == b.has_position &&
        a.pos_x == b.pos_x && a.pos_y == b.pos_y && a.pos_x_percent == b.pos_x_percent &&
        a.pos_y_percent == b.pos_y_percent && a.border_per_side == b.border_per_side &&
        a.border_width == b.border_width && a.border_width_top == b.border_width_top &&
        a.border_width_right == b.border_width_right && a.border_width_bottom == b.border_width_bottom &&
        a.border_width_left == b.border_width_left && a.font_size == b.font_size &&
        a.font_weight == b.font_weight && a.font_italic == b.font_italic &&
        a.text_transform == b.text_transform && a.text_align == b.text_align &&
        a.white_space_nowrap == b.white_space_nowrap && a.letter_spacing == b.letter_spacing &&
        a.line_height == b.line_height && a.text_overflow == b.text_overflow && a.visible == b.visible &&
        a.overflow_squish_x == b.overflow_squish_x && a.overflow_squish_y == b.overflow_squish_y &&
        a.overflow_scroll_x == b.overflow_scroll_x && a.overflow_scroll_y == b.overflow_scroll_y;
}

struct LayoutStyleBefore
{
    PanoramaNode* node = nullptr;
    LayoutStyleSignature signature;
};

void append_subtree_snapshots(PanoramaNode& node, std::vector<LayoutStyleBefore>& snapshots)
{
    snapshots.push_back(LayoutStyleBefore{&node, layout_signature(node, false)});
    for (const auto& child : node.children)
    {
        append_subtree_snapshots(*child, snapshots);
    }
}

// Mirrors compute_invalidated's routing and conservatively includes following
// siblings whenever a directly dirty child could be widened by a sibling rule.
// The roots are disjoint, so both snapshots and selective capture visit a node
// at most once.
void collect_invalidated_roots(PanoramaNode& node, std::vector<PanoramaNode*>& roots)
{
    if (node.style_dirty)
    {
        roots.push_back(&node);
        return;
    }
    if (!node.descendant_style_dirty)
    {
        return;
    }

    bool dirty_child_seen = false;
    for (const auto& child : node.children)
    {
        if (dirty_child_seen)
        {
            roots.push_back(child.get());
            continue;
        }
        if (child->style_dirty)
        {
            roots.push_back(child.get());
            dirty_child_seen = true;
        }
        else if (child->descendant_style_dirty)
        {
            collect_invalidated_roots(*child, roots);
        }
    }
}

void merge_capture(PanoramaAnimCaptureResult& destination, const PanoramaAnimCaptureResult& source)
{
    destination.any_transition_animating =
        destination.any_transition_animating || source.any_transition_animating;
    destination.any_keyframe_candidate =
        destination.any_keyframe_candidate || source.any_keyframe_candidate;
}

// Control presentation materializes engine-owned children (scrollbar chrome,
// slider parts, selection labels) after the cascade has revealed the control's
// computed role. A style frame is not complete until those new nodes receive
// their own computed styles. The ensure_* routines are idempotent, so this
// converges after the finite set of missing internals has been created.
bool apply_presentation_and_stabilize(PanoramaStyleSheet& sheet, PanoramaNode& root)
{
    bool required_followup_cascade = false;
    for (;;)
    {
        panorama_apply_visibility_overrides(root);
        panorama_apply_control_presentation(root);
        if (!root.style_dirty && !root.descendant_style_dirty)
        {
            return required_followup_cascade;
        }
        sheet.compute_invalidated(root);
        required_followup_cascade = true;
    }
}
}

PanoramaView::PanoramaView()
{
    configure_runtime_bridges();
}

PanoramaView::~PanoramaView()
{
    unload();
}

bool PanoramaView::load(std::string_view document_path, PanoramaViewLoadOptions options)
{
    unload();
    const std::filesystem::path runtime_resource_root = options.document.resource_root;

    if (!session_.load(document_path, std::move(options.document)))
    {
        return false;
    }

    loaded_ = true;
    style_dirty_ = true;
    layout_dirty_ = true;
    visual_dirty_ = true;
    force_full_style_dirty_ = true;

    // Give init-time scripts valid panel geometry. Some real Panorama scripts
    // inspect actual layout dimensions while installing their initial state.
    (void)recompute_styles(true);
    relayout();

    if (options.enable_scripting)
    {
        std::vector<PanoramaRuntimeScriptInclude> scripts;
        scripts.reserve(session_.script_refs().size());
        for (const PanoramaScriptInclude& script : session_.script_refs())
        {
            scripts.push_back(PanoramaRuntimeScriptInclude{script.path, script.context});
        }

        if (!runtime_.initialize_with_script_contexts(
                *root(), session_.resources(), scripts, runtime_resource_root))
        {
            pano_log_warning("Panorama view: failed to initialize scripting for '{}'", document_path);
            unload();
            return false;
        }
        style_dirty_ = runtime_.consume_dirty();
        force_full_style_dirty_ = style_dirty_;
    }

    if (style_dirty_)
    {
        (void)recompute_styles(true);
        relayout();
    }
    rebuild_draw_list();

    style_dirty_ = false;
    layout_dirty_ = false;
    visual_dirty_ = false;
    force_full_style_dirty_ = false;
    return true;
}

void PanoramaView::unload()
{
    runtime_.shutdown();
    input_.reset();
    session_.clear_document();
    draw_list_.commands.clear();
    draw_list_.contexts.clear();
    paint_scratch_.reusable_commands.clear();
    recomposite_dirty_nodes_.clear();
    recomposite_contexts_.clear();
    loaded_ = false;
    style_dirty_ = false;
    layout_dirty_ = false;
    visual_dirty_ = false;
    force_full_style_dirty_ = false;
    transitions_active_ = false;
    keyframes_active_ = false;
    scroll_animations_active_ = false;
    recomposite_generation_ = 0;
}

bool PanoramaView::loaded() const noexcept
{
    return loaded_;
}

PanoramaNode* PanoramaView::root() noexcept
{
    return loaded_ ? session_.document().root.get() : nullptr;
}

const PanoramaNode* PanoramaView::root() const noexcept
{
    return loaded_ ? session_.document().root.get() : nullptr;
}

PanoramaDocumentSession& PanoramaView::session() noexcept
{
    return session_;
}

const PanoramaDocumentSession& PanoramaView::session() const noexcept
{
    return session_;
}

PanoramaResourceManager& PanoramaView::resources() noexcept
{
    return session_.resources();
}

const PanoramaResourceManager& PanoramaView::resources() const noexcept
{
    return session_.resources();
}

PanoramaRuntime& PanoramaView::runtime() noexcept
{
    return runtime_;
}

const PanoramaRuntime& PanoramaView::runtime() const noexcept
{
    return runtime_;
}

PanoramaInputController& PanoramaView::input() noexcept
{
    return input_;
}

const PanoramaInputController& PanoramaView::input() const noexcept
{
    return input_;
}

void PanoramaView::set_viewport(float width, float height)
{
    width = std::max(width, 1.0F);
    height = std::max(height, 1.0F);
    if (width == viewport_width_ && height == viewport_height_)
    {
        return;
    }
    viewport_width_ = width;
    viewport_height_ = height;
    invalidate_layout();
}

float PanoramaView::viewport_width() const noexcept
{
    return viewport_width_;
}

float PanoramaView::viewport_height() const noexcept
{
    return viewport_height_;
}

void PanoramaView::set_font_atlas(PanoramaFontAtlas* atlas)
{
    font_atlas_ = atlas;
    if (font_atlas_ != nullptr)
    {
        text_measure_ = font_atlas_->text_measure();
    }
    else
    {
        text_measure_ = default_text_measure();
        glyph_source_ = {};
    }
    invalidate_layout();
}

void PanoramaView::set_text_measure(PanoramaTextMeasure measure)
{
    font_atlas_ = nullptr;
    text_measure_ = measure.measure ? std::move(measure) : default_text_measure();
    invalidate_layout();
}

void PanoramaView::set_glyph_source(PanoramaGlyphSource glyphs)
{
    font_atlas_ = nullptr;
    glyph_source_ = std::move(glyphs);
    invalidate_visual();
}

bool PanoramaView::update_pointer(float x, float y, bool down)
{
    PanoramaNode* document_root = root();
    if (document_root == nullptr)
    {
        return false;
    }
    const bool changed = input_.update_pointer(
        *document_root, x, y, down, runtime_.active() ? &runtime_ : nullptr);
    if (changed)
    {
        request_local_style_update();
    }
    return changed;
}

bool PanoramaView::update_wheel(float x, float y, float wheel_ticks_y)
{
    PanoramaNode* document_root = root();
    if (document_root == nullptr || wheel_ticks_y == 0.0F)
    {
        return false;
    }
    const bool changed = input_.update_wheel(
        *document_root, x, y, wheel_ticks_y, runtime_.active() ? &runtime_ : nullptr);
    if (changed)
    {
        invalidate_layout();
        scroll_animations_active_ = true;
    }
    return changed;
}

bool PanoramaView::handle_key_down(const PanoramaKeyEvent& event)
{
    PanoramaNode* document_root = root();
    if (document_root == nullptr)
    {
        return false;
    }
    const bool changed = input_.handle_key_down(
        *document_root, event, runtime_.active() ? &runtime_ : nullptr);
    if (changed)
    {
        request_local_style_update(true);
    }
    return changed;
}

bool PanoramaView::handle_text_input(std::string_view utf8)
{
    PanoramaNode* document_root = root();
    if (document_root == nullptr || utf8.empty())
    {
        return false;
    }
    const bool changed = input_.handle_text_input(
        *document_root, utf8, runtime_.active() ? &runtime_ : nullptr);
    if (changed)
    {
        request_local_style_update(true);
    }
    return changed;
}

bool PanoramaView::handle_paste(std::string_view utf8)
{
    PanoramaNode* document_root = root();
    if (document_root == nullptr || utf8.empty())
    {
        return false;
    }
    const bool changed = input_.handle_paste(
        *document_root, utf8, runtime_.active() ? &runtime_ : nullptr);
    if (changed)
    {
        request_local_style_update(true);
    }
    return changed;
}

void PanoramaView::set_focus(PanoramaNode* node)
{
    PanoramaNode* document_root = root();
    if (document_root == nullptr)
    {
        return;
    }
    input_.set_focus(*document_root, node, runtime_.active() ? &runtime_ : nullptr);
    request_local_style_update();
}

PanoramaViewUpdateResult PanoramaView::update(float dt_seconds)
{
    return update(dt_seconds, PanoramaViewUpdateOptions{});
}

PanoramaViewUpdateResult PanoramaView::update(float dt_seconds, PanoramaViewUpdateOptions options)
{
    PanoramaViewUpdateResult result;
    PanoramaViewWorkStats* work_stats = options.work_stats;
    if (work_stats != nullptr)
    {
        *work_stats = PanoramaViewWorkStats{};
    }
    PanoramaNode* document_root = root();
    if (document_root == nullptr)
    {
        return result;
    }

    recomposite_dirty_nodes_.clear();
    ++recomposite_generation_;
    if (recomposite_generation_ == 0)
    {
        ++recomposite_generation_;
    }
    PanoramaRecompositeDirtyTracker dirty_tracker{
        &recomposite_dirty_nodes_,
        recomposite_generation_,
    };
    PanoramaAnimationWorkStats animation_work_stats;
    PanoramaAnimationWorkStats* animation_stats = work_stats != nullptr ? &animation_work_stats : nullptr;

    dt_seconds = std::max(dt_seconds, 0.0F);
    if (runtime_.active())
    {
        result.work.runtime_pumped = true;
        runtime_.update(static_cast<double>(dt_seconds));
        if (runtime_.consume_dirty())
        {
            // Runtime mutation helpers intentionally expose one coarse dirty
            // bit, so they take the compatibility path until they can route
            // exact node damage.
            style_dirty_ = true;
            layout_dirty_ = true;
            visual_dirty_ = true;
            force_full_style_dirty_ = true;
            scroll_animations_active_ = true;
        }
    }

    // Native callers can mutate nodes through root()/session() and simply call
    // mark_style_dirty(). Honor those marks without requiring a second view API.
    style_dirty_ = document_root->style_dirty || document_root->descendant_style_dirty || style_dirty_;
    if (style_dirty_)
    {
        const bool forced_full =
            options.mode == PanoramaViewUpdateMode::ForcedFull || force_full_style_dirty_;
        const bool layout_damage = recompute_styles(forced_full);
        result.work.style = forced_full ? PanoramaViewStyleWork::Full : PanoramaViewStyleWork::Incremental;
        if (work_stats != nullptr)
        {
            if (forced_full)
            {
                ++work_stats->full_style_passes;
            }
            else
            {
                ++work_stats->incremental_style_passes;
            }
        }
        layout_dirty_ = layout_damage || layout_dirty_;
        result.style_changed = true;
    }

    const bool forced_animation_walk = options.mode == PanoramaViewUpdateMode::ForcedFull;
    PanoramaAnimationAdvanceResult transitions;
    if (forced_animation_walk || transitions_active_)
    {
        result.work.transitions_advanced = true;
        if (work_stats != nullptr)
        {
            ++work_stats->transition_passes;
        }
        transitions =
            panorama_advance_anim(*document_root, dt_seconds, &dirty_tracker, animation_stats);
        transitions_active_ = transitions.active;
        for (const PanoramaTransitionEnd& ended : transitions.transition_ends)
        {
            if (runtime_.active() && ended.node != nullptr && ended.property != nullptr)
            {
                runtime_.dispatch_property_transition_end(*ended.node, ended.property);
            }
        }
    }

    PanoramaAnimationAdvanceResult keyframes;
    if (forced_animation_walk || keyframes_active_)
    {
        result.work.keyframes_advanced = true;
        if (work_stats != nullptr)
        {
            ++work_stats->keyframe_passes;
        }
        keyframes = panorama_advance_keyframes(
            *document_root, session_.style_sheet(), dt_seconds, &dirty_tracker, animation_stats);
        keyframes_active_ = keyframes.active;
    }

    PanoramaAnimationAdvanceResult scrolls;
    if (forced_animation_walk || scroll_animations_active_)
    {
        result.work.scroll_animations_advanced = true;
        if (work_stats != nullptr)
        {
            ++work_stats->scroll_animation_passes;
        }
        scrolls = panorama_advance_scroll_animations(*document_root, dt_seconds, animation_stats);
        scroll_animations_active_ = scrolls.active;
    }

    if (work_stats != nullptr)
    {
        work_stats->transition_nodes_visited = animation_work_stats.transition_nodes_visited;
        work_stats->keyframe_nodes_visited = animation_work_stats.keyframe_nodes_visited;
        work_stats->scroll_nodes_visited = animation_work_stats.scroll_nodes_visited;
    }

    result.animation_active = transitions_active_ || keyframes_active_ || scroll_animations_active_;
    layout_dirty_ = transitions.layout_changed || keyframes.layout_changed || scrolls.layout_changed || layout_dirty_;
    visual_dirty_ = transitions.visual_changed || keyframes.visual_changed || visual_dirty_;
    const bool recomposite_changed = transitions.recomposite_changed || keyframes.recomposite_changed;

    if (layout_dirty_)
    {
        relayout();
        result.work.layout = true;
        if (work_stats != nullptr)
        {
            ++work_stats->layout_passes;
        }
        result.layout_changed = true;
    }

    if (!visual_dirty_ && recomposite_changed && options.allow_recomposite_only)
    {
        if (patch_recomposite_draw_list())
        {
            result.work.visual = PanoramaViewVisualWork::Recomposite;
            result.visual_changed = true;
            result.recomposite_only = true;
            result.recomposite_dirty_node_count = recomposite_dirty_nodes_.size();
            result.recomposite_generation = recomposite_generation_;
            if (work_stats != nullptr)
            {
                ++work_stats->recomposite_patches;
            }
        }
        else
        {
            visual_dirty_ = true;
            if (work_stats != nullptr)
            {
                ++work_stats->recomposite_fallbacks;
            }
        }
    }
    else if (recomposite_changed)
    {
        // Compatibility path for existing hosts that only observe
        // draw_list_rebuilt.
        visual_dirty_ = true;
    }

    if (visual_dirty_)
    {
        rebuild_draw_list();
        result.work.visual = PanoramaViewVisualWork::Rebuild;
        if (work_stats != nullptr)
        {
            ++work_stats->draw_list_builds;
        }
        result.draw_list_rebuilt = true;
        result.visual_changed = true;
    }

    style_dirty_ = false;
    layout_dirty_ = false;
    visual_dirty_ = false;
    force_full_style_dirty_ = false;
    return result;
}

void PanoramaView::request_local_style_update(bool content_layout_changed)
{
    style_dirty_ = true;
    layout_dirty_ = content_layout_changed || layout_dirty_;
    visual_dirty_ = true;
}

void PanoramaView::invalidate_style()
{
    style_dirty_ = true;
    layout_dirty_ = true;
    visual_dirty_ = true;
    force_full_style_dirty_ = true;
    if (PanoramaNode* document_root = root())
    {
        document_root->mark_style_dirty();
    }
}

void PanoramaView::invalidate_layout()
{
    layout_dirty_ = true;
    visual_dirty_ = true;
    // Native code commonly pairs this with panorama_smooth_scroll_to().
    // One gated scan discovers whether a spring remains active.
    scroll_animations_active_ = true;
}

void PanoramaView::invalidate_visual()
{
    visual_dirty_ = true;
}

const PanoramaDrawList& PanoramaView::draw_list() const noexcept
{
    return draw_list_;
}

const std::vector<PanoramaNode*>& PanoramaView::recomposite_dirty_nodes() const noexcept
{
    return recomposite_dirty_nodes_;
}

void PanoramaView::configure_runtime_bridges()
{
    runtime_.set_layout_loaders(
        [this](PanoramaNode& target, const std::string& source) {
            run_added_scripts(session_.load_sublayout(target, source));
        },
        [this](PanoramaNode& target, const std::string& name) {
            (void)session_.instantiate_snippet(target, name);
        },
        [this](const std::string& name) {
            return session_.has_snippet(name);
        });
    runtime_.set_focus_request_handler([this](PanoramaNode* node) {
        set_focus(node);
    });
}

void PanoramaView::run_added_scripts(const PanoramaDocumentLoadResult& result)
{
    if (!result.loaded || !runtime_.active())
    {
        return;
    }
    for (const PanoramaScriptInclude& script : result.scripts_added)
    {
        const std::string source = session_.read_text_resource(script.path);
        if (source.empty())
        {
            pano_log_warning("Panorama view: sublayout script not found: {}", script.path);
            continue;
        }
        PanoramaNode* context = script.context != nullptr ? script.context : root();
        if (context != nullptr)
        {
            runtime_.run_source_in_context(source, "panorama://" + script.path, *context);
        }
    }
}

bool PanoramaView::recompute_styles(bool forced_full)
{
    PanoramaNode* document_root = root();
    if (document_root == nullptr)
    {
        return false;
    }

    bool layout_damage = forced_full;
    PanoramaAnimCaptureResult capture;
    if (forced_full)
    {
        session_.style_sheet().compute(*document_root);
        (void)apply_presentation_and_stabilize(session_.style_sheet(), *document_root);
        capture = panorama_capture_anim_targets_with_result(*document_root);
    }
    else
    {
        std::vector<PanoramaNode*> invalidated_roots;
        collect_invalidated_roots(*document_root, invalidated_roots);
        if (invalidated_roots.empty())
        {
            // A coarse dirty request without a routed node mark cannot be
            // made selective safely.
            session_.style_sheet().compute(*document_root);
            (void)apply_presentation_and_stabilize(session_.style_sheet(), *document_root);
            capture = panorama_capture_anim_targets_with_result(*document_root);
            layout_damage = true;
        }
        else
        {
            std::vector<LayoutStyleBefore> layout_before;
            for (PanoramaNode* dirty_root : invalidated_roots)
            {
                append_subtree_snapshots(*dirty_root, layout_before);
            }

            session_.style_sheet().compute_invalidated(*document_root);
            // These compatibility presentation passes can synthesize control
            // internals. They remain full-tree until their public APIs expose
            // exact damage routing; cascade, animation, layout and paint are
            // nevertheless selective/gated.
            layout_damage =
                apply_presentation_and_stabilize(session_.style_sheet(), *document_root) || layout_damage;

            for (const LayoutStyleBefore& before : layout_before)
            {
                // Candidate roots conservatively include following siblings in
                // case the sheet has sibling combinators. Compare only nodes
                // the cascade actually touched; an untouched auto-sized node's
                // computed width/height may legitimately contain layout's
                // resolved value rather than its animation target.
                if (before.node->style_fresh &&
                    !same_layout_signature(before.signature, layout_signature(*before.node, true)))
                {
                    layout_damage = true;
                    break;
                }
            }
            for (PanoramaNode* dirty_root : invalidated_roots)
            {
                merge_capture(capture, panorama_capture_anim_targets_recomputed(*dirty_root));
            }
        }
    }

    if (forced_full)
    {
        transitions_active_ = capture.any_transition_animating;
        keyframes_active_ = capture.any_keyframe_candidate;
    }
    else
    {
        transitions_active_ = transitions_active_ || capture.any_transition_animating;
        keyframes_active_ = keyframes_active_ || capture.any_keyframe_candidate;
    }
    style_dirty_ = false;
    visual_dirty_ = true;
    return layout_damage;
}

void PanoramaView::relayout()
{
    PanoramaNode* document_root = root();
    if (document_root == nullptr)
    {
        return;
    }
    layout_panorama_tree(*document_root, viewport_width_, viewport_height_, text_measure_);
    layout_dirty_ = false;
    visual_dirty_ = true;
}

bool PanoramaView::patch_recomposite_draw_list()
{
    recomposite_contexts_.resize(draw_list_.contexts.size());
    for (std::size_t i = 0; i < draw_list_.contexts.size(); ++i)
    {
        const PanoramaLayerContextEntry& entry = draw_list_.contexts[i];
        if (entry.source_node == nullptr || entry.parent_context_index >= static_cast<int>(i))
        {
            return false;
        }
        const PanoramaNode* parent_source = nullptr;
        PanoramaDrawConstants parent_constants;
        if (entry.parent_context_index >= 0)
        {
            const std::size_t parent_index = static_cast<std::size_t>(entry.parent_context_index);
            if (parent_index >= draw_list_.contexts.size())
            {
                return false;
            }
            parent_source = draw_list_.contexts[parent_index].source_node;
            parent_constants = recomposite_contexts_[parent_index];
        }
        recomposite_contexts_[i] =
            panorama_recompute_layer_context(*entry.source_node, parent_source, parent_constants);
    }

    // A newly promoted animation node cannot be represented by the retained
    // context table; style-start frames normally rebuild and establish it.
    for (const PanoramaNode* dirty : recomposite_dirty_nodes_)
    {
        if (dirty == nullptr || !panorama_node_opens_layer_context(*dirty))
        {
            continue;
        }
        const bool represented = std::any_of(draw_list_.contexts.begin(), draw_list_.contexts.end(),
            [dirty](const PanoramaLayerContextEntry& entry) { return entry.source_node == dirty; });
        if (!represented)
        {
            return false;
        }
    }

    // Preflight every changed command before mutating anything so failure is
    // atomic and a caller can safely fall back to a full paint.
    for (const PanoramaDrawCommand& command : draw_list_.commands)
    {
        if (command.context_index >= 0)
        {
            const std::size_t context_index = static_cast<std::size_t>(command.context_index);
            if (context_index >= recomposite_contexts_.size())
            {
                return false;
            }
            if (!(command.constants == recomposite_contexts_[context_index]) && !command.constants_patchable)
            {
                return false;
            }
        }
        else if (!command.constants.is_identity() && !command.constants_patchable)
        {
            return false;
        }
    }

    for (PanoramaDrawCommand& command : draw_list_.commands)
    {
        if (command.context_index >= 0)
        {
            command.constants = recomposite_contexts_[static_cast<std::size_t>(command.context_index)];
        }
        if (command.blur_source_node != nullptr)
        {
            const PanoramaBlur& blur = command.blur_source_node->computed.blur;
            command.blur_std_x = std::max(0.0F, blur.std_x);
            command.blur_std_y = std::max(0.0F, blur.std_y);
            command.blur_passes = std::max(1, static_cast<int>(blur.passes + 0.5F));
        }
    }
    return true;
}

void PanoramaView::rebuild_draw_list()
{
    PanoramaNode* document_root = root();
    if (document_root == nullptr)
    {
        draw_list_.commands.clear();
        return;
    }
    if (font_atlas_ != nullptr)
    {
        font_atlas_->ensure_tree_text(*document_root);
        font_atlas_->upload_if_dirty();
        glyph_source_ = font_atlas_->glyph_source();
    }
    build_panorama_draw_list(draw_list_, *document_root, glyph_source_, &paint_scratch_);
    visual_dirty_ = false;
}
}
