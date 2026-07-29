#pragma once

#include "ui/panorama/panorama_anim.hpp"
#include "ui/panorama/panorama_document_session.hpp"
#include "ui/panorama/panorama_font_atlas.hpp"
#include "ui/panorama/panorama_input.hpp"
#include "ui/panorama/panorama_layout.hpp"
#include "ui/panorama/panorama_paint.hpp"
#include "ui/panorama/panorama_runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace panorama
{
// High-level document options for PanoramaView. The low-level engine remains
// available component-by-component, but most standalone hosts should start here.
struct PanoramaViewLoadOptions
{
    PanoramaDocumentSessionOptions document;
    bool enable_scripting = true;
};

enum class PanoramaViewStyleWork : std::uint8_t
{
    None,
    Incremental,
    Full,
};

enum class PanoramaViewVisualWork : std::uint8_t
{
    None,
    Recomposite,
    Rebuild,
};

// The actual work classes selected for one update. This is intentionally more
// precise than the compatibility booleans in PanoramaViewUpdateResult.
struct PanoramaViewWorkPlan
{
    bool runtime_pumped = false;
    PanoramaViewStyleWork style = PanoramaViewStyleWork::None;
    bool transitions_advanced = false;
    bool keyframes_advanced = false;
    bool scroll_animations_advanced = false;
    bool layout = false;
    PanoramaViewVisualWork visual = PanoramaViewVisualWork::None;
};

// Opt-in counters for regression tests and profilers. update() resets the
// supplied object before recording one frame; no object means no counter work.
struct PanoramaViewWorkStats
{
    std::uint64_t full_style_passes = 0;
    std::uint64_t incremental_style_passes = 0;
    std::uint64_t transition_passes = 0;
    std::uint64_t keyframe_passes = 0;
    std::uint64_t scroll_animation_passes = 0;
    std::uint64_t transition_nodes_visited = 0;
    std::uint64_t keyframe_nodes_visited = 0;
    std::uint64_t scroll_nodes_visited = 0;
    std::uint64_t layout_passes = 0;
    std::uint64_t draw_list_builds = 0;
    std::uint64_t recomposite_patches = 0;
    std::uint64_t recomposite_fallbacks = 0;
};

enum class PanoramaViewUpdateMode : std::uint8_t
{
    Incremental,
    ForcedFull,
};

struct PanoramaViewUpdateOptions
{
    // ForcedFull is a compatibility/reference oracle: dirty style uses a full
    // cascade and all three animation trees are visited.
    PanoramaViewUpdateMode mode = PanoramaViewUpdateMode::Incremental;
    // Existing hosts key redraws on draw_list_rebuilt. Keep their behavior by
    // default; an aware host can opt into retained-list constants patching.
    bool allow_recomposite_only = false;
    PanoramaViewWorkStats* work_stats = nullptr;
};

// Describes the work performed by PanoramaView::update(). This is useful to
// hosts that want to skip their own compositing when the Panorama surface did
// not change, while draw_list_rebuilt tells custom renderers when the display
// list has new contents.
struct PanoramaViewUpdateResult
{
    bool style_changed = false;
    bool layout_changed = false;
    bool visual_changed = false;
    bool draw_list_rebuilt = false;
    bool animation_active = false;
    bool recomposite_only = false;
    std::size_t recomposite_dirty_node_count = 0;
    std::uint32_t recomposite_generation = 0;
    PanoramaViewWorkPlan work;
};

// Owns the host-independent parts of one live Panorama surface and sequences
// them correctly: document loading, script contexts and runtime sublayouts,
// input, cascade, animation, layout, and display-list rebuilding.
//
// A windowing or game host still supplies platform events and a renderer, but
// no longer has to duplicate PanoramaEngine's frame-ordering rules. Advanced
// integrations can access every owned subsystem through the accessors below or
// continue using the lower-level APIs directly.
class PanoramaView
{
public:
    PanoramaView();
    ~PanoramaView();

    PanoramaView(const PanoramaView&) = delete;
    PanoramaView& operator=(const PanoramaView&) = delete;

    // Registered resource providers survive unload() and a subsequent load(),
    // matching PanoramaDocumentSession. Configure runtime() (bootstrap scripts,
    // host actions, client) before load() when document scripts need those hooks.
    [[nodiscard]] bool load(std::string_view document_path, PanoramaViewLoadOptions options = {});
    void unload();

    [[nodiscard]] bool loaded() const noexcept;
    [[nodiscard]] PanoramaNode* root() noexcept;
    [[nodiscard]] const PanoramaNode* root() const noexcept;

    [[nodiscard]] PanoramaDocumentSession& session() noexcept;
    [[nodiscard]] const PanoramaDocumentSession& session() const noexcept;
    [[nodiscard]] PanoramaResourceManager& resources() noexcept;
    [[nodiscard]] const PanoramaResourceManager& resources() const noexcept;
    [[nodiscard]] PanoramaRuntime& runtime() noexcept;
    [[nodiscard]] const PanoramaRuntime& runtime() const noexcept;
    [[nodiscard]] PanoramaInputController& input() noexcept;
    [[nodiscard]] const PanoramaInputController& input() const noexcept;

    // Viewport dimensions are Panorama design units. Values <= 0 are clamped
    // to one. Resizing marks layout and paint dirty; update() performs the work.
    void set_viewport(float width, float height);
    [[nodiscard]] float viewport_width() const noexcept;
    [[nodiscard]] float viewport_height() const noexcept;

    // The atlas binding is the simplest built-in text path: it installs the
    // atlas measurer and ensures/rasterizes/uploads glyphs before paint through
    // the active PanoramaRenderBackend. The atlas remains caller-owned and must
    // stay alive while bound. Pass nullptr to return to default measurement
    // with text painting disabled.
    void set_font_atlas(PanoramaFontAtlas* atlas);

    // Custom text implementations should provide matching measurement and
    // glyph sources. Either call leaves the manual path active (unbinding a
    // PanoramaFontAtlas) and invalidates the appropriate downstream stages.
    void set_text_measure(PanoramaTextMeasure measure);
    void set_glyph_source(PanoramaGlyphSource glyphs);

    // Platform-neutral input entry points. Coordinates are design-space values.
    // Each method marks the exact downstream stages dirty when state changes.
    bool update_pointer(float x, float y, bool down);
    bool update_wheel(float x, float y, float wheel_ticks_y);
    bool handle_key_down(const PanoramaKeyEvent& event);
    bool handle_text_input(std::string_view utf8);
    // Clipboard access remains host-owned. Read the platform clipboard in
    // response to the host's paste shortcut/command, then pass its UTF-8 payload
    // here. The focused TextEntry applies normal selection/maxchars semantics.
    bool handle_paste(std::string_view utf8);
    void set_focus(PanoramaNode* node);

    // Pumps scripts and animations, then conditionally recomputes styles,
    // layout, and the renderer-independent draw list. dt is in seconds.
    [[nodiscard]] PanoramaViewUpdateResult update(float dt_seconds);
    [[nodiscard]] PanoramaViewUpdateResult update(float dt_seconds, PanoramaViewUpdateOptions options);

    // Use these after native code mutates the DOM without going through input
    // or PanoramaRuntime. invalidate_style() also marks the root for cascade.
    void invalidate_style();
    void invalidate_layout();
    void invalidate_visual();

    [[nodiscard]] const PanoramaDrawList& draw_list() const noexcept;
    // Stable until the next update()/unload(). Useful with recomposite_only to
    // patch a host-side geometry cache without scanning the whole node tree.
    [[nodiscard]] const std::vector<PanoramaNode*>& recomposite_dirty_nodes() const noexcept;

private:
    void configure_runtime_bridges();
    void run_added_scripts(const PanoramaDocumentLoadResult& result);
    void request_local_style_update(bool content_layout_changed = false);
    bool recompute_styles(bool forced_full);
    void relayout();
    void rebuild_draw_list();
    bool patch_recomposite_draw_list();

    // Declaration order is intentional: input/runtime observers are destroyed
    // before the session-owned node tree.
    PanoramaDocumentSession session_;
    PanoramaRuntime runtime_;
    PanoramaInputController input_;
    PanoramaFontAtlas* font_atlas_ = nullptr;
    PanoramaTextMeasure text_measure_ = default_text_measure();
    PanoramaGlyphSource glyph_source_;
    PanoramaDrawList draw_list_;
    PanoramaPaintScratch paint_scratch_;
    float viewport_width_ = 1280.0F;
    float viewport_height_ = 720.0F;
    bool loaded_ = false;
    bool style_dirty_ = false;
    bool layout_dirty_ = false;
    bool visual_dirty_ = false;
    bool force_full_style_dirty_ = false;
    bool transitions_active_ = false;
    bool keyframes_active_ = false;
    bool scroll_animations_active_ = false;
    std::vector<PanoramaNode*> recomposite_dirty_nodes_;
    std::vector<PanoramaDrawConstants> recomposite_contexts_;
    std::uint32_t recomposite_generation_ = 0;
};
}
