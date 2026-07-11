#include "ui/panorama/panorama_layout.hpp"

#include "ui/panorama/panorama_text_break.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace panorama
{
namespace
{
constexpr float kInf = std::numeric_limits<float>::infinity();

bool is_horizontal(PanoramaFlow flow)
{
    return flow == PanoramaFlow::Right || flow == PanoramaFlow::Left || flow == PanoramaFlow::RightWrap;
}

bool is_vertical(PanoramaFlow flow)
{
    return flow == PanoramaFlow::Down || flow == PanoramaFlow::Up || flow == PanoramaFlow::Down_Wrap;
}

bool is_wrap_flow(PanoramaFlow flow)
{
    return flow == PanoramaFlow::RightWrap || flow == PanoramaFlow::Down_Wrap;
}

const PanoramaNode* open_dropdown_header_child(const PanoramaNode& node)
{
    if (!panorama_node_collapses_to_selected_child(node) || !panorama_dropdown_is_open(node))
    {
        return nullptr;
    }
    return panorama_dropdown_selected_child(node);
}

bool participates_in_parent_flow(const PanoramaNode& child, const PanoramaNode* open_dropdown_header)
{
    if (panorama_node_is_scrollbar(child))
    {
        // Synthesized scrollbar chrome overlays the viewport; it neither flows
        // nor contributes intrinsic/fit extents. layout_scrollbars places it.
        return false;
    }
    return open_dropdown_header == nullptr || &child == open_dropdown_header;
}

float horizontal_padding(const PanoramaComputedStyle& s)
{
    return s.padding.left + s.padding.right + s.border_left() + s.border_right();
}

float vertical_padding(const PanoramaComputedStyle& s)
{
    return s.padding.top + s.padding.bottom + s.border_top() + s.border_bottom();
}

// ---- intrinsic (min-content) pass -------------------------------------------

// Panorama's default (unset) size: hug the content (fit-children), like real
// Panorama lays out plain wrapper panels — e.g. playercard.xml's JsPlayerCardAvatar
// wrapper inside the flow-right player-card row must hug the 96px avatar, and the
// un-styled `.full-width` section wrappers must hug their rows so the playercard's
// fit-children height chain doesn't collapse to 0.
//
// Exception: fit-children is degenerate on an axis where NO child contributes an
// intrinsic size (all fill/percent — e.g. JsNewsPanel wrapping a `width:100%;
// height:fill-parent-flow(1)` news panel would collapse to its navbar text).
// Such wrappers fall back to fill on that axis, which is also where real Panorama
// ends up: the child spans the wrapper, the wrapper its parent. A percent child
// does NOT force fill when an intrinsic sibling exists — it resolves against the
// fit size the siblings determine (playercard's `height:100%` bg panel stretches
// over the fit-sized sections wrapper).
void apply_panorama_size_defaults(PanoramaNode& node)
{
    // Lazily computed and cached: panorama_node_defaults_to_content_size() walks
    // up to ~23 string_view tag comparisons, but its result is only read inside
    // the Auto-guarded branches below. Once a node's width/height are already
    // resolved (the common case on repeat layout passes), the value is dead and
    // must not be computed at all.
    bool content_sized_value = false;
    bool content_sized_known = false;
    const auto content_sized = [&node, &content_sized_value, &content_sized_known]() {
        if (!content_sized_known)
        {
            content_sized_value = panorama_node_defaults_to_content_size(node);
            content_sized_known = true;
        }
        return content_sized_value;
    };
    const auto fit_degenerate = [&node](auto&& axis_length) {
        bool any_intrinsic = false;
        for (const auto& child : node.children)
        {
            const PanoramaLengthType type = axis_length(child->computed).type;
            any_intrinsic = any_intrinsic ||
                (type != PanoramaLengthType::Percent && type != PanoramaLengthType::FillParentFlow);
        }
        return !node.children.empty() && !any_intrinsic;
    };
    if (node.computed.width.type == PanoramaLengthType::Auto)
    {
        // A text leaf normally hugs its content (fit-children). But `text-align`
        // centre/right only has room to act when the box is wider than the text, so
        // a width-less text node with a non-left alignment fills its parent's flow
        // width instead — matching how width-less centred labels render in Panorama.
        //
        // That only applies when width is the CROSS axis (vertical-flow or
        // flow:none parents): the label has no sibling contending for it, so
        // filling the parent's width is harmless and lets alignment act. When
        // width is instead the parent's MAIN (packing) axis — a horizontal-flow
        // parent — FillParentFlow would consume the row's entire leftover flow
        // space instead of hugging the glyph width, shoving every subsequent
        // sibling toward the row's far edge (e.g. `.weapon-row-number` inside
        // `.weapon-row-horiz-container`). Such labels fall through to the
        // normal fit-children sizing below.
        const bool parent_is_horizontal_flow =
            node.parent != nullptr && is_horizontal(node.parent->computed.flow);
        const bool aligned_text = !node.text.empty() &&
            node.computed.text_align != PanoramaHAlign::Left && !parent_is_horizontal_flow;
        const bool fit_width = content_sized() ||
            !fit_degenerate([](const PanoramaComputedStyle& s) { return s.width; });
        node.computed.width = (fit_width && !aligned_text)
            ? PanoramaLength{PanoramaLengthType::FitChildren, 0.0F}
            : PanoramaLength{PanoramaLengthType::FillParentFlow, 1.0F};
    }
    if (node.computed.height.type == PanoramaLengthType::Auto)
    {
        const bool fit_height = content_sized() ||
            !fit_degenerate([](const PanoramaComputedStyle& s) { return s.height; });
        node.computed.height = fit_height
            ? PanoramaLength{PanoramaLengthType::FitChildren, 0.0F}
            : PanoramaLength{PanoramaLengthType::FillParentFlow, 1.0F};
    }
    for (const auto& child : node.children)
    {
        apply_panorama_size_defaults(*child);
    }
}

float intrinsic_axis(PanoramaLengthType type, float pixels, float fit_value)
{
    switch (type)
    {
    case PanoramaLengthType::Pixels:
        return pixels;
    case PanoramaLengthType::Auto:
    case PanoramaLengthType::FitChildren:
        return fit_value;
    case PanoramaLengthType::Percent:
    case PanoramaLengthType::WidthPercent:
    case PanoramaLengthType::HeightPercent:
        // Percent sizes inside a fit-children ancestor are indefinite. Treat
        // them like auto for intrinsic measurement so containers with children
        // such as the Play menu's full-height map-tile internals do not collapse.
        return fit_value;
    case PanoramaLengthType::FillParentFlow:
    default:
        // Flow sizes have no intrinsic contribution; they resolve against a
        // definite parent in the top-down pass.
        return 0.0F;
    }
}

void measure_intrinsic(PanoramaNode& node, const PanoramaTextMeasure& tm)
{
    const PanoramaComputedStyle& s = node.computed;

    float content_w = 0.0F;
    float content_h = 0.0F;
    float text_w = 0.0F;

    node.intrinsic_text_width = 0.0F;
    node.intrinsic_text_height = 0.0F;
    if (!node.text.empty() && tm.measure && panorama_node_paints_own_text(node))
    {
        std::string transformed_storage;
        const std::string_view display = panorama_transform_text_view(node.text, s.text_transform, transformed_storage);
        if (node.is_html_text())
        {
            // html="true" labels carry inline <b>/<i> markup: measure each styled run
            // at its own weight and sum the advances so bold spans size correctly. The
            // tag characters themselves are excluded (they are not part of any run).
            float tw = 0.0F;
            float th = 0.0F;
            for (const PanoramaTextRun& run : panorama_parse_inline_markup(display))
            {
                const int weight = panorama_run_font_weight(s.font_weight, run.bold);
                const auto [rw, rh] = tm.measure(run.text, s.font_size, weight, s.letter_spacing);
                tw += rw;
                th = std::max(th, rh);
            }
            content_w = std::max(content_w, tw);
            content_h = std::max(content_h, s.line_height > 0.0F ? s.line_height : th);
            text_w = tw;
            node.intrinsic_text_width = tw;
            node.intrinsic_text_height = th;
        }
        else
        {
            const auto [tw, th] = tm.measure(display, s.font_size, s.font_weight, s.letter_spacing);
            content_w = std::max(content_w, tw);
            content_h = std::max(content_h, s.line_height > 0.0F ? s.line_height : th);
            text_w = tw;
            node.intrinsic_text_width = tw;
            node.intrinsic_text_height = th;
        }
    }

    float sum_w = 0.0F;
    float max_w = 0.0F;
    float sum_h = 0.0F;
    float max_h = 0.0F;
    const bool children_flow = is_horizontal(s.flow) || is_vertical(s.flow);
    const PanoramaNode* open_dropdown_header = open_dropdown_header_child(node);
    for (const auto& child : node.children)
    {
        measure_intrinsic(*child, tm);
        const PanoramaComputedStyle& cs = child->computed;
        if (!cs.visible || !participates_in_parent_flow(*child, open_dropdown_header))
        {
            continue;
        }
        float mbw = child->intrinsic_width + cs.margin.left + cs.margin.right;
        float mbh = child->intrinsic_height + cs.margin.top + cs.margin.bottom;
        // In a flow:none container, `position` offsets apply (they are ignored
        // by flowing parents) and extend the fit-children extent: the HUD's
        // .hud-HA-health places its ProgressBar at x:110px, so the section must
        // measure 110px + the bar, or the sibling armor block — positioned by
        // the parent's flow from this PROVISIONAL size — overlaps it.
        if (!children_flow && cs.has_position)
        {
            if (!cs.pos_x_percent && cs.pos_x > 0.0F)
            {
                mbw += cs.pos_x;
            }
            if (!cs.pos_y_percent && cs.pos_y > 0.0F)
            {
                mbh += cs.pos_y;
            }
        }
        sum_w += mbw;
        sum_h += mbh;
        max_w = std::max(max_w, mbw);
        max_h = std::max(max_h, mbh);
    }

    float children_w = max_w;
    float children_h = max_h;
    if (is_horizontal(s.flow))
    {
        children_w = sum_w;
        children_h = max_h;
    }
    else if (is_vertical(s.flow))
    {
        children_w = max_w;
        children_h = sum_h;
    }
    content_w = std::max(content_w, children_w);
    content_h = std::max(content_h, children_h);

    node.intrinsic_width = intrinsic_axis(s.width.type, s.width.value, content_w + horizontal_padding(s));
    node.intrinsic_height = intrinsic_axis(s.height.type, s.height.value, content_h + vertical_padding(s));

    // A text node that fills its flow (e.g. the width default for centred
    // labels, so text-align has room to act) still contributes its TEXT width
    // to fit-children ancestors — otherwise a fit-sized button collapses to its
    // padding and the label's fill share resolves to 0 (the GO button's label).
    if (text_w > 0.0F && s.width.type == PanoramaLengthType::FillParentFlow)
    {
        node.intrinsic_width = std::max(node.intrinsic_width, text_w + horizontal_padding(s));
    }

    // A definite pixel minimum reserves space in fit-children ancestors.
    if (s.min_width.type == PanoramaLengthType::Pixels)
    {
        node.intrinsic_width = std::max(node.intrinsic_width, s.min_width.value);
    }
    if (s.min_height.type == PanoramaLengthType::Pixels)
    {
        node.intrinsic_height = std::max(node.intrinsic_height, s.min_height.value);
    }
}

// ---- top-down resolve pass --------------------------------------------------

// The cross-axis units reference the element's OWN other axis (Panorama's
// square-icon idiom: `width: height-percentage(100%); height: 32px;`), not the
// parent's. They resolve in provisional_width/height below, where the other
// axis is computable; in these generic helpers (also used for min/max sizes,
// where the CS:GO stylesheet corpus never uses cross-axis units) they take the
// fallback.
float resolve_width_value(const PanoramaLength& len, float cw, float /*ch*/, float fallback)
{
    switch (len.type)
    {
    case PanoramaLengthType::Pixels:
        return len.value;
    case PanoramaLengthType::Percent:
        return cw * len.value / 100.0F;
    default:
        return fallback;
    }
}

float resolve_height_value(const PanoramaLength& len, float /*cw*/, float ch, float fallback)
{
    switch (len.type)
    {
    case PanoramaLengthType::Pixels:
        return len.value;
    case PanoramaLengthType::Percent:
        return ch * len.value / 100.0F;
    default:
        return fallback;
    }
}

float clamp_width(const PanoramaComputedStyle& s, float value, float cw, float ch)
{
    const float lo = resolve_width_value(s.min_width, cw, ch, 0.0F);
    const float hi = resolve_width_value(s.max_width, cw, ch, kInf);
    return std::clamp(value, lo, std::max(lo, hi));
}

float clamp_height(const PanoramaComputedStyle& s, float value, float cw, float ch)
{
    const float lo = resolve_height_value(s.min_height, cw, ch, 0.0F);
    const float hi = resolve_height_value(s.max_height, cw, ch, kInf);
    return std::clamp(value, lo, std::max(lo, hi));
}

float provisional_width(const PanoramaNode& child, float cw, float ch)
{
    const PanoramaComputedStyle& s = child.computed;
    float base;
    if (s.width.type == PanoramaLengthType::HeightPercent)
    {
        // width: height-percentage(P) = P% of the element's own resolved
        // height. A mutual cross-reference (height: width-percentage too) is
        // degenerate; fall back to the intrinsic height for the base.
        const float own_height = s.height.type == PanoramaLengthType::WidthPercent
            ? child.intrinsic_height
            : clamp_height(s, resolve_height_value(s.height, cw, ch, child.intrinsic_height), cw, ch);
        base = own_height * s.width.value / 100.0F;
    }
    else
    {
        base = resolve_width_value(s.width, cw, ch, child.intrinsic_width);
    }
    if (s.width.type == PanoramaLengthType::FitChildren && is_wrap_flow(s.flow) && cw > 0.0F)
    {
        base = std::min(base, std::max(0.0F, cw - s.margin.left - s.margin.right));
    }
    return clamp_width(s, base, cw, ch);
}

float provisional_height(const PanoramaNode& child, float cw, float ch)
{
    const PanoramaComputedStyle& s = child.computed;
    float base;
    if (s.height.type == PanoramaLengthType::WidthPercent)
    {
        // height: width-percentage(P) = P% of the element's own resolved width.
        const float own_width = s.width.type == PanoramaLengthType::HeightPercent
            ? child.intrinsic_width
            : clamp_width(s, resolve_width_value(s.width, cw, ch, child.intrinsic_width), cw, ch);
        base = own_width * s.height.value / 100.0F;
    }
    else
    {
        base = resolve_height_value(s.height, cw, ch, child.intrinsic_height);
    }
    if (s.height.type == PanoramaLengthType::FitChildren && is_wrap_flow(s.flow) && ch > 0.0F)
    {
        base = std::min(base, std::max(0.0F, ch - s.margin.top - s.margin.bottom));
    }
    return clamp_height(s, base, cw, ch);
}

float halign_offset(PanoramaHAlign align, float available)
{
    switch (align)
    {
    case PanoramaHAlign::Center:
        return available * 0.5F;
    case PanoramaHAlign::Right:
        return available;
    case PanoramaHAlign::Left:
    default:
        return 0.0F;
    }
}

// Offset of a child's leading edge from the container's content origin along one
// axis, combining alignment with the child's margins. Panorama treats alignment
// and margin as INDEPENDENT: the child's border box is aligned within the full
// container extent, then its leading margin translates it. A negative leading
// margin on a centered element therefore shifts it by the FULL margin (this is how
// CS:GO's .team-logo, `vertical-align: center; margin-top: -150px`, pulls itself up
// into the buy-wheel hub). Folding the margins into the centering space instead
// (container - m_lead - m_trail - size) would only apply half of a negative margin,
// leaving such icons off-centre. Top/Left and Bottom/Right are unchanged from that
// older model; only the centered case differs, and only when a margin is non-zero.
float aligned_leading_offset(int align_center, int align_end, float container_extent, float size, float m_lead,
    float m_trail)
{
    if (align_center)
    {
        return (container_extent - size) * 0.5F + m_lead;
    }
    if (align_end)
    {
        return container_extent - size - m_trail;
    }
    return m_lead;
}

float aligned_offset_h(PanoramaHAlign align, float container_extent, float size, float m_lead, float m_trail)
{
    return aligned_leading_offset(align == PanoramaHAlign::Center, align == PanoramaHAlign::Right, container_extent,
        size, m_lead, m_trail);
}

float aligned_offset_v(PanoramaVAlign align, float container_extent, float size, float m_lead, float m_trail)
{
    return aligned_leading_offset(align == PanoramaVAlign::Middle, align == PanoramaVAlign::Bottom, container_extent,
        size, m_lead, m_trail);
}

float valign_offset(PanoramaVAlign align, float available)
{
    switch (align)
    {
    case PanoramaVAlign::Middle:
        return available * 0.5F;
    case PanoramaVAlign::Bottom:
        return available;
    case PanoramaVAlign::Top:
    default:
        return 0.0F;
    }
}

void resolve_node(PanoramaNode& node, const PanoramaTextMeasure& tm);

float margin_box_width(const PanoramaNode& node)
{
    const PanoramaComputedStyle& s = node.computed;
    return s.margin.left + node.layout.width + s.margin.right;
}

float margin_box_height(const PanoramaNode& node)
{
    const PanoramaComputedStyle& s = node.computed;
    return s.margin.top + node.layout.height + s.margin.bottom;
}

// Moves an already-resolved subtree (layout boxes are absolute coordinates).
void offset_subtree(PanoramaNode& node, float dx, float dy)
{
    node.layout.x += dx;
    node.layout.y += dy;
    node.layout.content_x += dx;
    node.layout.content_y += dy;
    if (node.has_popup_layout)
    {
        node.popup_layout.x += dx;
        node.popup_layout.y += dy;
        node.popup_layout.content_x += dx;
        node.popup_layout.content_y += dy;
    }
    for (const auto& child : node.children)
    {
        offset_subtree(*child, dx, dy);
    }
}

void apply_position_offset(PanoramaNode& child, float parent_width, float parent_height)
{
    const PanoramaComputedStyle& cs = child.computed;
    if (!cs.has_position)
    {
        return;
    }

    child.layout.x += cs.pos_x_percent ? cs.pos_x / 100.0F * parent_width : cs.pos_x;
    child.layout.y += cs.pos_y_percent ? cs.pos_y / 100.0F * parent_height : cs.pos_y;
}

// `in_parent_flow` — Panorama's `position` property only takes effect in
// non-flowing layouts; a child laid out by a flowing parent ignores it. CS:GO
// proof: `ToggleButton Label { position: 42px 0px 0px; }` (the checkbox-text
// offset for default flow-none toggles) is live engine-wide, yet map-tile
// names — Labels inside a flow-children:up info block of a ToggleButton tile —
// render centred, not shifted 42px.
void resolve_positioned_child(
    PanoramaNode& child, float parent_width, float parent_height, const PanoramaTextMeasure& tm,
    bool in_parent_flow)
{
    if (!in_parent_flow)
    {
        apply_position_offset(child, parent_width, parent_height);
    }
    resolve_node(child, tm);
}

void refresh_content_box(PanoramaLayoutBox& box, const PanoramaComputedStyle& s)
{
    box.content_x = box.x + s.padding.left + s.border_left();
    box.content_y = box.y + s.padding.top + s.border_top();
    box.content_width = std::max(0.0F, box.width - horizontal_padding(s));
    box.content_height = std::max(0.0F, box.height - vertical_padding(s));
}

void refresh_content_box(PanoramaNode& node)
{
    refresh_content_box(node.layout, node.computed);
}

void finalize_fit_children_size(PanoramaNode& node, float parent_width, float parent_height)
{
    const PanoramaComputedStyle& s = node.computed;
    if (s.width.type != PanoramaLengthType::FitChildren && s.height.type != PanoramaLengthType::FitChildren)
    {
        return;
    }

    float content_right = node.layout.content_x;
    float content_bottom = node.layout.content_y;
    bool any_visible_child = false;
    const PanoramaNode* open_dropdown_header = open_dropdown_header_child(node);
    for (const auto& child_owner : node.children)
    {
        const PanoramaNode& child = *child_owner;
        const PanoramaComputedStyle& cs = child.computed;
        if (!cs.visible || !participates_in_parent_flow(child, open_dropdown_header))
        {
            continue;
        }
        any_visible_child = true;
        content_right = std::max(content_right, child.layout.x + child.layout.width + cs.margin.right);
        content_bottom = std::max(content_bottom, child.layout.y + child.layout.height + cs.margin.bottom);
    }
    if (!any_visible_child)
    {
        return;
    }

    if (s.width.type == PanoramaLengthType::FitChildren)
    {
        const float children_width = any_visible_child ? std::max(0.0F, content_right - node.layout.content_x) : 0.0F;
        node.layout.width = clamp_width(s, children_width + horizontal_padding(s), parent_width, parent_height);
    }
    if (s.height.type == PanoramaLengthType::FitChildren)
    {
        const float children_height = any_visible_child ? std::max(0.0F, content_bottom - node.layout.content_y) : 0.0F;
        node.layout.height = clamp_height(s, children_height + vertical_padding(s), parent_width, parent_height);
    }
    refresh_content_box(node);
}

void position_open_dropdown_popup(PanoramaNode& node, const PanoramaTextMeasure& tm)
{
    const PanoramaNode* open_dropdown_header = open_dropdown_header_child(node);
    if (open_dropdown_header == nullptr)
    {
        return;
    }

    float popup_width = node.layout.width;
    for (const auto& child_owner : node.children)
    {
        const PanoramaNode& child = *child_owner;
        if (!child.computed.visible)
        {
            continue;
        }
        popup_width = std::max(popup_width, margin_box_width(child));
    }

    float y = node.layout.y + node.layout.height;
    float popup_height = 0.0F;
    for (const auto& child_owner : node.children)
    {
        PanoramaNode& child = *child_owner;
        const PanoramaComputedStyle& cs = child.computed;
        if (!cs.visible)
        {
            continue;
        }

        PanoramaLayoutBox popup_box = child.layout;
        popup_box.width = clamp_width(
            cs, std::max(child.layout.width, popup_width - cs.margin.left - cs.margin.right), popup_width, node.layout.height);
        popup_box.x = node.layout.x + cs.margin.left;
        popup_box.y = y + cs.margin.top;
        refresh_content_box(popup_box, cs);

        child.has_popup_layout = true;
        child.popup_layout = popup_box;
        if (!participates_in_parent_flow(child, open_dropdown_header))
        {
            child.layout = popup_box;
            resolve_positioned_child(child, popup_width, node.layout.height, tm, /*in_parent_flow=*/false);
            child.popup_layout = child.layout;
        }
        y += margin_box_height(child);
        popup_height += margin_box_height(child);
    }

    node.has_popup_layout = popup_height > 0.0F;
    node.popup_layout = {
        node.layout.x,
        node.layout.y + node.layout.height,
        popup_width,
        popup_height,
        node.layout.x,
        node.layout.y + node.layout.height,
        popup_width,
        popup_height,
    };
}

void position_right_wrap_line(
    const std::vector<PanoramaNode*>& line,
    float line_x,
    float line_y,
    float line_height,
    float parent_width,
    float parent_height,
    const PanoramaTextMeasure& tm)
{
    float x = line_x;
    for (PanoramaNode* child : line)
    {
        const PanoramaComputedStyle& cs = child->computed;
        const float w = child->layout.width;
        const float h = child->layout.height;
        child->layout.x = x + cs.margin.left;
        child->layout.y = line_y + aligned_offset_v(cs.valign, line_height, h, cs.margin.top, cs.margin.bottom);
        resolve_positioned_child(*child, parent_width, parent_height, tm, /*in_parent_flow=*/true);
        x += margin_box_width(*child);
    }
}

void position_down_wrap_column(
    const std::vector<PanoramaNode*>& column,
    float column_x,
    float column_y,
    float column_width,
    float parent_width,
    float parent_height,
    const PanoramaTextMeasure& tm)
{
    float y = column_y;
    for (PanoramaNode* child : column)
    {
        const PanoramaComputedStyle& cs = child->computed;
        const float w = child->layout.width;
        const float h = child->layout.height;
        child->layout.x = column_x + aligned_offset_h(cs.halign, column_width, w, cs.margin.left, cs.margin.right);
        child->layout.y = y + cs.margin.top;
        resolve_positioned_child(*child, parent_width, parent_height, tm, /*in_parent_flow=*/true);
        y += margin_box_height(*child);
    }
}

void position_right_wrap_children(PanoramaNode& node, const PanoramaTextMeasure& tm)
{
    const PanoramaLayoutBox& box = node.layout;
    const float limit_x = box.content_x + box.content_width;
    std::vector<PanoramaNode*> line;
    float cursor_x = box.content_x;
    float line_y = box.content_y;
    float line_height = 0.0F;

    const auto flush = [&]() {
        if (line.empty())
        {
            return;
        }
        position_right_wrap_line(line, box.content_x, line_y, line_height, box.content_width, box.content_height, tm);
        line_y += line_height;
        cursor_x = box.content_x;
        line_height = 0.0F;
        line.clear();
    };

    for (const auto& child_owner : node.children)
    {
        PanoramaNode& child = *child_owner;
        if (!child.computed.visible)
        {
            continue;
        }

        const float outer_w = margin_box_width(child);
        if (!line.empty() && cursor_x + outer_w > limit_x)
        {
            flush();
        }
        line.push_back(&child);
        cursor_x += outer_w;
        line_height = std::max(line_height, margin_box_height(child));
    }
    flush();
}

void position_down_wrap_children(PanoramaNode& node, const PanoramaTextMeasure& tm)
{
    const PanoramaLayoutBox& box = node.layout;
    const float limit_y = box.content_y + box.content_height;
    std::vector<PanoramaNode*> column;
    float column_x = box.content_x;
    float cursor_y = box.content_y;
    float column_width = 0.0F;

    const auto flush = [&]() {
        if (column.empty())
        {
            return;
        }
        position_down_wrap_column(column, column_x, box.content_y, column_width, box.content_width, box.content_height, tm);
        column_x += column_width;
        cursor_y = box.content_y;
        column_width = 0.0F;
        column.clear();
    };

    for (const auto& child_owner : node.children)
    {
        PanoramaNode& child = *child_owner;
        if (!child.computed.visible)
        {
            continue;
        }

        const float outer_h = margin_box_height(child);
        if (!column.empty() && cursor_y + outer_h > limit_y)
        {
            flush();
        }
        column.push_back(&child);
        cursor_y += outer_h;
        column_width = std::max(column_width, margin_box_width(child));
    }
    flush();
}

// ---- overflow: scroll --------------------------------------------------------

// Computes the node's scrollable extents (WebCore RenderLayerScrollableArea::
// computeScrollDimensions), clamps the persistent scroll offset to the new
// [0, max] range, and shifts the children by it — WebCore folds scrollPosition()
// into the layer positions the same way (RenderLayer::updateLayerPosition's
// `localPoint -= scrollPosition()`), so paint clipping and hit testing both see
// scrolled coordinates with no further work.
void apply_overflow_scroll(PanoramaNode& node)
{
    const bool scroll_x = panorama_node_scrolls_x(node);
    const bool scroll_y = panorama_node_scrolls_y(node);
    node.max_scroll_x = 0.0F;
    node.max_scroll_y = 0.0F;
    if (!scroll_x && !scroll_y)
    {
        node.scroll_offset_x = 0.0F;
        node.scroll_offset_y = 0.0F;
        return;
    }

    const PanoramaComputedStyle& s = node.computed;

    // The viewport is the overflow clip box — border box inset by the border —
    // matching both this engine's paint clip (overflow_content_clip) and
    // WebCore's clientWidth/clientHeight (the padding box).
    const float viewport_x = node.layout.x + std::max(0.0F, s.border_left());
    const float viewport_y = node.layout.y + std::max(0.0F, s.border_top());
    const float viewport_w =
        std::max(0.0F, node.layout.width - std::max(0.0F, s.border_left()) - std::max(0.0F, s.border_right()));
    const float viewport_h =
        std::max(0.0F, node.layout.height - std::max(0.0F, s.border_top()) - std::max(0.0F, s.border_bottom()));

    // Children extent at margin-box edges (the same measure finalize_fit_children_size
    // uses; child.x already contains margin.left).
    float extent_right = node.layout.content_x;
    float extent_bottom = node.layout.content_y;
    for (const auto& child : node.children)
    {
        const PanoramaComputedStyle& cs = child->computed;
        if (!cs.visible || panorama_node_is_scrollbar(*child))
        {
            continue;
        }
        extent_right = std::max(extent_right, child->layout.x + child->layout.width + cs.margin.right);
        extent_bottom = std::max(extent_bottom, child->layout.y + child->layout.height + cs.margin.bottom);
    }

    // WebCore RenderBox::scrollWidth/scrollHeight: max(client size, layout
    // overflow edge - border edge), where block flow appends the trailing
    // padding after the last in-flow child (the scrolled-to-bottom view keeps
    // the container's padding under the last row).
    const float scroll_w = std::max(viewport_w, extent_right + s.padding.right - viewport_x);
    const float scroll_h = std::max(viewport_h, extent_bottom + s.padding.bottom - viewport_y);

    // maximumScrollOffset = totalContentsSize - visibleSize; clamp the stored
    // offset into the new range (content/viewport changes shrink it).
    node.max_scroll_x = scroll_x ? scroll_w - viewport_w : 0.0F;
    node.max_scroll_y = scroll_y ? scroll_h - viewport_h : 0.0F;
    node.scroll_offset_x = std::clamp(node.scroll_offset_x, 0.0F, node.max_scroll_x);
    node.scroll_offset_y = std::clamp(node.scroll_offset_y, 0.0F, node.max_scroll_y);

    if (node.scroll_offset_x != 0.0F || node.scroll_offset_y != 0.0F)
    {
        for (const auto& child : node.children)
        {
            if (!child->computed.visible || panorama_node_is_scrollbar(*child))
            {
                continue;
            }
            offset_subtree(*child, -node.scroll_offset_x, -node.scroll_offset_y);
        }
    }
}

// Lays out the synthesized scrollbar chrome (ensure_panorama_scrollbar_internals)
// as a viewport overlay. The bar itself honors its CSS like a flow:none child of
// the viewport (Valve: `width: 8px; height: 100%; horizontal-align: right;`
// margins); the thumb follows WebCore ScrollbarThemeComposite:
//   thumbLength  = max(round(visible / total * trackLen), CSS min size), gone
//                  when it no longer fits the track;
//   thumbPosition = currentPos * (trackLen - thumbLen) / (total - visible).
// Overlay scrollbars only show when there is overflow (WebCore treats
// overflow:scroll like auto when scrollbars are overlay), so a non-scrollable
// axis zeroes its chrome out.
void layout_scrollbars(PanoramaNode& node)
{
    for (const auto& child : node.children)
    {
        if (!panorama_node_is_scrollbar(*child))
        {
            continue;
        }
        const auto axis_it = child->attributes.find("__scrollbar");
        const bool vertical = axis_it != child->attributes.end() && axis_it->second == "vertical";
        const float max_scroll = vertical ? node.max_scroll_y : node.max_scroll_x;
        PanoramaNode* thumb = child->children.empty() ? nullptr : child->children.front().get();
        if (max_scroll <= 0.0F)
        {
            child->layout = PanoramaLayoutBox{};
            if (thumb != nullptr)
            {
                thumb->layout = PanoramaLayoutBox{};
            }
            continue;
        }

        const PanoramaComputedStyle& s = node.computed;
        const float viewport_x = node.layout.x + std::max(0.0F, s.border_left());
        const float viewport_y = node.layout.y + std::max(0.0F, s.border_top());
        const float viewport_w =
            std::max(0.0F, node.layout.width - std::max(0.0F, s.border_left()) - std::max(0.0F, s.border_right()));
        const float viewport_h =
            std::max(0.0F, node.layout.height - std::max(0.0F, s.border_top()) - std::max(0.0F, s.border_bottom()));

        const PanoramaComputedStyle& bs = child->computed;
        child->layout.width = provisional_width(*child, viewport_w, viewport_h);
        child->layout.height = provisional_height(*child, viewport_w, viewport_h);
        const float avail_w = viewport_w - bs.margin.left - bs.margin.right - child->layout.width;
        const float avail_h = viewport_h - bs.margin.top - bs.margin.bottom - child->layout.height;
        child->layout.x = viewport_x + bs.margin.left + halign_offset(bs.halign, avail_w);
        child->layout.y = viewport_y + bs.margin.top + valign_offset(bs.valign, avail_h);
        refresh_content_box(*child);

        if (thumb == nullptr)
        {
            continue;
        }
        const PanoramaComputedStyle& ts = thumb->computed;
        const float track_x = child->layout.content_x;
        const float track_y = child->layout.content_y;
        const float track_w = child->layout.content_width;
        const float track_h = child->layout.content_height;
        const float track_len = vertical ? track_h : track_w;
        const float visible = vertical ? viewport_h : viewport_w;
        const float total = visible + max_scroll;
        const float current = vertical ? node.scroll_offset_y : node.scroll_offset_x;

        // proportion * trackLen, raised to the CSS minimum (Valve: min-height
        // 32px), gone once it exceeds the track.
        float thumb_len = total > 0.0F ? std::round(visible / total * track_len) : 0.0F;
        thumb_len = vertical ? clamp_height(ts, thumb_len, track_w, track_h)
                             : clamp_width(ts, thumb_len, track_w, track_h);
        if (thumb_len > track_len)
        {
            thumb_len = 0.0F;
        }
        const float scroll_range = total - visible;
        const float thumb_pos =
            scroll_range > 0.0F ? std::max(0.0F, current) * (track_len - thumb_len) / scroll_range : 0.0F;

        if (vertical)
        {
            thumb->layout.width = provisional_width(*thumb, track_w, track_h);
            thumb->layout.height = thumb_len;
            const float avail = track_w - ts.margin.left - ts.margin.right - thumb->layout.width;
            thumb->layout.x = track_x + ts.margin.left + halign_offset(ts.halign, avail);
            thumb->layout.y = track_y + thumb_pos;
        }
        else
        {
            thumb->layout.width = thumb_len;
            thumb->layout.height = provisional_height(*thumb, track_w, track_h);
            const float avail = track_h - ts.margin.top - ts.margin.bottom - thumb->layout.height;
            thumb->layout.x = track_x + thumb_pos;
            thumb->layout.y = track_y + ts.margin.top + valign_offset(ts.valign, avail);
        }
        refresh_content_box(*thumb);
    }
}

// Positions a `Slider`'s thumb + progress fill from its `value` fraction, after
// the normal flow pass has laid out the track and its children. Mirrors WebKit's
// RenderSliderContainer::layout: thumbOffset = fraction * (trackWidth - thumbWidth),
// so the thumb centre rides the value and the progress fills up to it. Horizontal
// only (the settings rows are all HorizontalSlider); a no-op for any other node.
void layout_sliders(PanoramaNode& node)
{
    if (!panorama_node_is_slider(node))
    {
        return;
    }
    PanoramaNode* track = nullptr;
    for (const auto& child : node.children)
    {
        if (child->id == "SliderTrack")
        {
            track = child.get();
            break;
        }
    }
    if (track == nullptr)
    {
        return;
    }
    PanoramaNode* progress = nullptr;
    PanoramaNode* thumb = nullptr;
    for (const auto& child : track->children)
    {
        if (child->id == "SliderTrackProgress")
        {
            progress = child.get();
        }
        else if (child->id == "SliderThumb")
        {
            thumb = child.get();
        }
    }
    if (thumb == nullptr)
    {
        return;
    }

    const float fraction = panorama_slider_fraction(node);
    const float track_x = track->layout.content_x;
    const float track_w = track->layout.content_width;
    const float track_h = track->layout.content_height;

    // The thumb is taller than the rail (10px vs 8px) and the rail's padding can
    // collapse its content box, so flow layout zeroes the thumb/progress. Size
    // them from their own computed style and centre them on the track's border
    // box vertically, the way layout_scrollbars sizes the scroll thumb.
    thumb->layout.width = provisional_width(*thumb, track_w, track_h);
    thumb->layout.height = provisional_height(*thumb, track_w, track_h);
    const float travel = std::max(0.0F, track_w - thumb->layout.width);
    thumb->layout.x = track_x + fraction * travel;
    thumb->layout.y = track->layout.y + (track->layout.height - thumb->layout.height) * 0.5F;
    refresh_content_box(*thumb);

    if (progress != nullptr)
    {
        progress->layout.width = std::max(0.0F, fraction * travel + thumb->layout.width * 0.5F);
        progress->layout.height = provisional_height(*progress, track_w, track_h);
        progress->layout.x = track_x;
        progress->layout.y = track->layout.y + (track->layout.height - progress->layout.height) * 0.5F;
        refresh_content_box(*progress);
    }
}

// Word wrap (WebCore line breaking, panorama_text_break.hpp). Runs once the
// node's WIDTH is final (resolve step 3.75) and before its parent positions the
// flow, so following siblings and valign see the multi-line height. Fills
// node.text_lines (empty = single line; paint re-derives the identical display
// text + styled runs and slices them by the stored byte ranges) and grows a
// fit-children height to the line count. Labels wrap by default, as in real
// Panorama — `white-space: nowrap` opts out; ellipsis/shrink labels keep their
// single-line truncation semantics.
void apply_text_wrap(PanoramaNode& node, const PanoramaTextMeasure& tm, float parent_cw, float parent_ch)
{
    node.text_lines.clear();
    node.text_line_advance = 0.0F;

    const PanoramaComputedStyle& s = node.computed;
    if (node.text.empty() || !tm.measure || !panorama_node_paints_own_text(node))
    {
        return;
    }
    if (s.white_space_nowrap || s.text_overflow == PanoramaTextOverflow::Ellipsis ||
        s.text_overflow == PanoramaTextOverflow::Shrink)
    {
        return;
    }
    const float available = node.layout.width - horizontal_padding(s);
    if (available <= 0.0F)
    {
        return;
    }
    // Cheap out: the single-line measure from the intrinsic pass fits (epsilon
    // absorbs per-word float-summation drift) and no '\n' forces a break.
    const bool forced_break = node.text.find('\n') != std::string::npos;
    if (!forced_break && node.intrinsic_text_width <= available + 0.25F)
    {
        return;
    }

    std::string transformed_storage;
    const std::string_view display = panorama_transform_text_view(node.text, s.text_transform, transformed_storage);
    std::vector<PanoramaTextRun> markup_runs; // keeps the views' source alive below
    std::vector<PanoramaTextWrapRun> runs;
    if (node.is_html_text())
    {
        markup_runs = panorama_parse_inline_markup(display);
        runs.reserve(markup_runs.size());
        for (const PanoramaTextRun& run : markup_runs)
        {
            runs.push_back({run.text, panorama_run_font_weight(s.font_weight, run.bold)});
        }
    }
    else
    {
        runs.push_back({display, s.font_weight});
    }

    const auto measure_segment = [&](int run, std::size_t begin, std::size_t end) {
        return tm
            .measure(runs[static_cast<std::size_t>(run)].text.substr(begin, end - begin), s.font_size,
                runs[static_cast<std::size_t>(run)].font_weight, s.letter_spacing)
            .first;
    };
    std::vector<PanoramaTextWrapLine> lines = panorama_wrap_text_lines(runs, available, measure_segment);
    if (lines.size() <= 1)
    {
        return;
    }

    const float line_box = s.line_height > 0.0F ? s.line_height : node.intrinsic_text_height;
    node.text_line_advance = line_box;
    node.text_lines = std::move(lines);

    // Only content-sized heights grow with the wrapped line count; definite
    // heights keep their box (extra lines clip per overflow, like real Panorama).
    if (s.height.type == PanoramaLengthType::FitChildren)
    {
        node.layout.height = clamp_height(
            s, static_cast<float>(node.text_lines.size()) * line_box + vertical_padding(s), parent_cw, parent_ch);
    }
}

void resolve_node(PanoramaNode& node, const PanoramaTextMeasure& tm)
{
    const PanoramaComputedStyle& s = node.computed;
    PanoramaLayoutBox& box = node.layout;

    refresh_content_box(node);

    const float cw = box.content_width;
    const float ch = box.content_height;
    const PanoramaFlow flow = s.flow;
    const bool horizontal = is_horizontal(flow);
    const bool vertical = is_vertical(flow);
    const PanoramaNode* open_dropdown_header = open_dropdown_header_child(node);

    // 0. Resolve each child's percentage margins against this content box's WIDTH
    // (CSS resolves all percentage margins against the containing block's width,
    // even top/bottom). Folded into the px `margin` the rest of layout reads; the
    // authored percent spec is left untouched so a relayout without a fresh
    // cascade still resolves correctly. The buy menu's `.wedge { margin-left: 50% }`
    // needs this to pin its rotate pivot to the wheel hub.
    for (const auto& child : node.children)
    {
        PanoramaComputedStyle& cs = child->computed;
        if (cs.margin_pct_mask == 0)
        {
            continue;
        }
        if (cs.margin_pct_mask & kPanoramaMarginTopPct)
        {
            cs.margin.top = cs.margin_pct.top / 100.0F * cw;
        }
        if (cs.margin_pct_mask & kPanoramaMarginRightPct)
        {
            cs.margin.right = cs.margin_pct.right / 100.0F * cw;
        }
        if (cs.margin_pct_mask & kPanoramaMarginBottomPct)
        {
            cs.margin.bottom = cs.margin_pct.bottom / 100.0F * cw;
        }
        if (cs.margin_pct_mask & kPanoramaMarginLeftPct)
        {
            cs.margin.left = cs.margin_pct.left / 100.0F * cw;
        }
    }

    // 1. Provisional sizes (fill children get their intrinsic size for now).
    for (const auto& child : node.children)
    {
        if (!child->computed.visible)
        {
            child->layout = PanoramaLayoutBox{};
            continue;
        }
        child->layout.width = provisional_width(*child, cw, ch);
        child->layout.height = provisional_height(*child, cw, ch);
    }

    // 2. fill-parent-flow distribution along the main axis.
    if (horizontal)
    {
        // As with vertical flows below, a main-axis `width: 100%` child behaves
        // like fill-parent-flow: it takes the space LEFT OVER after fixed/auto
        // siblings instead of the full parent width (which would overflow the
        // flow). Matches Panorama's `full-width` idiom on flow children (e.g.
        // the play menu's map-list container flowing right after the 200px
        // presets sidebar must take the REMAINING width).
        const auto is_main_fill = [](const PanoramaComputedStyle& cs) {
            return cs.width.type == PanoramaLengthType::FillParentFlow ||
                   (cs.width.type == PanoramaLengthType::Percent && cs.width.value >= 100.0F);
        };
        float used = 0.0F;
        float total_ratio = 0.0F;
        for (const auto& child : node.children)
        {
            const PanoramaComputedStyle& cs = child->computed;
            if (!cs.visible || !participates_in_parent_flow(*child, open_dropdown_header))
            {
                continue;
            }
            used += cs.margin.left + cs.margin.right;
            if (is_main_fill(cs))
            {
                total_ratio += cs.width.type == PanoramaLengthType::FillParentFlow ? cs.width.value : 1.0F;
            }
            else
            {
                used += child->layout.width;
            }
        }
        const float leftover = std::max(0.0F, cw - used);
        for (const auto& child : node.children)
        {
            const PanoramaComputedStyle& cs = child->computed;
            if (cs.visible && participates_in_parent_flow(*child, open_dropdown_header) && is_main_fill(cs))
            {
                const float ratio = cs.width.type == PanoramaLengthType::FillParentFlow ? cs.width.value : 1.0F;
                const float share = total_ratio > 0.0F ? leftover * ratio / total_ratio : 0.0F;
                child->layout.width = clamp_width(cs, share, cw, ch);
            }
        }
    }
    else if (vertical)
    {
        // A main-axis `height: 100%` (Percent) child in a down/up flow behaves like
        // fill-parent-flow: it takes the space LEFT OVER after fixed/auto siblings,
        // not the full parent height (which would overflow the flow). This matches
        // Panorama's `full-height` idiom on flow children (e.g. the play menu's content
        // column: fixed navbars + a height:100% body + a fixed action bar must all fit).
        const auto is_main_fill = [](const PanoramaComputedStyle& cs) {
            return cs.height.type == PanoramaLengthType::FillParentFlow ||
                   (cs.height.type == PanoramaLengthType::Percent && cs.height.value >= 100.0F);
        };
        float used = 0.0F;
        float total_ratio = 0.0F;
        for (const auto& child : node.children)
        {
            const PanoramaComputedStyle& cs = child->computed;
            if (!cs.visible || !participates_in_parent_flow(*child, open_dropdown_header))
            {
                continue;
            }
            used += cs.margin.top + cs.margin.bottom;
            if (is_main_fill(cs))
            {
                total_ratio += cs.height.type == PanoramaLengthType::FillParentFlow ? cs.height.value : 1.0F;
            }
            else
            {
                used += child->layout.height;
            }
        }
        const float leftover = std::max(0.0F, ch - used);
        for (const auto& child : node.children)
        {
            const PanoramaComputedStyle& cs = child->computed;
            if (cs.visible && participates_in_parent_flow(*child, open_dropdown_header) && is_main_fill(cs))
            {
                const float ratio = cs.height.type == PanoramaLengthType::FillParentFlow ? cs.height.value : 1.0F;
                const float share = total_ratio > 0.0F ? leftover * ratio / total_ratio : 0.0F;
                child->layout.height = clamp_height(cs, share, cw, ch);
            }
        }
    }

    // 3. fill-parent-flow on the cross axis (and both axes under flow:none) fills
    // the parent content box minus margins.
    for (const auto& child : node.children)
    {
        const PanoramaComputedStyle& cs = child->computed;
        if (!cs.visible || !participates_in_parent_flow(*child, open_dropdown_header))
        {
            continue;
        }
        if (cs.width.type == PanoramaLengthType::FillParentFlow && !horizontal)
        {
            child->layout.width = clamp_width(cs, cw - cs.margin.left - cs.margin.right, cw, ch);
        }
        if (cs.height.type == PanoramaLengthType::FillParentFlow && !vertical)
        {
            child->layout.height = clamp_height(cs, ch - cs.margin.top - cs.margin.bottom, cw, ch);
        }
    }

    // 3.5 Panorama's default overflow `squish` SHRINKS an oversized child to the
    // parent's definite content box on that axis (instead of merely clipping its
    // paint): the 96px navbar tab buttons squish into their 64px `--short` row
    // and keep their bottom selection underline visible. Fit-sized parent axes
    // are skipped — they derive their size FROM the children.
    {
        const bool width_definite =
            s.width.type != PanoramaLengthType::FitChildren && s.width.type != PanoramaLengthType::Auto;
        const bool height_definite =
            s.height.type != PanoramaLengthType::FitChildren && s.height.type != PanoramaLengthType::Auto;
        const bool squish_x = s.overflow_squish_x && width_definite;
        const bool squish_y = s.overflow_squish_y && height_definite;
        if (squish_x || squish_y)
        {
            for (const auto& child : node.children)
            {
                const PanoramaComputedStyle& cs = child->computed;
                if (!cs.visible)
                {
                    continue;
                }
                if (squish_x)
                {
                    child->layout.width =
                        std::min(child->layout.width, std::max(0.0F, cw - cs.margin.left - cs.margin.right));
                }
                if (squish_y)
                {
                    child->layout.height =
                        std::min(child->layout.height, std::max(0.0F, ch - cs.margin.top - cs.margin.bottom));
                }
            }
        }
    }

    // 3.75 Word wrap: every child's width is now final, so wrap overflowing
    // label text and grow fit-sized heights to the wrapped line count BEFORE
    // positioning (the flow below reads the children's heights).
    for (const auto& child : node.children)
    {
        if (child->computed.visible)
        {
            apply_text_wrap(*child, tm, cw, ch);
        }
    }

    // 4. Position children.
    if (flow == PanoramaFlow::RightWrap)
    {
        position_right_wrap_children(node, tm);
        finalize_fit_children_size(node, cw, ch);
        apply_overflow_scroll(node);
        layout_scrollbars(node);
        layout_sliders(node);
        position_open_dropdown_popup(node, tm);
        return;
    }
    if (flow == PanoramaFlow::Down_Wrap)
    {
        position_down_wrap_children(node, tm);
        finalize_fit_children_size(node, cw, ch);
        apply_overflow_scroll(node);
        layout_scrollbars(node);
        layout_sliders(node);
        position_open_dropdown_popup(node, tm);
        return;
    }

    // Panorama honours a flow child's `horizontal-align` ALONG a horizontal flow
    // as an offset into the flow's LEFTOVER space: every child keeps its
    // flow-order position, and centre/right-aligned children are additionally
    // shifted by half/all of the unused row space. CS:GO's toolbars rely on it
    // (the `horizontal-align-right` button group of a left-right-flow settings
    // row sits at the row's right edge; the GO button centres its label panel
    // inside a min-width flow-children:right button). The vertical counterpart
    // is asymmetric in real Panorama: `vertical-align: bottom` on a down-flow
    // child IS a leftover offset (the HUD's #HudWeaponPanel/#HudWeaponSelection
    // pin to the bottom of the full-height flow-children:down #HudBottomRight)
    // — applied AFTER the children resolve, see below — but `vertical-align:
    // center` is IGNORED (CS:GO's content-navbar--dropdown row and the play
    // menu's presets title sit at the top of their height:100% columns in the
    // real client despite their stray `vertical-align: center`).
    float main_leftover = 0.0F;
    if (flow == PanoramaFlow::Right)
    {
        float flow_total = 0.0F;
        for (const auto& child : node.children)
        {
            const PanoramaComputedStyle& cs = child->computed;
            if (!cs.visible || !participates_in_parent_flow(*child, open_dropdown_header))
            {
                continue;
            }
            flow_total += margin_box_width(*child);
        }
        main_leftover = std::max(0.0F, cw - flow_total);
    }
    const auto main_align_offset = [main_leftover](bool center, bool end) {
        return center ? main_leftover * 0.5F : (end ? main_leftover : 0.0F);
    };

    float pen_x = flow == PanoramaFlow::Left ? box.content_x + cw : box.content_x;
    float pen_y = flow == PanoramaFlow::Up ? box.content_y + ch : box.content_y;
    for (const auto& child : node.children)
    {
        const PanoramaComputedStyle& cs = child->computed;
        if (!cs.visible || !participates_in_parent_flow(*child, open_dropdown_header))
        {
            continue;
        }
        const float w = child->layout.width;
        const float h = child->layout.height;
        float x = 0.0F;
        float y = 0.0F;

        if (horizontal)
        {
            if (flow == PanoramaFlow::Left)
            {
                x = pen_x - cs.margin.right - w;
                pen_x = x - cs.margin.left;
            }
            else
            {
                x = pen_x + cs.margin.left;
                pen_x = x + w + cs.margin.right;
                x += main_align_offset(cs.halign == PanoramaHAlign::Center, cs.halign == PanoramaHAlign::Right);
            }
            y = box.content_y + aligned_offset_v(cs.valign, ch, h, cs.margin.top, cs.margin.bottom);
        }
        else if (vertical)
        {
            if (flow == PanoramaFlow::Up)
            {
                y = pen_y - cs.margin.bottom - h;
                pen_y = y - cs.margin.top;
            }
            else
            {
                y = pen_y + cs.margin.top;
                pen_y = y + h + cs.margin.bottom;
            }
            x = box.content_x + aligned_offset_h(cs.halign, cw, w, cs.margin.left, cs.margin.right);
        }
        else // flow: none — align within the content box on both axes
        {
            x = box.content_x + aligned_offset_h(cs.halign, cw, w, cs.margin.left, cs.margin.right);
            y = box.content_y + aligned_offset_v(cs.valign, ch, h, cs.margin.top, cs.margin.bottom);
        }

        child->layout.x = x;
        child->layout.y = y;
        resolve_positioned_child(*child, cw, ch, tm, /*in_parent_flow=*/horizontal || vertical);

        // flow:none alignment (above) used the child's PROVISIONAL size, but an
        // auto / fit-children child only reaches its final size once its own
        // subtree resolves in resolve_positioned_child — e.g. the buy menu's
        // `.buy-wheel` declares no width and fits to its `width: height-percentage(
        // 100%)` `.radial-selector` child, growing from its (height-blind)
        // intrinsic measure to the real 600px square, which left its
        // `horizontal-align: right` box ~469px off. Shift the resolved subtree by
        // the change in the ALIGNMENT term only, so the `position:` offset that
        // resolve_positioned_child just applied is preserved. A no-op for
        // definite-size children (provisional size already equals the final one)
        // and for left/top alignment (offset is size-independent).
        if (!horizontal && !vertical)
        {
            const float align_dx =
                halign_offset(cs.halign, cw - cs.margin.left - cs.margin.right - child->layout.width) -
                halign_offset(cs.halign, cw - cs.margin.left - cs.margin.right - w);
            const float align_dy =
                valign_offset(cs.valign, ch - cs.margin.top - cs.margin.bottom - child->layout.height) -
                valign_offset(cs.valign, ch - cs.margin.top - cs.margin.bottom - h);
            if (align_dx != 0.0F || align_dy != 0.0F)
            {
                offset_subtree(*child, align_dx, align_dy);
            }
        }
    }

    // Down-flow `vertical-align: bottom` = offset into the column's leftover
    // space (see the comment above main_leftover). Applied AFTER the children
    // resolved: a fit-children child's height at positioning time is its
    // intrinsic measure, which can overshoot the resolved height (the HUD's
    // #HudWeaponPanel measures 195 intrinsic but resolves to 99 — computing the
    // leftover from provisional sizes left it floating 96px above the bottom).
    if (flow == PanoramaFlow::Down)
    {
        float flow_total = 0.0F;
        bool any_bottom = false;
        for (const auto& child : node.children)
        {
            const PanoramaComputedStyle& cs = child->computed;
            if (!cs.visible || !participates_in_parent_flow(*child, open_dropdown_header))
            {
                continue;
            }
            flow_total += margin_box_height(*child);
            any_bottom = any_bottom || cs.valign == PanoramaVAlign::Bottom;
        }
        const float leftover = std::max(0.0F, ch - flow_total);
        if (any_bottom && leftover > 0.0F)
        {
            for (const auto& child : node.children)
            {
                const PanoramaComputedStyle& cs = child->computed;
                if (cs.visible && participates_in_parent_flow(*child, open_dropdown_header) &&
                    cs.valign == PanoramaVAlign::Bottom)
                {
                    offset_subtree(*child, 0.0F, leftover);
                }
            }
        }
    }

    finalize_fit_children_size(node, cw, ch);
    apply_overflow_scroll(node);
    layout_scrollbars(node);
    layout_sliders(node);
    position_open_dropdown_popup(node, tm);
}

void clear_popup_layouts(PanoramaNode& node)
{
    node.has_popup_layout = false;
    node.popup_layout = PanoramaLayoutBox{};
    for (const auto& child : node.children)
    {
        clear_popup_layouts(*child);
    }
}
}

PanoramaTextMeasure default_text_measure()
{
    PanoramaTextMeasure measure;
    measure.measure = [](std::string_view text, float font_size, int, float letter_spacing) {
        // Metrics-free approximation: ~0.5em advance, 1.2em line height. Good
        // enough for deterministic layout; PanoramaFontAtlas provides a real
        // FreeType-backed measurer for pixel accuracy.
        const float width = static_cast<float>(text.size()) * (font_size * 0.5F + letter_spacing);
        const float height = font_size * 1.2F;
        return std::pair<float, float>{width, height};
    };
    return measure;
}

void layout_panorama_tree(
    PanoramaNode& root,
    float viewport_width,
    float viewport_height,
    const PanoramaTextMeasure& text_measure)
{
    clear_popup_layouts(root);
    apply_panorama_size_defaults(root);
    measure_intrinsic(root, text_measure);

    root.layout = PanoramaLayoutBox{};
    root.layout.x = 0.0F;
    root.layout.y = 0.0F;
    root.layout.width = viewport_width;
    root.layout.height = viewport_height;

    // The root is nobody's child, so its own label text (degenerate but legal)
    // wraps here; descendants wrap inside resolve_node step 3.75.
    apply_text_wrap(root, text_measure, viewport_width, viewport_height);
    resolve_node(root, text_measure);
}
}
