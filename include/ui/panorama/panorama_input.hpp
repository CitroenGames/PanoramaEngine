#pragma once

#include "ui/panorama/panorama_dom.hpp"

#include <cstdint>
#include <string_view>
#include <vector>

// Panorama pointer interaction over a laid-out PanoramaNode tree. The engine owns
// the full input semantics — hit-testing (including open dropdown popups painted
// out of normal flow), hover/active/focus pseudo-class flags, event bubbling
// (onactivate / onmouseover / onmouseout), radio-group exclusivity, and the
// DropDown open/select/dismiss emulation. The host only supplies pointer samples
// in design-space coordinates; how they are obtained (SDL, Win32, a test) is the
// host's business.
namespace panorama
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

// Engine-native key identity for the keys the input controller acts on. The host
// translates its platform keycodes (SDL_Keycode, VK_*) into these; printable
// character entry comes in separately through handle_text_input (the platform's
// composed-text event), mirroring WebCore's split between keydown and textInput.
enum class PanoramaKey : std::uint16_t
{
    Unknown = 0,
    ArrowLeft,
    ArrowRight,
    ArrowUp,
    ArrowDown,
    Home,
    End,
    Backspace,
    Delete,
    Enter,
    Tab,
    Escape,
    A, // for Ctrl+A (Select All); other letters arrive via handle_text_input
};

struct PanoramaKeyEvent
{
    PanoramaKey key = PanoramaKey::Unknown;
    bool shift = false;
    bool ctrl = false;
    bool alt = false;
};

// Document-order tab sequence (WebCore FocusController): positive tabindex first
// (ascending), then tabindex 0 / focusable controls without an explicit index, in
// document order; negative tabindex and hidden/disabled controls are excluded.
[[nodiscard]] std::vector<PanoramaNode*> panorama_collect_tab_order(PanoramaNode& root);

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

    // Feeds one non-character key press (WebCore EventHandler::keyEvent ->
    // defaultKeyboardEventHandler + Editor command bindings). Drives caret motion /
    // deletion / select-all on the focused TextEntry, Tab/Shift+Tab focus
    // navigation, and Enter activation/submit. Printable characters do NOT come
    // through here — they arrive via handle_text_input. Returns true when the tree
    // changed and the host should recompute/relayout. `runtime` may be null (state
    // still updates; JS handlers are skipped).
    bool handle_key_down(PanoramaNode& root, const PanoramaKeyEvent& event, PanoramaRuntime* runtime);

    // Feeds composed printable text (the platform's textInput/IME event) into the
    // focused TextEntry, inserting it at the caret (replacing any selection). Fires
    // ontextentrychange (plus the legacy ontextentrychanged alias). Returns true
    // when a field changed.
    bool handle_text_input(PanoramaNode& root, std::string_view utf8, PanoramaRuntime* runtime);

    // Inserts UTF-8 text supplied by the host clipboard into the focused
    // TextEntry. Paste deliberately takes the clipboard payload instead of
    // reading a platform API: SDL, Win32, X11, browser, and asynchronous hosts
    // retain ownership of clipboard access. The edit uses the same single-line,
    // selection-replacement, maxchars, dirty-state, and ontextentrychanged path
    // as composed text input. Returns true when the field value changed.
    bool handle_paste(PanoramaNode& root, std::string_view utf8, PanoramaRuntime* runtime);

    // Moves keyboard focus to `node` (null clears focus): blurs the old focus
    // (onblur), focuses the new (onfocus), updates :focus / :focus-within, and seeds
    // a TextEntry's caret at its value end (WebCore setFocusedElement order). Safe to
    // call with a node outside `root`'s tree (no-op-ish; focus still set).
    void set_focus(PanoramaNode& root, PanoramaNode* node, PanoramaRuntime* runtime);

    // Tab/Shift+Tab focus advance (FocusController::advanceFocusInDocumentOrder),
    // wrapping at the ends. Returns true when focus moved.
    bool advance_focus(PanoramaNode& root, bool forward, PanoramaRuntime* runtime);

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
