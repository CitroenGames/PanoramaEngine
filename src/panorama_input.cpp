#include "ui/panorama/panorama_input.hpp"

#include "ui/panorama/panorama_log.hpp"
#include "ui/panorama/panorama_runtime.hpp"
#include "ui/panorama/panorama_text_edit.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

namespace panorama
{
namespace
{
const PanoramaNode* open_dropdown_header_child(const PanoramaNode& node)
{
    if (!panorama_node_collapses_to_selected_child(node) || !panorama_dropdown_is_open(node))
    {
        return nullptr;
    }
    return panorama_dropdown_selected_child(node);
}

bool is_open_dropdown_popup_child(const PanoramaNode& parent, const PanoramaNode& child)
{
    return open_dropdown_header_child(parent) != nullptr && child.has_popup_layout;
}

bool paints_in_normal_dropdown_flow(const PanoramaNode& parent, const PanoramaNode& child)
{
    const PanoramaNode* header = open_dropdown_header_child(parent);
    return header == nullptr || &child == header;
}

bool point_inside(const PanoramaLayoutBox& box, float x, float y)
{
    return x >= box.x && x < box.x + box.width && y >= box.y && y < box.y + box.height;
}

bool node_has_handler(const PanoramaNode& node, const char* handler)
{
    const auto it = node.attributes.find(handler);
    return it != node.attributes.end() && !it->second.empty();
}

std::vector<PanoramaNode*> node_chain(PanoramaNode* node)
{
    std::vector<PanoramaNode*> chain;
    for (PanoramaNode* n = node; n != nullptr; n = n->parent)
    {
        chain.push_back(n);
    }
    return chain;
}

// Fires onmouseout up the chain left behind and onmouseover down the chain
// entered (outermost first), skipping the common ancestry.
void run_pointer_transition_handlers(PanoramaRuntime* runtime, PanoramaNode* old_hit, PanoramaNode* new_hit)
{
    if (runtime == nullptr || old_hit == new_hit)
    {
        return;
    }

    std::vector<PanoramaNode*> left = node_chain(old_hit);
    std::vector<PanoramaNode*> entered = node_chain(new_hit);
    while (!left.empty() && !entered.empty() && left.back() == entered.back())
    {
        left.pop_back();
        entered.pop_back();
    }

    // A handler may delete arbitrary panels (including later entries in these
    // chains); the watches null destroyed entries so iteration skips them.
    PanoramaScopedNodeWatch left_watch(std::move(left));
    PanoramaScopedNodeWatch entered_watch(std::move(entered));

    for (PanoramaNode* node : left_watch.nodes())
    {
        if (node != nullptr && node_has_handler(*node, "onmouseout"))
        {
            runtime->run_node_handler(*node, "onmouseout");
        }
    }
    const auto& entered_nodes = entered_watch.nodes();
    for (auto it = entered_nodes.rbegin(); it != entered_nodes.rend(); ++it)
    {
        if (*it != nullptr && node_has_handler(**it, "onmouseover"))
        {
            runtime->run_node_handler(**it, "onmouseover");
        }
    }
}

// Nearest node (self or ancestor) with a non-empty attribute. Panorama events
// bubble, so a click on a button's icon must still fire the button's onactivate.
PanoramaNode* ancestor_with_attr(PanoramaNode* node, const char* attr)
{
    for (; node != nullptr; node = node->parent)
    {
        const auto it = node->attributes.find(attr);
        if (it != node->attributes.end() && !it->second.empty())
        {
            return node;
        }
    }
    return nullptr;
}

bool node_focusable(const PanoramaNode& node)
{
    if (node.attributes.count("tabindex") != 0 || node.attributes.count("onactivate") != 0)
    {
        return true;
    }
    return panorama_node_is_focusable_control(node);
}

PanoramaNode* ancestor_focusable(PanoramaNode* node)
{
    for (; node != nullptr; node = node->parent)
    {
        if (node_focusable(*node))
        {
            return node;
        }
    }
    return nullptr;
}

// Disabled state, mirroring panorama_style.cpp's node_disabled (a `.disabled`
// class or enabled="false"): such a control is skipped by the tab sequence.
bool node_enabled(const PanoramaNode& node)
{
    if (std::find(node.classes.begin(), node.classes.end(), "disabled") != node.classes.end())
    {
        return false;
    }
    const auto it = node.attributes.find("enabled");
    return it == node.attributes.end() || it->second != "false";
}

// FocusController shadowAdjustedTabIndex: explicit tabindex, else 0 for a
// focusable control. A control absent from the tab sequence (tabindex < 0) returns
// -1 and is filtered out by the collector.
int node_tab_index(const PanoramaNode& node)
{
    const auto it = node.attributes.find("tabindex");
    if (it != node.attributes.end())
    {
        return std::atoi(it->second.c_str());
    }
    return 0;
}

// Document-order walk collecting candidates, pruning subtrees that are not visible
// (an invisible ancestor removes its whole subtree from the tab sequence, like a
// display:none render-tree gap in WebCore).
void collect_focus_candidates(PanoramaNode& node, std::vector<PanoramaNode*>& out)
{
    if (!node.computed.visible)
    {
        return;
    }
    if (node_focusable(node) && node_enabled(node) && node_tab_index(node) >= 0)
    {
        out.push_back(&node);
    }
    for (const auto& child : node.children)
    {
        collect_focus_candidates(*child, out);
    }
}

// Nearest node (self or ancestor) that is a radio button in a group.
PanoramaNode* ancestor_radio(PanoramaNode* node)
{
    for (; node != nullptr; node = node->parent)
    {
        if (panorama_node_is_radio_button(*node) && node->attributes.count("group") != 0)
        {
            return node;
        }
    }
    return nullptr;
}

// Nearest node (self or ancestor) that is a ToggleButton. Valve's ToggleButton
// control flips its own selected state on activate (the JS onactivate handler
// then reads `checked`, e.g. the play menu's map tiles); without this no
// `:selected` tile styling can ever apply.
PanoramaNode* ancestor_toggle(PanoramaNode* node)
{
    for (; node != nullptr; node = node->parent)
    {
        if (panorama_node_is_toggle_button(*node))
        {
            return node;
        }
    }
    return nullptr;
}

// Clears `selected` on every node sharing `group` (radio-button exclusivity).
void clear_group_selection(PanoramaNode& node, const std::string& group)
{
    const auto it = node.attributes.find("group");
    if (it != node.attributes.end() && it->second == group && node.selected)
    {
        panorama_set_node_selected(node, false);
    }
    for (const auto& child : node.children)
    {
        clear_group_selection(*child, group);
    }
}

// Nearest self-or-ancestor that is synthesized scrollbar chrome (the bar node;
// the thumb is its child, so the walk lands on the bar from either).
PanoramaNode* ancestor_scrollbar(PanoramaNode* node)
{
    for (; node != nullptr; node = node->parent)
    {
        if (panorama_node_is_scrollbar(*node))
        {
            return node;
        }
    }
    return nullptr;
}

// Nearest self-or-ancestor that is a Slider control (owns a draggable thumb).
PanoramaNode* ancestor_slider(PanoramaNode* node)
{
    for (; node != nullptr; node = node->parent)
    {
        if (panorama_node_is_slider(*node))
        {
            return node;
        }
    }
    return nullptr;
}

// Nearest self-or-ancestor that behaves as a dropdown (collapses to its selection).
PanoramaNode* ancestor_dropdown(PanoramaNode* node)
{
    for (; node != nullptr; node = node->parent)
    {
        if (panorama_node_collapses_to_selected_child(*node))
        {
            return node;
        }
    }
    return nullptr;
}

// The direct child of `dropdown` that is `hit` (or an ancestor of `hit`) — i.e. the
// option row the cursor is over. Null if the hit is not inside an option.
PanoramaNode* dropdown_option_for_hit(PanoramaNode& dropdown, PanoramaNode* hit)
{
    for (PanoramaNode* node = hit; node != nullptr; node = node->parent)
    {
        if (node->parent == &dropdown)
        {
            return node;
        }
    }
    return nullptr;
}

// Closes every open dropdown except `keep` — drives click-away dismissal.
void close_open_dropdowns(PanoramaNode& node, const PanoramaNode* keep)
{
    if (&node != keep && panorama_node_collapses_to_selected_child(node) && panorama_dropdown_is_open(node))
    {
        panorama_set_dropdown_open(node, false);
    }
    for (const auto& child : node.children)
    {
        close_open_dropdowns(*child, keep);
    }
}
}

bool panorama_node_is_hittable(const PanoramaNode& node)
{
    const auto hittest = node.attributes.find("hittest");
    if (hittest != node.attributes.end())
    {
        if (hittest->second == "false")
        {
            return false;
        }
        if (hittest->second == "true")
        {
            return true;
        }
    }
    if (node.computed.background_color.a > 0 || node.paint_texture != 0 || node.background_texture != 0 || !node.text.empty())
    {
        return true;
    }
    for (const auto& [key, value] : node.attributes)
    {
        if (key.size() > 2 && key[0] == 'o' && key[1] == 'n' && !value.empty())
        {
            return true; // has a non-empty event handler (onactivate, onmouseover, ...)
        }
    }
    return false;
}

PanoramaNode* panorama_hit_test_open_dropdown_popup(PanoramaNode& root, float x, float y)
{
    if (!root.computed.visible || !root.subtree_has_popup_layout)
    {
        return nullptr;
    }

    for (auto it = root.children.rbegin(); it != root.children.rend(); ++it)
    {
        if (PanoramaNode* hit = panorama_hit_test_open_dropdown_popup(**it, x, y))
        {
            return hit;
        }
    }

    if (root.has_popup_layout && open_dropdown_header_child(root) != nullptr)
    {
        for (auto it = root.children.rbegin(); it != root.children.rend(); ++it)
        {
            PanoramaNode& child = **it;
            if (child.computed.visible && child.has_popup_layout && point_inside(child.popup_layout, x, y))
            {
                return &child;
            }
        }
        if (point_inside(root.popup_layout, x, y))
        {
            return &root;
        }
    }

    return nullptr;
}

PanoramaNode* panorama_hit_test(PanoramaNode& root, float x, float y)
{
    if (!root.computed.visible)
    {
        return nullptr;
    }
    const PanoramaLayoutBox& b = root.layout;

    // overflow:scroll containers clip hit testing the way they clip paint
    // (WebCore hit-tests through the overflow clip rect): a row scrolled out of
    // the viewport overlaps whatever sits above/below the scroller and must not
    // steal its hover/clicks. Restricted to scroll containers so the long-standing
    // unclipped behavior of plain squish/clip panels is untouched.
    bool descend = true;
    if (panorama_node_scrolls_x(root) || panorama_node_scrolls_y(root))
    {
        const float border_l = std::max(0.0F, root.computed.border_left());
        const float border_r = std::max(0.0F, root.computed.border_right());
        const float border_t = std::max(0.0F, root.computed.border_top());
        const float border_b = std::max(0.0F, root.computed.border_bottom());
        const bool outside_x = root.computed.overflow_clip_x &&
            (x < b.x + border_l || x >= b.x + std::max(border_l, b.width - border_r));
        const bool outside_y = root.computed.overflow_clip_y &&
            (y < b.y + border_t || y >= b.y + std::max(border_t, b.height - border_b));
        descend = !outside_x && !outside_y;
    }

    // Children paint over parents, so test them front-to-back (last child first).
    if (descend)
    {
        for (auto it = root.children.rbegin(); it != root.children.rend(); ++it)
        {
            if (is_open_dropdown_popup_child(root, **it) && !paints_in_normal_dropdown_flow(root, **it))
            {
                continue;
            }
            if (PanoramaNode* hit = panorama_hit_test(**it, x, y))
            {
                return hit;
            }
        }
    }
    const bool inside = point_inside(b, x, y);
    return (inside && panorama_node_is_hittable(root)) ? &root : nullptr;
}

void panorama_apply_visibility_overrides(PanoramaNode& node)
{
    if (node.visibility_override == 0)
    {
        node.computed.visible = false;
    }
    else if (node.visibility_override == 1)
    {
        node.computed.visible = true;
    }
    for (const auto& child : node.children)
    {
        panorama_apply_visibility_overrides(*child);
    }
}

void panorama_apply_control_presentation(PanoramaNode& node)
{
    // Toggle/radio buttons own internal children (.TickBox/.RadioBox + the
    // control-text label) in real Panorama; materialize them idempotently.
    ensure_panorama_selection_control_internals(node);

    // overflow:scroll panels own scrollbar chrome (VerticalScrollBar +
    // .ScrollThumb) in real Panorama; CS:GO styles it by tag/class.
    ensure_panorama_scrollbar_internals(node);

    // Settings rows (CSGOSettingsSlider) and the Slider control own internal
    // chrome too; materialize it so the cascade/layout can style and place it.
    // Order matters: building the row's #Slider first lets the same recursive
    // pass (below) build that slider's track/thumb on this frame.
    ensure_panorama_settings_slider_internals(node);
    ensure_panorama_slider_internals(node);

    // Settings key-rebind rows (CSGOSettingsKeyBinder) own a labelled bind button +
    // a clear button; materialize them so the host can show the bound key + capture.
    ensure_panorama_settings_keybinder_internals(node);

    // A dropdown collapses to its selected option when closed. While open,
    // layout keeps the control anchored and positions the option rows as a
    // popup below it, matching WebCore's select-popup anchor behavior.
    if (panorama_node_collapses_to_selected_child(node))
    {
        if (!panorama_dropdown_is_open(node))
        {
            const PanoramaNode* selected = panorama_dropdown_selected_child(node);
            for (const auto& child : node.children)
            {
                child->computed.visible = child.get() == selected && child->computed.visible;
            }
        }
    }

    for (const auto& child : node.children)
    {
        panorama_apply_control_presentation(*child);
    }
}

bool PanoramaInputController::update_pointer(
    PanoramaNode& root, float design_x, float design_y, bool down, PanoramaRuntime* runtime)
{
    bool state_changed = false;

    PanoramaNode* hit = panorama_hit_test_open_dropdown_popup(root, design_x, design_y);
    if (hit == nullptr)
    {
        hit = panorama_hit_test(root, design_x, design_y);
    }
    if (hit != hover_node_)
    {
        PanoramaNode* old_hover = hover_node_;
        // Only the nodes whose `hovered` value actually flips are marked dirty:
        // both chains share their ancestry above the divergence point, and those
        // common ancestors keep hovered == true.
        for (PanoramaNode* n = hover_node_; n != nullptr; n = n->parent)
        {
            if (n->hovered)
            {
                n->hovered = false;
                n->mark_style_dirty();
            }
        }
        hover_node_ = hit;
        for (PanoramaNode* n = hit; n != nullptr; n = n->parent)
        {
            if (!n->hovered)
            {
                n->hovered = true;
                n->mark_style_dirty();
            }
        }
        run_pointer_transition_handlers(runtime, old_hover, hit);
        state_changed = true;
    }

    // An in-progress thumb drag tracks every pointer move while the button is
    // held (WebCore Scrollbar::moveThumb), even once the cursor leaves the bar.
    if (scrollbar_drag_scroller_ != nullptr && down && update_scrollbar_drag(design_x, design_y))
    {
        state_changed = true;
    }

    // A slider thumb tracks the pointer while held, even once it leaves the rail
    // (WebKit SliderThumbElement drag), so a value can be dragged past the ends.
    if (slider_drag_node_ != nullptr && down && update_slider_drag(design_x, design_y))
    {
        state_changed = true;
    }

    if (down != pointer_down_)
    {
        pointer_down_ = down;
        if (!down)
        {
            end_scrollbar_drag();
            slider_drag_node_ = nullptr;
        }
        for (PanoramaNode* n = hover_node_; n != nullptr; n = n->parent)
        {
            if (n->active != down)
            {
                n->active = down;
                n->mark_style_dirty();
            }
        }
        if (down)
        {
            // A press focuses the nearest focusable ancestor (WebCore
            // EventHandler::handleMousePressEvent -> setFocusedElement); clicking
            // empty space clears focus. set_focus fires onblur/onfocus and seeds a
            // text field's caret.
            set_focus(root, hover_node_ != nullptr ? ancestor_focusable(hover_node_) : nullptr, runtime);
        }

        if (down && hover_node_ != nullptr)
        {
            // Scrollbar chrome owns its click: a press on the thumb grabs it, a
            // press on the track jumps the thumb to the pointer; neither fires
            // onactivate/selection on the panels beneath.
            if (PanoramaNode* scrollbar = ancestor_scrollbar(hover_node_))
            {
                close_open_dropdowns(root, nullptr);
                begin_scrollbar_drag(*scrollbar, design_x, design_y);
                return true;
            }
            // A slider owns its click the same way: the press jumps the thumb to the
            // pointer and starts a drag; the value tracks the cursor until release.
            if (PanoramaNode* slider = ancestor_slider(hover_node_))
            {
                close_open_dropdowns(root, nullptr);
                begin_slider_drag(*slider, design_x, design_y);
                return true;
            }
            // Dropdowns own the click: open the option menu, or pick an option. A click
            // anywhere else dismisses any open dropdown (click-away).
            PanoramaNode* dropdown = ancestor_dropdown(hover_node_);
            close_open_dropdowns(root, dropdown);
            if (dropdown != nullptr)
            {
                handle_dropdown_click(*dropdown, hover_node_, runtime);
            }
            else
            {
                // Bubble up: a click on a button's icon should select/activate the button.
                if (PanoramaNode* radio = ancestor_radio(hover_node_))
                {
                    clear_group_selection(root, radio->attributes["group"]);
                    panorama_set_node_selected(*radio, true);
                }
                else if (PanoramaNode* toggle = ancestor_toggle(hover_node_))
                {
                    // ToggleButton flips BEFORE onactivate runs so the handler reads
                    // the post-toggle `checked`, matching Valve's control. The shared
                    // setter keeps the `checked` class in sync — JS reads it back.
                    panorama_set_node_selected(*toggle, !toggle->selected);
                }
                PanoramaNode* target = ancestor_with_attr(hover_node_, "onactivate");
                if (target != nullptr && runtime != nullptr)
                {
                    pano_log_info("Panorama click: #{} <{}> onactivate", target->id, target->tag);
                    runtime->run_node_handler(*target, "onactivate");
                }
                else
                {
                    pano_log_info("Panorama click: #{} <{}> (no onactivate in ancestry)", hover_node_->id, hover_node_->tag);
                }
            }
        }
        state_changed = true;
    }

    return state_changed;
}

void PanoramaInputController::handle_dropdown_click(PanoramaNode& dropdown, PanoramaNode* hit, PanoramaRuntime* runtime)
{
    if (!panorama_dropdown_is_open(dropdown))
    {
        // Closed: the selected option is showing as the header; reveal the menu.
        panorama_set_dropdown_open(dropdown, true);
        pano_log_info("Panorama dropdown #{} opened", dropdown.id);
        return;
    }

    // Open: a click on an option commits it as the new selection; either way the
    // menu closes. CS:GO settings dropdowns react to selection via onuserinputsubmit.
    // The submit handler may delete the dropdown (or the whole menu it lives in),
    // so everything after it goes through a lifetime watch.
    PanoramaScopedNodeWatch watch({&dropdown});
    if (PanoramaNode* option = dropdown_option_for_hit(dropdown, hit))
    {
        const std::string dropdown_id = dropdown.id;
        const std::string option_id = option->id;
        panorama_select_dropdown_option(dropdown, *option);
        if (runtime != nullptr)
        {
            runtime->run_node_handler(dropdown, "onuserinputsubmit");
        }
        pano_log_info("Panorama dropdown #{} selected #{}", dropdown_id, option_id);
    }
    if (PanoramaNode* live_dropdown = watch.nodes()[0])
    {
        panorama_set_dropdown_open(*live_dropdown, false);
    }
}

bool PanoramaInputController::update_wheel(
    PanoramaNode& root, float design_x, float design_y, float wheel_ticks_y, PanoramaRuntime* /*runtime*/)
{
    if (wheel_ticks_y == 0.0F)
    {
        return false;
    }
    PanoramaNode* hit = panorama_hit_test_open_dropdown_popup(root, design_x, design_y);
    if (hit == nullptr)
    {
        hit = panorama_hit_test(root, design_x, design_y);
    }
    if (hit == nullptr)
    {
        return false;
    }

    // WebKit: a wheel tick is 3 lines (the Windows SPI_GETWHEELSCROLLLINES
    // default) of Scrollbar::pixelsPerLineStep() = 40px each. SDL's +y means
    // wheel-up, which scrolls the content up (offset decreases).
    constexpr float kPixelsPerWheelTick = 3.0F * 40.0F;
    const float delta = -wheel_ticks_y * kPixelsPerWheelTick;

    // WebCore EventHandler::handleWheelEventInAppropriateEnclosingBox: the
    // innermost enclosing scrollable handles the event iff it can actually move;
    // otherwise the event propagates to the next enclosing scrollable. The tick
    // retargets the smooth-scroll spring FROM ITS DESTINATION (not the displayed
    // offset), so repeated ticks accumulate and the glide stays C1-continuous;
    // panorama_advance_scroll_animations applies the motion each frame.
    for (PanoramaNode* n = hit; n != nullptr; n = n->parent)
    {
        if (!panorama_node_scrolls_y(*n) || n->max_scroll_y <= 0.0F)
        {
            continue;
        }
        const float from_y = panorama_scroll_destination_y(*n);
        const float dest_y = std::clamp(from_y + delta, 0.0F, n->max_scroll_y);
        if (dest_y != from_y)
        {
            panorama_smooth_scroll_to(*n, panorama_scroll_destination_x(*n), dest_y);
            return true;
        }
    }
    return false;
}

void PanoramaInputController::begin_scrollbar_drag(PanoramaNode& scrollbar, float design_x, float design_y)
{
    PanoramaNode* scroller = scrollbar.parent;
    if (scroller == nullptr || scrollbar.children.empty())
    {
        return;
    }
    const auto axis_it = scrollbar.attributes.find("__scrollbar");
    const bool vertical = axis_it == scrollbar.attributes.end() || axis_it->second != "horizontal";
    const PanoramaNode& thumb = *scrollbar.children.front();

    const float track_pos = vertical ? scrollbar.layout.content_y : scrollbar.layout.content_x;
    const float thumb_pos = (vertical ? thumb.layout.y : thumb.layout.x) - track_pos;
    const float thumb_len = vertical ? thumb.layout.height : thumb.layout.width;
    const float pointer = vertical ? design_y : design_x;

    // Grabbing the thumb keeps the pointer anchored where it landed on it; a
    // track press centres the thumb on the pointer and drags from there.
    const bool on_thumb = pointer >= track_pos + thumb_pos && pointer < track_pos + thumb_pos + thumb_len;
    scrollbar_drag_scroller_ = scroller;
    scrollbar_drag_bar_ = &scrollbar;
    scrollbar_drag_vertical_ = vertical;
    scrollbar_drag_grab_ = on_thumb ? pointer - (track_pos + thumb_pos) : thumb_len * 0.5F;

    // Valve's styles target both `:active` (free via the hover chain) and an
    // explicit `.MouseDown` class while the bar is held.
    if (!scrollbar.has_class("MouseDown"))
    {
        scrollbar.classes.emplace_back("MouseDown");
        scrollbar.mark_style_dirty();
    }
    update_scrollbar_drag(design_x, design_y);
}

bool PanoramaInputController::update_scrollbar_drag(float design_x, float design_y)
{
    PanoramaNode* scroller = scrollbar_drag_scroller_;
    PanoramaNode* bar = scrollbar_drag_bar_;
    if (scroller == nullptr || bar == nullptr || bar->children.empty())
    {
        return false;
    }
    const bool vertical = scrollbar_drag_vertical_;
    const PanoramaNode& thumb = *bar->children.front();
    const float track_pos = vertical ? bar->layout.content_y : bar->layout.content_x;
    const float track_len = vertical ? bar->layout.content_height : bar->layout.content_width;
    const float thumb_len = vertical ? thumb.layout.height : thumb.layout.width;
    const float range = track_len - thumb_len;
    const float max_scroll = vertical ? scroller->max_scroll_y : scroller->max_scroll_x;
    if (range <= 0.0F || max_scroll <= 0.0F)
    {
        return false;
    }
    // WebCore ScrollbarThemeComposite::thumbPosition inverted: offset =
    // thumbPos * scrollableSize / (trackLen - thumbLen).
    const float pointer = vertical ? design_y : design_x;
    const float thumb_pos = std::clamp(pointer - track_pos - scrollbar_drag_grab_, 0.0F, range);
    const float offset = thumb_pos * max_scroll / range;
    // Direct manipulation: the thumb tracks the pointer exactly, and a live
    // smooth-scroll spring must not fight it.
    panorama_cancel_scroll_animation(*scroller);
    return panorama_set_scroll_offset(*scroller,
        vertical ? scroller->scroll_offset_x : offset,
        vertical ? offset : scroller->scroll_offset_y);
}

void PanoramaInputController::end_scrollbar_drag()
{
    if (scrollbar_drag_bar_ != nullptr && scrollbar_drag_bar_->has_class("MouseDown"))
    {
        auto& classes = scrollbar_drag_bar_->classes;
        classes.erase(std::remove(classes.begin(), classes.end(), "MouseDown"), classes.end());
        scrollbar_drag_bar_->mark_style_dirty();
    }
    scrollbar_drag_scroller_ = nullptr;
    scrollbar_drag_bar_ = nullptr;
    scrollbar_drag_grab_ = 0.0F;
}

void PanoramaInputController::begin_slider_drag(PanoramaNode& slider, float design_x, float design_y)
{
    slider_drag_node_ = &slider;
    update_slider_drag(design_x, design_y);
}

bool PanoramaInputController::update_slider_drag(float design_x, float /*design_y*/)
{
    PanoramaNode* slider = slider_drag_node_;
    if (slider == nullptr)
    {
        return false;
    }
    PanoramaNode* track = nullptr;
    for (const auto& child : slider->children)
    {
        if (child->id == "SliderTrack")
        {
            track = child.get();
            break;
        }
    }
    if (track == nullptr)
    {
        return false;
    }
    float thumb_width = 0.0F;
    for (const auto& child : track->children)
    {
        if (child->id == "SliderThumb")
        {
            thumb_width = child->layout.width;
            break;
        }
    }
    const float track_x = track->layout.content_x;
    const float travel = track->layout.content_width - thumb_width;
    if (travel <= 0.0F)
    {
        return false;
    }
    // WebKit SliderThumbElement::setPositionFromPoint: the pointer maps to the
    // thumb CENTRE, clamped to the rail, then to a 0..1 fraction of the travel.
    const float position = std::clamp(design_x - track_x - thumb_width * 0.5F, 0.0F, travel);
    return panorama_set_slider_fraction(*slider, position / travel);
}

std::vector<PanoramaNode*> panorama_collect_tab_order(PanoramaNode& root)
{
    std::vector<PanoramaNode*> candidates;
    collect_focus_candidates(root, candidates);

    // FocusController::advanceFocusInDocumentOrder: positive tabindex values are
    // visited first in ascending order (stable for equal values -> document
    // order), then the run of tabindex 0 / default-focusable controls in document
    // order. (Negative tabindex was already filtered by the collector.)
    std::vector<PanoramaNode*> positive;
    std::vector<PanoramaNode*> zero;
    for (PanoramaNode* node : candidates)
    {
        if (node_tab_index(*node) > 0)
        {
            positive.push_back(node);
        }
        else
        {
            zero.push_back(node);
        }
    }
    std::stable_sort(positive.begin(), positive.end(),
        [](PanoramaNode* a, PanoramaNode* b) { return node_tab_index(*a) < node_tab_index(*b); });
    positive.insert(positive.end(), zero.begin(), zero.end());
    return positive;
}

void PanoramaInputController::set_focus(PanoramaNode& root, PanoramaNode* node, PanoramaRuntime* runtime)
{
    if (node == focus_node_)
    {
        return;
    }
    PanoramaNode* old = focus_node_;
    if (old != nullptr)
    {
        old->focused = false;
    }
    focus_node_ = node;
    if (node != nullptr)
    {
        node->focused = true;
        // A freshly focused field places a collapsed caret at the end of its value
        // (its selection is clamped onto codepoint boundaries first).
        if (panorama_node_is_text_entry(*node))
        {
            panorama_text_entry_clamp_selection(*node);
            panorama_text_entry_collapse_to_end(*node);
        }
    }
    // :focus / :focus-within can alter matches anywhere in the tree; full invalidate.
    root.mark_style_dirty();

    // WebCore setFocusedElement order: blur the previously focused element, then
    // focus the new one. Guard against a handler deleting the new node mid-call
    // (node destruction nulls focus_node_, so re-check identity before onfocus).
    if (runtime != nullptr)
    {
        if (old != nullptr)
        {
            runtime->run_node_handler(*old, "onblur");
        }
        if (node != nullptr && focus_node_ == node)
        {
            runtime->run_node_handler(*node, "onfocus");
        }
    }
}

bool PanoramaInputController::advance_focus(PanoramaNode& root, bool forward, PanoramaRuntime* runtime)
{
    std::vector<PanoramaNode*> order = panorama_collect_tab_order(root);
    if (order.empty())
    {
        return false;
    }
    int current = -1;
    for (int i = 0; i < static_cast<int>(order.size()); ++i)
    {
        if (order[static_cast<std::size_t>(i)] == focus_node_)
        {
            current = i;
            break;
        }
    }
    const int count = static_cast<int>(order.size());
    int next = 0;
    if (current < 0)
    {
        next = forward ? 0 : count - 1;
    }
    else
    {
        next = current + (forward ? 1 : -1);
        if (next < 0)
        {
            next = count - 1; // wrap to the end
        }
        else if (next >= count)
        {
            next = 0; // wrap to the start
        }
    }
    PanoramaNode* target = order[static_cast<std::size_t>(next)];
    if (target == focus_node_)
    {
        return false; // only one candidate, already focused
    }
    set_focus(root, target, runtime);
    // Windows convention (and CS:GO's): tabbing into a text field selects all of
    // its current contents so the next keystroke replaces them.
    if (focus_node_ == target && panorama_node_is_text_entry(*target))
    {
        panorama_text_entry_select_all(*target);
    }
    return true;
}

bool PanoramaInputController::handle_text_input(PanoramaNode& root, std::string_view utf8, PanoramaRuntime* runtime)
{
    (void)root;
    if (focus_node_ == nullptr || !panorama_node_is_text_entry(*focus_node_) || utf8.empty())
    {
        return false;
    }
    PanoramaNode* field = focus_node_;
    if (!panorama_text_entry_insert(*field, utf8))
    {
        return false;
    }
    sync_panorama_text_entry_input_class(*field);
    field->mark_style_dirty();
    if (runtime != nullptr)
    {
        // Valve's shipped Panorama layouts use `ontextentrychange` (for example
        // context_menu_inventory_search.xml). Keep the older engine-specific
        // `ontextentrychanged` spelling as a compatibility alias. The first
        // handler may delete this TextEntry or an ancestor, so watch it before
        // invoking the alias.
        PanoramaScopedNodeWatch watch({field});
        runtime->run_node_handler(*field, "ontextentrychange");
        if (PanoramaNode* live_field = watch.nodes()[0])
        {
            runtime->run_node_handler(*live_field, "ontextentrychanged");
        }
    }
    return true;
}

bool PanoramaInputController::handle_key_down(PanoramaNode& root, const PanoramaKeyEvent& event, PanoramaRuntime* runtime)
{
    // Tab advances focus (EventHandler::defaultTabEventHandler — ignored when a
    // Ctrl/Alt modifier is held, which is a different shortcut).
    if (event.key == PanoramaKey::Tab && !event.ctrl && !event.alt)
    {
        return advance_focus(root, !event.shift, runtime);
    }

    PanoramaNode* field = focus_node_;
    if (field == nullptr)
    {
        return false;
    }

    if (panorama_node_is_text_entry(*field))
    {
        // Ctrl turns the arrow/delete keys into their word-granularity variants
        // (MoveWordLeft, DeleteWordForward, ...).
        const PanoramaTextGranularity granularity =
            event.ctrl ? PanoramaTextGranularity::Word : PanoramaTextGranularity::Character;
        bool selection_changed = false; // caret/selection moved -> repaint only
        bool value_changed = false;     // text edited -> relayout + text-entry change handlers
        switch (event.key)
        {
        case PanoramaKey::ArrowLeft:
            selection_changed = panorama_text_entry_move(*field, PanoramaTextDirection::Backward, granularity, event.shift);
            break;
        case PanoramaKey::ArrowRight:
            selection_changed = panorama_text_entry_move(*field, PanoramaTextDirection::Forward, granularity, event.shift);
            break;
        case PanoramaKey::Home:
            selection_changed =
                panorama_text_entry_move(*field, PanoramaTextDirection::Backward, PanoramaTextGranularity::LineBoundary, event.shift);
            break;
        case PanoramaKey::End:
            selection_changed =
                panorama_text_entry_move(*field, PanoramaTextDirection::Forward, PanoramaTextGranularity::LineBoundary, event.shift);
            break;
        case PanoramaKey::Backspace:
            value_changed = panorama_text_entry_delete(*field, PanoramaTextDirection::Backward, granularity);
            break;
        case PanoramaKey::Delete:
            value_changed = panorama_text_entry_delete(*field, PanoramaTextDirection::Forward, granularity);
            break;
        case PanoramaKey::A:
            if (!event.ctrl)
            {
                return false; // a plain 'a' is text — delivered via handle_text_input
            }
            selection_changed = panorama_text_entry_select_all(*field);
            break;
        case PanoramaKey::Enter:
            // Single-line: Enter never inserts a newline; it submits the field.
            if (runtime != nullptr)
            {
                runtime->run_node_handler(*field, "oninputsubmit");
            }
            return true;
        default:
            return false; // unhandled key (e.g. Up/Down) falls through to the host
        }

        if (value_changed)
        {
            sync_panorama_text_entry_input_class(*field);
            field->mark_style_dirty();
            if (runtime != nullptr)
            {
                PanoramaScopedNodeWatch watch({field});
                runtime->run_node_handler(*field, "ontextentrychange");
                if (PanoramaNode* live_field = watch.nodes()[0])
                {
                    runtime->run_node_handler(*live_field, "ontextentrychanged");
                }
            }
            return true;
        }
        if (selection_changed)
        {
            field->mark_style_dirty();
        }
        return true; // caret/selection keys are consumed by the focused field
    }

    // A focused non-text control activates on Enter (matching the press path's
    // onactivate), so keyboard-only users can trigger buttons.
    if (event.key == PanoramaKey::Enter && runtime != nullptr)
    {
        runtime->run_node_handler(*field, "onactivate");
        return true;
    }
    return false;
}

void PanoramaInputController::reset()
{
    hover_node_ = nullptr;
    focus_node_ = nullptr;
    pointer_down_ = false;
    scrollbar_drag_scroller_ = nullptr;
    scrollbar_drag_bar_ = nullptr;
    scrollbar_drag_grab_ = 0.0F;
    slider_drag_node_ = nullptr;
}

PanoramaInputController::PanoramaInputController()
{
    panorama_add_node_lifetime_observer(*this);
}

PanoramaInputController::~PanoramaInputController()
{
    panorama_remove_node_lifetime_observer(*this);
}

void PanoramaInputController::on_panorama_node_destroyed(PanoramaNode& node)
{
    // Identity comparisons only — the node is mid-destruction. An ancestor's
    // destruction reaches us through each descendant's own notification, so
    // pointer equality is sufficient.
    if (hover_node_ == &node)
    {
        hover_node_ = nullptr;
    }
    if (focus_node_ == &node)
    {
        focus_node_ = nullptr;
    }
    if (scrollbar_drag_scroller_ == &node || scrollbar_drag_bar_ == &node)
    {
        scrollbar_drag_scroller_ = nullptr;
        scrollbar_drag_bar_ = nullptr;
        scrollbar_drag_grab_ = 0.0F;
    }
    if (slider_drag_node_ == &node)
    {
        slider_drag_node_ = nullptr;
    }
}
}
