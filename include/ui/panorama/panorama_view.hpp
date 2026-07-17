#pragma once

#include "ui/panorama/panorama_anim.hpp"
#include "ui/panorama/panorama_document_session.hpp"
#include "ui/panorama/panorama_font_atlas.hpp"
#include "ui/panorama/panorama_input.hpp"
#include "ui/panorama/panorama_layout.hpp"
#include "ui/panorama/panorama_paint.hpp"
#include "ui/panorama/panorama_runtime.hpp"

#include <string_view>

namespace panorama
{
// High-level document options for PanoramaView. The low-level engine remains
// available component-by-component, but most standalone hosts should start here.
struct PanoramaViewLoadOptions
{
    PanoramaDocumentSessionOptions document;
    bool enable_scripting = true;
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
    void set_focus(PanoramaNode* node);

    // Pumps scripts and animations, then conditionally recomputes styles,
    // layout, and the renderer-independent draw list. dt is in seconds.
    [[nodiscard]] PanoramaViewUpdateResult update(float dt_seconds);

    // Use these after native code mutates the DOM without going through input
    // or PanoramaRuntime. invalidate_style() also marks the root for cascade.
    void invalidate_style();
    void invalidate_layout();
    void invalidate_visual();

    [[nodiscard]] const PanoramaDrawList& draw_list() const noexcept;

private:
    void configure_runtime_bridges();
    void run_added_scripts(const PanoramaDocumentLoadResult& result);
    void recompute_styles();
    void relayout();
    void rebuild_draw_list();

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
};
}
