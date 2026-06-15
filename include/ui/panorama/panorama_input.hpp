#pragma once

#include "ui/panorama/panorama_dom.hpp"

// Panorama pointer interaction over a laid-out PanoramaNode tree. The engine owns
// the full input semantics — hit-testing (including open dropdown popups painted
// out of normal flow), hover/active/focus pseudo-class flags, event bubbling
// (onactivate / onmouseover / onmouseout), radio-group exclusivity, and the
// DropDown open/select/dismiss emulation. The host only supplies pointer samples
// in design-space coordinates; how they are obtained (SDL, Win32, a test) is the
// host's business.
namespace openstrike
{
class PanoramaRuntime;

// Whether a node captures pointer input. Respects an explicit `hittest`; otherwise
// only solid/interactive panels capture, so the many full-screen but empty overlay
// containers (PopupManager, ContextMenuManager, ...) let input fall through to the
// menu beneath instead of swallowing all hover.
[[nodiscard]] bool panorama_node_is_hittable(const PanoramaNode& node);

// Front-to-back (last child first) hit test against the laid-out tree. Children
// of an open dropdown that paint as its popup are skipped here; use
// panorama_hit_test_open_dropdown_popup first so popups win over the page.
[[nodiscard]] PanoramaNode* panorama_hit_test(PanoramaNode& root, float x, float y);

// Hit test restricted to open dropdown popup geometry (popup_layout boxes), which
// paints on top of everything else. Null if no open popup is under the point.
[[nodiscard]] PanoramaNode* panorama_hit_test_open_dropdown_popup(PanoramaNode& root, float x, float y);

// JS-set visibility overrides (Panel.visible) are applied on top of the cascade so
// they survive style recompute; call after PanoramaStyleSheet::compute.
void panorama_apply_visibility_overrides(PanoramaNode& node);

// Control presentation pass: a closed dropdown collapses to its selected option
// (matching WebCore's select-popup anchor behavior). Call after the cascade +
// visibility overrides, before layout.
void panorama_apply_control_presentation(PanoramaNode& node);

// Tracks pointer state across frames and drives the interaction flags + event
// handlers. Typical host loop:
//
//   if (input.update_pointer(*root, x, y, lmb_down, runtime))
//       recompute styles (hover/active/focus/selected feed pseudo-class rules);
//
// update_pointer hit-tests the tree's CURRENT layout boxes, so hosts that lay out
// once per frame get one frame of latency (imperceptible). The controller
// observes node destruction and drops matching hover/focus/drag pointers on its
// own; reset() remains for wholesale tree swaps (it also clears pointer-button
// state, which destruction does not).
class PanoramaInputController : public PanoramaNodeLifetimeObserver
{
public:
    PanoramaInputController();
    ~PanoramaInputController() override;
    PanoramaInputController(const PanoramaInputController&) = delete;
    PanoramaInputController& operator=(const PanoramaInputController&) = delete;

    // Feeds one pointer sample (design-space coordinates + primary-button state).
    // Fires onmouseover/onmouseout transitions, onactivate on press (bubbled to the
    // nearest ancestor carrying the handler), radio-group selection, and dropdown
    // open/select/dismiss through `runtime` (may be null: flags still update, JS
    // handlers are skipped). Returns true when interaction state changed and the
    // host should recompute styles.
    bool update_pointer(PanoramaNode& root, float design_x, float design_y, bool down, PanoramaRuntime* runtime);

    // Feeds one mouse-wheel sample (design-space pointer position + wheel ticks,
    // SDL sign convention: +y = wheel up). Scrolls the innermost overflow:scroll
    // ancestor under the pointer that can still move in the wheel direction,
    // walking outward when it can't — WebCore's
    // EventHandler::handleWheelEventInAppropriateEnclosingBox propagation. One
    // tick = 3 lines x Scrollbar::pixelsPerLineStep() (40px), the WebKit/Windows
    // default. Returns true when a scroll offset changed; the host must relayout
    // (the layout pass applies the offset to the children).
    bool update_wheel(PanoramaNode& root, float design_x, float design_y, float wheel_ticks_y, PanoramaRuntime* runtime);

    // Forgets cached node pointers and pointer state. Call on tree rebuild/unload.
    void reset();

    // PanoramaNodeLifetimeObserver: drops hover/focus/drag pointers that match
    // the dying node (registered for the controller's lifetime).
    void on_panorama_node_destroyed(PanoramaNode& node) override;

    [[nodiscard]] PanoramaNode* hover_node() const noexcept { return hover_node_; }
    [[nodiscard]] PanoramaNode* focus_node() const noexcept { return focus_node_; }
    [[nodiscard]] bool pointer_down() const noexcept { return pointer_down_; }

private:
    void handle_dropdown_click(PanoramaNode& dropdown, PanoramaNode* hit, PanoramaRuntime* runtime);
    void begin_scrollbar_drag(PanoramaNode& scrollbar, float design_x, float design_y);
    bool update_scrollbar_drag(float design_x, float design_y);
    void end_scrollbar_drag();
    // Slider thumb drag (WebKit SliderThumbElement): maps the pointer to a 0..1
    // fraction over the rail and writes it to the slider's `value` attribute.
    void begin_slider_drag(PanoramaNode& slider, float design_x, float design_y);
    bool update_slider_drag(float design_x, float design_y);

    PanoramaNode* hover_node_ = nullptr;
    PanoramaNode* focus_node_ = nullptr;
    bool pointer_down_ = false;

    // Scroll-thumb drag (WebCore Scrollbar::moveThumb): the scroller + bar being
    // dragged and the grab offset between the pointer and the thumb's leading
    // edge at press time, so the thumb tracks the pointer without snapping.
    PanoramaNode* scrollbar_drag_scroller_ = nullptr;
    PanoramaNode* scrollbar_drag_bar_ = nullptr;
    bool scrollbar_drag_vertical_ = true;
    float scrollbar_drag_grab_ = 0.0F;

    // The Slider control whose thumb is being dragged (null when idle). The drag
    // reads the slider's track/thumb layout each move to map pointer -> value.
    PanoramaNode* slider_drag_node_ = nullptr;
};
}
