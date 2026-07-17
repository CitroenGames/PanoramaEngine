#include "ui/panorama/panorama_view.hpp"

#include "ui/panorama/panorama_log.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace panorama
{
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

    // Give init-time scripts valid panel geometry. Some real Panorama scripts
    // inspect actual layout dimensions while installing their initial state.
    recompute_styles();
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
    }

    if (style_dirty_)
    {
        recompute_styles();
        relayout();
    }
    rebuild_draw_list();

    style_dirty_ = false;
    layout_dirty_ = false;
    visual_dirty_ = false;
    return true;
}

void PanoramaView::unload()
{
    runtime_.shutdown();
    input_.reset();
    session_.clear_document();
    draw_list_.commands.clear();
    paint_scratch_.reusable_commands.clear();
    loaded_ = false;
    style_dirty_ = false;
    layout_dirty_ = false;
    visual_dirty_ = false;
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
        invalidate_style();
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
        invalidate_style();
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
        invalidate_style();
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
    invalidate_style();
}

PanoramaViewUpdateResult PanoramaView::update(float dt_seconds)
{
    PanoramaViewUpdateResult result;
    PanoramaNode* document_root = root();
    if (document_root == nullptr)
    {
        return result;
    }

    dt_seconds = std::max(dt_seconds, 0.0F);
    if (runtime_.active())
    {
        runtime_.update(static_cast<double>(dt_seconds));
        style_dirty_ = runtime_.consume_dirty() || style_dirty_;
    }

    // Native callers can mutate nodes through root()/session() and simply call
    // mark_style_dirty(). Honor those marks without requiring a second view API.
    style_dirty_ = document_root->style_dirty || document_root->descendant_style_dirty || style_dirty_;
    if (style_dirty_)
    {
        recompute_styles();
        result.style_changed = true;
    }

    PanoramaAnimationAdvanceResult transitions = panorama_advance_anim(*document_root, dt_seconds);
    for (const PanoramaTransitionEnd& ended : transitions.transition_ends)
    {
        if (runtime_.active() && ended.node != nullptr && ended.property != nullptr)
        {
            runtime_.dispatch_property_transition_end(*ended.node, ended.property);
        }
    }
    const PanoramaAnimationAdvanceResult keyframes =
        panorama_advance_keyframes(*document_root, session_.style_sheet().keyframes(), dt_seconds);
    const PanoramaAnimationAdvanceResult scrolls = panorama_advance_scroll_animations(*document_root, dt_seconds);

    result.animation_active = transitions.active || keyframes.active || scrolls.active;
    layout_dirty_ = transitions.layout_changed || keyframes.layout_changed || scrolls.layout_changed || layout_dirty_;
    visual_dirty_ = transitions.visual_changed || keyframes.visual_changed || visual_dirty_;

    if (layout_dirty_)
    {
        relayout();
        result.layout_changed = true;
    }
    if (visual_dirty_)
    {
        rebuild_draw_list();
        result.draw_list_rebuilt = true;
        result.visual_changed = true;
    }

    style_dirty_ = false;
    layout_dirty_ = false;
    visual_dirty_ = false;
    return result;
}

void PanoramaView::invalidate_style()
{
    style_dirty_ = true;
    layout_dirty_ = true;
    visual_dirty_ = true;
    if (PanoramaNode* document_root = root())
    {
        document_root->mark_style_dirty();
    }
}

void PanoramaView::invalidate_layout()
{
    layout_dirty_ = true;
    visual_dirty_ = true;
}

void PanoramaView::invalidate_visual()
{
    visual_dirty_ = true;
}

const PanoramaDrawList& PanoramaView::draw_list() const noexcept
{
    return draw_list_;
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

void PanoramaView::recompute_styles()
{
    PanoramaNode* document_root = root();
    if (document_root == nullptr)
    {
        return;
    }
    session_.style_sheet().compute(*document_root);
    panorama_apply_visibility_overrides(*document_root);
    panorama_apply_control_presentation(*document_root);
    panorama_capture_anim_targets(*document_root);
    style_dirty_ = false;
    layout_dirty_ = true;
    visual_dirty_ = true;
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
