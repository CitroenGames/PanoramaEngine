#include "ui/panorama/panorama_anim.hpp"

#include <algorithm>
#include <cmath>

namespace openstrike
{
namespace
{
float ease(const PanoramaEasing& easing, float t)
{
    return easing.evaluate(t);
}

std::uint8_t lerp_channel(std::uint8_t from, std::uint8_t to, float e)
{
    const float v = static_cast<float>(from) + (static_cast<float>(to) - static_cast<float>(from)) * e;
    return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F) + 0.5F);
}

// Defined in the keyframe section below; both anonymous-namespace blocks share the
// translation unit's unnamed namespace, so this forward declaration is sufficient.
PanoramaTransform lerp_transform(const PanoramaTransform& from, const PanoramaTransform& to, float e);

bool transform_equal(const PanoramaTransform& a, const PanoramaTransform& b)
{
    if (a.ops.size() != b.ops.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < a.ops.size(); ++i)
    {
        const PanoramaTransformOp& x = a.ops[i];
        const PanoramaTransformOp& y = b.ops[i];
        if (x.type != y.type || x.x != y.x || x.y != y.y || x.x_percent != y.x_percent || x.y_percent != y.y_percent)
        {
            return false;
        }
    }
    return true;
}

bool color_equal(const PanoramaColor& a, const PanoramaColor& b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

bool length_equal(const PanoramaLength& a, const PanoramaLength& b)
{
    return a.type == b.type && a.value == b.value;
}

float transition_transformed_progress(const PanoramaPropAnim& anim)
{
    if (!anim.animating)
    {
        return 1.0F;
    }
    const float active = anim.elapsed - anim.delay;
    if (active <= 0.0F)
    {
        return 0.0F;
    }
    if (anim.duration <= 0.0F || active >= anim.duration)
    {
        return 1.0F;
    }
    return ease(anim.easing, std::clamp(active / anim.duration, 0.0F, 1.0F));
}

float transition_reversing_shortening_factor(const PanoramaPropAnim& anim)
{
    return std::clamp(
        transition_transformed_progress(anim) * anim.reversing_shortening_factor +
            (1.0F - anim.reversing_shortening_factor),
        0.0F, 1.0F);
}

PanoramaPropAnim make_transition_anim(const PanoramaTransition& tr, float reversing_shortening_factor)
{
    const float factor = std::clamp(reversing_shortening_factor, 0.0F, 1.0F);
    return PanoramaPropAnim{
        true, 0.0F, tr.duration * factor, tr.delay < 0.0F ? tr.delay * factor : tr.delay, tr.easing, factor};
}

// Begins (or skips) a transition when a scalar target changes.
void start_float(float& cur, float& tgt, float& from, float& reversing_start, PanoramaPropAnim& anim, float new_target,
    const PanoramaComputedStyle& style, std::string_view property)
{
    if (new_target == tgt)
    {
        return;
    }
    if (new_target == cur)
    {
        cur = tgt = reversing_start = new_target;
        anim = PanoramaPropAnim{};
        return;
    }
    PanoramaTransition tr;
    if (style.find_transition(property, tr))
    {
        const bool reversing = anim.animating && new_target == reversing_start;
        const float old_tgt = tgt;
        const float factor = reversing ? transition_reversing_shortening_factor(anim) : 1.0F;
        from = cur;
        tgt = new_target;
        reversing_start = reversing ? old_tgt : from;
        anim = make_transition_anim(tr, factor);
    }
    else
    {
        cur = tgt = reversing_start = new_target;
        anim = PanoramaPropAnim{};
    }
}

void start_color(PanoramaColor& cur, PanoramaColor& tgt, PanoramaColor& from, PanoramaPropAnim& anim,
    PanoramaColor& reversing_start, PanoramaColor new_target, const PanoramaComputedStyle& style, std::string_view property)
{
    if (color_equal(new_target, tgt))
    {
        return;
    }
    if (color_equal(new_target, cur))
    {
        cur = tgt = reversing_start = new_target;
        anim = PanoramaPropAnim{};
        return;
    }
    PanoramaTransition tr;
    if (style.find_transition(property, tr))
    {
        const bool reversing = anim.animating && color_equal(new_target, reversing_start);
        const PanoramaColor old_tgt = tgt;
        const float factor = reversing ? transition_reversing_shortening_factor(anim) : 1.0F;
        from = cur;
        tgt = new_target;
        reversing_start = reversing ? old_tgt : from;
        anim = make_transition_anim(tr, factor);
    }
    else
    {
        cur = tgt = reversing_start = new_target;
        anim = PanoramaPropAnim{};
    }
}

// Begins (or skips) a transition when the transform target changes.
void start_transform(PanoramaTransform& cur, PanoramaTransform& tgt, PanoramaTransform& from, PanoramaPropAnim& anim,
    PanoramaTransform& reversing_start, const PanoramaTransform& new_target, const PanoramaComputedStyle& style)
{
    if (transform_equal(new_target, tgt))
    {
        return;
    }
    if (transform_equal(new_target, cur))
    {
        cur = tgt = reversing_start = new_target;
        anim = PanoramaPropAnim{};
        return;
    }
    PanoramaTransition tr;
    if (style.find_transition("transform", tr))
    {
        const bool reversing = anim.animating && transform_equal(new_target, reversing_start);
        const PanoramaTransform old_tgt = tgt;
        const float factor = reversing ? transition_reversing_shortening_factor(anim) : 1.0F;
        from = cur;
        tgt = new_target;
        reversing_start = reversing ? old_tgt : from;
        anim = make_transition_anim(tr, factor);
    }
    else
    {
        cur = tgt = reversing_start = new_target;
        anim = PanoramaPropAnim{};
    }
}

float advance_progress(PanoramaPropAnim& anim, float dt); // defined below

bool box_shadow_equal(const PanoramaBoxShadow& a, const PanoramaBoxShadow& b)
{
    return a.present == b.present && a.inset == b.inset && a.fill == b.fill && a.offset_x == b.offset_x &&
        a.offset_y == b.offset_y && a.blur == b.blur && a.spread == b.spread && a.color.r == b.color.r &&
        a.color.g == b.color.g && a.color.b == b.color.b && a.color.a == b.color.a;
}

// WebCore CSSPropertyAnimation `shadowForBlending`: an absent endpoint blends
// against a default shadow (zero geometry, transparent colour) carrying the
// PRESENT side's shadow style, so a shadow fades in/out instead of popping.
// WebCore blends colours premultiplied — against transparent black the visible
// side's hue is preserved — which a straight-RGBA lerp only matches if the
// default's RGB copies the counterpart, so it does.
PanoramaBoxShadow box_shadow_for_blending(const PanoramaBoxShadow& shadow, const PanoramaBoxShadow& other)
{
    if (shadow.present)
    {
        return shadow;
    }
    PanoramaBoxShadow def;
    def.present = true;
    def.inset = other.inset;
    def.fill = other.fill;
    def.color = other.color;
    def.color.a = 0;
    return def;
}

// WebCore ShadowData blendFunc: location/spread lerp, radius (blur) lerp clamped
// non-negative, colour lerp. Mismatched shadow styles cannot interpolate
// (PropertyWrapperShadow::canInterpolate) -> discrete swap at 50%.
PanoramaBoxShadow lerp_box_shadow(const PanoramaBoxShadow& from, const PanoramaBoxShadow& to, float t)
{
    if (!from.present && !to.present)
    {
        return to;
    }
    const PanoramaBoxShadow a = box_shadow_for_blending(from, to);
    const PanoramaBoxShadow b = box_shadow_for_blending(to, from);
    if (a.inset != b.inset || a.fill != b.fill)
    {
        return t < 0.5F ? from : to;
    }
    PanoramaBoxShadow out = b;
    out.present = true;
    out.offset_x = a.offset_x + (b.offset_x - a.offset_x) * t;
    out.offset_y = a.offset_y + (b.offset_y - a.offset_y) * t;
    out.blur = std::max(0.0F, a.blur + (b.blur - a.blur) * t);
    out.spread = a.spread + (b.spread - a.spread) * t;
    out.color.r = lerp_channel(a.color.r, b.color.r, t);
    out.color.g = lerp_channel(a.color.g, b.color.g, t);
    out.color.b = lerp_channel(a.color.b, b.color.b, t);
    out.color.a = lerp_channel(a.color.a, b.color.a, t);
    return out;
}

// Begins (or skips) a transition when the box-shadow target changes. The two
// shadows' numeric params + colour interpolate; an ABSENT endpoint blends
// against the WebCore default shadow (fade in/out). Mismatched inset/fill snaps.
void start_box_shadow(PanoramaBoxShadow& cur, PanoramaBoxShadow& tgt, PanoramaBoxShadow& from, PanoramaPropAnim& anim,
    PanoramaBoxShadow& reversing_start, const PanoramaBoxShadow& new_target, const PanoramaComputedStyle& style)
{
    if (box_shadow_equal(new_target, tgt))
    {
        return;
    }
    if (box_shadow_equal(new_target, cur))
    {
        cur = tgt = reversing_start = new_target;
        anim = PanoramaPropAnim{};
        return;
    }
    PanoramaTransition tr;
    const bool interpolatable = (new_target.present || cur.present) &&
        (!new_target.present || !cur.present ||
            (new_target.inset == cur.inset && new_target.fill == cur.fill));
    if (interpolatable && style.find_transition("box-shadow", tr))
    {
        const bool reversing = anim.animating && box_shadow_equal(new_target, reversing_start);
        const PanoramaBoxShadow old_tgt = tgt;
        const float factor = reversing ? transition_reversing_shortening_factor(anim) : 1.0F;
        from = cur;
        tgt = new_target;
        reversing_start = reversing ? old_tgt : from;
        anim = make_transition_anim(tr, factor);
    }
    else
    {
        cur = tgt = reversing_start = new_target;
        anim = PanoramaPropAnim{};
    }
}

void advance_box_shadow(PanoramaBoxShadow& cur, PanoramaBoxShadow tgt, PanoramaBoxShadow from, PanoramaPropAnim& anim,
    float dt)
{
    if (!anim.animating)
    {
        cur = tgt;
        return;
    }
    const float e = advance_progress(anim, dt);
    if (e < 0.0F)
    {
        cur = from;
    }
    else if (e >= 1.0F)
    {
        cur = tgt;
    }
    else
    {
        cur = lerp_box_shadow(from, tgt, e);
    }
}

bool blur_equal(const PanoramaBlur& a, const PanoramaBlur& b)
{
    return a.present == b.present && a.std_x == b.std_x && a.std_y == b.std_y && a.passes == b.passes;
}

// WebCore filter blur interpolation: the missing endpoint is the identity
// (stddev 0), so blur fades in/out; stddevs lerp clamped non-negative. `passes`
// follows the present side (sigma_eff = sigma*sqrt(passes) stays continuous when
// only the stddev animates, which is how the CS:GO sheets use it).
PanoramaBlur lerp_blur(const PanoramaBlur& from, const PanoramaBlur& to, float t)
{
    if (!from.present && !to.present)
    {
        return to;
    }
    PanoramaBlur a = from;
    PanoramaBlur b = to;
    if (!a.present)
    {
        a = PanoramaBlur{true, 0.0F, 0.0F, b.passes};
    }
    if (!b.present)
    {
        b = PanoramaBlur{true, 0.0F, 0.0F, a.passes};
    }
    PanoramaBlur out;
    out.present = true;
    out.std_x = std::max(0.0F, a.std_x + (b.std_x - a.std_x) * t);
    out.std_y = std::max(0.0F, a.std_y + (b.std_y - a.std_y) * t);
    out.passes = std::max(0.0F, a.passes + (b.passes - a.passes) * t);
    return out;
}

void start_blur(PanoramaBlur& cur, PanoramaBlur& tgt, PanoramaBlur& from, PanoramaPropAnim& anim,
    PanoramaBlur& reversing_start, const PanoramaBlur& new_target, const PanoramaComputedStyle& style)
{
    if (blur_equal(new_target, tgt))
    {
        return;
    }
    if (blur_equal(new_target, cur))
    {
        cur = tgt = reversing_start = new_target;
        anim = PanoramaPropAnim{};
        return;
    }
    PanoramaTransition tr;
    if ((new_target.present || cur.present) && style.find_transition("blur", tr))
    {
        const bool reversing = anim.animating && blur_equal(new_target, reversing_start);
        const PanoramaBlur old_tgt = tgt;
        const float factor = reversing ? transition_reversing_shortening_factor(anim) : 1.0F;
        from = cur;
        tgt = new_target;
        reversing_start = reversing ? old_tgt : from;
        anim = make_transition_anim(tr, factor);
    }
    else
    {
        cur = tgt = reversing_start = new_target;
        anim = PanoramaPropAnim{};
    }
}

void advance_blur(PanoramaBlur& cur, PanoramaBlur tgt, PanoramaBlur from, PanoramaPropAnim& anim, float dt)
{
    if (!anim.animating)
    {
        cur = tgt;
        return;
    }
    const float e = advance_progress(anim, dt);
    if (e < 0.0F)
    {
        cur = from;
    }
    else if (e >= 1.0F)
    {
        cur = tgt;
    }
    else
    {
        cur = lerp_blur(from, tgt, e);
    }
}

bool clip_equal(const PanoramaClip& a, const PanoramaClip& b)
{
    return a.type == b.type && a.rect_top == b.rect_top && a.rect_right == b.rect_right &&
        a.rect_bottom == b.rect_bottom && a.rect_left == b.rect_left && a.radial_center_x == b.radial_center_x &&
        a.radial_center_y == b.radial_center_y && a.radial_start == b.radial_start &&
        a.radial_sweep == b.radial_sweep;
}

// clip interpolation: same-type rect edges / radial centre+angles lerp
// component-wise (CSS Length/angle blending); a type mismatch is discrete (swap
// at 50%, like WebCore's non-interpolable clip shapes).
PanoramaClip lerp_clip(const PanoramaClip& from, const PanoramaClip& to, float t)
{
    if (from.type != to.type)
    {
        return t < 0.5F ? from : to;
    }
    PanoramaClip out = to;
    out.rect_top = from.rect_top + (to.rect_top - from.rect_top) * t;
    out.rect_right = from.rect_right + (to.rect_right - from.rect_right) * t;
    out.rect_bottom = from.rect_bottom + (to.rect_bottom - from.rect_bottom) * t;
    out.rect_left = from.rect_left + (to.rect_left - from.rect_left) * t;
    out.radial_center_x = from.radial_center_x + (to.radial_center_x - from.radial_center_x) * t;
    out.radial_center_y = from.radial_center_y + (to.radial_center_y - from.radial_center_y) * t;
    out.radial_start = from.radial_start + (to.radial_start - from.radial_start) * t;
    out.radial_sweep = from.radial_sweep + (to.radial_sweep - from.radial_sweep) * t;
    return out;
}

void start_clip(PanoramaClip& cur, PanoramaClip& tgt, PanoramaClip& from, PanoramaPropAnim& anim,
    PanoramaClip& reversing_start, const PanoramaClip& new_target, const PanoramaComputedStyle& style)
{
    if (clip_equal(new_target, tgt))
    {
        return;
    }
    if (clip_equal(new_target, cur))
    {
        cur = tgt = reversing_start = new_target;
        anim = PanoramaPropAnim{};
        return;
    }
    PanoramaTransition tr;
    const bool interpolatable = new_target.type == cur.type && new_target.type != PanoramaClipType::None;
    if (interpolatable && style.find_transition("clip", tr))
    {
        const bool reversing = anim.animating && clip_equal(new_target, reversing_start);
        const PanoramaClip old_tgt = tgt;
        const float factor = reversing ? transition_reversing_shortening_factor(anim) : 1.0F;
        from = cur;
        tgt = new_target;
        reversing_start = reversing ? old_tgt : from;
        anim = make_transition_anim(tr, factor);
    }
    else
    {
        cur = tgt = reversing_start = new_target;
        anim = PanoramaPropAnim{};
    }
}

void advance_clip(PanoramaClip& cur, PanoramaClip tgt, PanoramaClip from, PanoramaPropAnim& anim, float dt)
{
    if (!anim.animating)
    {
        cur = tgt;
        return;
    }
    const float e = advance_progress(anim, dt);
    if (e < 0.0F)
    {
        cur = from;
    }
    else if (e >= 1.0F)
    {
        cur = tgt;
    }
    else
    {
        cur = lerp_clip(from, tgt, e);
    }
}

// Begins (or skips) a transition when a length target changes. Only interpolates
// between same-type definite lengths (px<->px, %<->%); other changes snap.
void start_length(PanoramaLength& cur, PanoramaLength& tgt, PanoramaLength& from, PanoramaPropAnim& anim,
    PanoramaLength& reversing_start, PanoramaLength new_target, const PanoramaComputedStyle& style,
    std::string_view property)
{
    if (length_equal(new_target, tgt))
    {
        return;
    }
    if (length_equal(new_target, cur))
    {
        cur = tgt = reversing_start = new_target;
        anim = PanoramaPropAnim{};
        return;
    }
    PanoramaTransition tr;
    const bool interpolatable = new_target.type == cur.type && new_target.is_definite();
    if (interpolatable && style.find_transition(property, tr))
    {
        const bool reversing = anim.animating && length_equal(new_target, reversing_start);
        const PanoramaLength old_tgt = tgt;
        const float factor = reversing ? transition_reversing_shortening_factor(anim) : 1.0F;
        from = cur;
        tgt = new_target;
        reversing_start = reversing ? old_tgt : from;
        anim = make_transition_anim(tr, factor);
    }
    else
    {
        cur = tgt = reversing_start = new_target;
        anim = PanoramaPropAnim{};
    }
}

void advance_length(PanoramaLength& cur, PanoramaLength tgt, PanoramaLength from, PanoramaPropAnim& anim, float dt)
{
    if (!anim.animating)
    {
        cur = tgt;
        return;
    }
    const float e = advance_progress(anim, dt);
    cur.type = tgt.type;
    if (e < 0.0F)
    {
        cur.value = from.value;
    }
    else if (e >= 1.0F)
    {
        cur.value = tgt.value;
    }
    else
    {
        cur.value = from.value + (tgt.value - from.value) * e;
    }
}

// Returns the eased progress in [0,1], or -1 while still in the delay phase.
float advance_progress(PanoramaPropAnim& anim, float dt)
{
    anim.elapsed += dt;
    const float active = anim.elapsed - anim.delay;
    if (active <= 0.0F)
    {
        return -1.0F;
    }
    if (anim.duration <= 0.0F || active >= anim.duration)
    {
        anim.animating = false;
        return 1.0F;
    }
    return ease(anim.easing, active / anim.duration);
}

void advance_float(float& cur, float tgt, float from, PanoramaPropAnim& anim, float dt)
{
    if (!anim.animating)
    {
        cur = tgt;
        return;
    }
    const float e = advance_progress(anim, dt);
    if (e < 0.0F)
    {
        cur = from;
    }
    else if (e >= 1.0F)
    {
        cur = tgt;
    }
    else
    {
        cur = from + (tgt - from) * e;
    }
}

void advance_color(PanoramaColor& cur, PanoramaColor tgt, PanoramaColor from, PanoramaPropAnim& anim, float dt)
{
    if (!anim.animating)
    {
        cur = tgt;
        return;
    }
    const float e = advance_progress(anim, dt);
    if (e < 0.0F)
    {
        cur = from;
    }
    else if (e >= 1.0F)
    {
        cur = tgt;
    }
    else
    {
        cur.r = lerp_channel(from.r, tgt.r, e);
        cur.g = lerp_channel(from.g, tgt.g, e);
        cur.b = lerp_channel(from.b, tgt.b, e);
        cur.a = lerp_channel(from.a, tgt.a, e);
    }
}

// Per-node capture body (no recursion): seeds or re-targets this node's
// transition state from its computed style.
void capture_node_values(PanoramaNode& node)
{
    PanoramaAnimState& a = node.anim;
    const PanoramaComputedStyle& s = node.computed;

    if (!a.initialized)
    {
        a.opacity_cur = a.opacity_tgt = a.opacity_from = s.opacity;
        a.opacity_reversing_start = s.opacity;
        a.pos_x_cur = a.pos_x_tgt = a.pos_x_from = s.pos_x;
        a.pos_y_cur = a.pos_y_tgt = a.pos_y_from = s.pos_y;
        a.pos_x_reversing_start = s.pos_x;
        a.pos_y_reversing_start = s.pos_y;
        a.pos_has = s.has_position;
        a.pos_x_percent = s.pos_x_percent;
        a.pos_y_percent = s.pos_y_percent;
        a.color_cur = a.color_tgt = a.color_from = s.color;
        a.color_reversing_start = s.color;
        a.bg_cur = a.bg_tgt = a.bg_from = s.background_color;
        a.bg_reversing_start = s.background_color;
        a.background_image_opacity_cur = a.background_image_opacity_tgt = a.background_image_opacity_from =
            s.background_image_opacity;
        a.background_image_opacity_reversing_start = s.background_image_opacity;
        a.wash_cur = a.wash_tgt = a.wash_from = s.wash_color;
        a.wash_reversing_start = s.wash_color;
        a.brightness_cur = a.brightness_tgt = a.brightness_from = s.brightness;
        a.brightness_reversing_start = s.brightness;
        a.transform_cur = a.transform_tgt = a.transform_from = s.transform;
        a.transform_reversing_start = s.transform;
        a.width_cur = a.width_tgt = a.width_from = s.width;
        a.width_reversing_start = s.width;
        a.height_cur = a.height_tgt = a.height_from = s.height;
        a.height_reversing_start = s.height;
        a.border_width_cur = a.border_width_tgt = a.border_width_from = s.border_width;
        a.border_width_reversing_start = s.border_width;
        a.border_color_cur = a.border_color_tgt = a.border_color_from = s.border_color;
        a.border_color_reversing_start = s.border_color;
        a.box_shadow_cur = a.box_shadow_tgt = a.box_shadow_from = s.box_shadow;
        a.box_shadow_reversing_start = s.box_shadow;
        a.blur_cur = a.blur_tgt = a.blur_from = s.blur;
        a.blur_reversing_start = s.blur;
        a.clip_cur = a.clip_tgt = a.clip_from = s.clip;
        a.clip_reversing_start = s.clip;
        a.pre_scale_x_cur = a.pre_scale_x_tgt = a.pre_scale_x_from = s.pre_scale_x;
        a.pre_scale_y_cur = a.pre_scale_y_tgt = a.pre_scale_y_from = s.pre_scale_y;
        a.pre_scale_x_reversing_start = s.pre_scale_x;
        a.pre_scale_y_reversing_start = s.pre_scale_y;
        a.initialized = true;
    }
    else
    {
        start_float(
            a.opacity_cur, a.opacity_tgt, a.opacity_from, a.opacity_reversing_start, a.opacity, s.opacity, s, "opacity");

        // Position: percent flags follow the target; the two coordinates share one
        // `position` transition.
        a.pos_has = s.has_position;
        a.pos_x_percent = s.pos_x_percent;
        a.pos_y_percent = s.pos_y_percent;
        if (s.pos_x != a.pos_x_tgt || s.pos_y != a.pos_y_tgt)
        {
            if (s.pos_x == a.pos_x_cur && s.pos_y == a.pos_y_cur)
            {
                a.pos_x_cur = a.pos_x_tgt = a.pos_x_reversing_start = s.pos_x;
                a.pos_y_cur = a.pos_y_tgt = a.pos_y_reversing_start = s.pos_y;
                a.pos = PanoramaPropAnim{};
            }
            else
            {
                PanoramaTransition tr;
                if (s.find_transition("position", tr))
                {
                    const bool reversing = a.pos.animating && s.pos_x == a.pos_x_reversing_start &&
                        s.pos_y == a.pos_y_reversing_start;
                    const float old_tgt_x = a.pos_x_tgt;
                    const float old_tgt_y = a.pos_y_tgt;
                    const float factor = reversing ? transition_reversing_shortening_factor(a.pos) : 1.0F;
                    a.pos_x_from = a.pos_x_cur;
                    a.pos_y_from = a.pos_y_cur;
                    a.pos_x_tgt = s.pos_x;
                    a.pos_y_tgt = s.pos_y;
                    a.pos_x_reversing_start = reversing ? old_tgt_x : a.pos_x_from;
                    a.pos_y_reversing_start = reversing ? old_tgt_y : a.pos_y_from;
                    a.pos = make_transition_anim(tr, factor);
                }
                else
                {
                    a.pos_x_cur = a.pos_x_tgt = a.pos_x_reversing_start = s.pos_x;
                    a.pos_y_cur = a.pos_y_tgt = a.pos_y_reversing_start = s.pos_y;
                    a.pos = PanoramaPropAnim{};
                }
            }
        }

        start_color(a.color_cur, a.color_tgt, a.color_from, a.color, a.color_reversing_start, s.color, s, "color");
        start_color(a.bg_cur, a.bg_tgt, a.bg_from, a.bg, a.bg_reversing_start, s.background_color, s,
            "background-color");
        start_float(a.background_image_opacity_cur,
            a.background_image_opacity_tgt,
            a.background_image_opacity_from,
            a.background_image_opacity_reversing_start,
            a.background_image_opacity,
            s.background_image_opacity,
            s,
            "background-img-opacity");
        start_color(a.wash_cur, a.wash_tgt, a.wash_from, a.wash, a.wash_reversing_start, s.wash_color, s,
            "wash-color");
        start_float(a.brightness_cur, a.brightness_tgt, a.brightness_from, a.brightness_reversing_start,
            a.brightness, s.brightness, s, "brightness");
        start_transform(
            a.transform_cur, a.transform_tgt, a.transform_from, a.transform, a.transform_reversing_start, s.transform, s);
        start_length(a.width_cur, a.width_tgt, a.width_from, a.width, a.width_reversing_start, s.width, s, "width");
        start_length(
            a.height_cur, a.height_tgt, a.height_from, a.height, a.height_reversing_start, s.height, s, "height");
        start_float(a.border_width_cur, a.border_width_tgt, a.border_width_from, a.border_width_reversing_start,
            a.border_width, s.border_width, s, "border");
        start_color(a.border_color_cur, a.border_color_tgt, a.border_color_from, a.border_color,
            a.border_color_reversing_start, s.border_color, s, "border");
        start_box_shadow(a.box_shadow_cur, a.box_shadow_tgt, a.box_shadow_from, a.box_shadow,
            a.box_shadow_reversing_start, s.box_shadow, s);
        start_blur(a.blur_cur, a.blur_tgt, a.blur_from, a.blur, a.blur_reversing_start, s.blur, s);
        start_clip(a.clip_cur, a.clip_tgt, a.clip_from, a.clip, a.clip_reversing_start, s.clip, s);

        // pre-transform-scale2d: two values share one transition (like position).
        if (s.pre_scale_x != a.pre_scale_x_tgt || s.pre_scale_y != a.pre_scale_y_tgt)
        {
            if (s.pre_scale_x == a.pre_scale_x_cur && s.pre_scale_y == a.pre_scale_y_cur)
            {
                a.pre_scale_x_cur = a.pre_scale_x_tgt = a.pre_scale_x_reversing_start = s.pre_scale_x;
                a.pre_scale_y_cur = a.pre_scale_y_tgt = a.pre_scale_y_reversing_start = s.pre_scale_y;
                a.pre_scale = PanoramaPropAnim{};
            }
            else
            {
                PanoramaTransition tr;
                if (s.find_transition("pre-transform-scale2d", tr))
                {
                    const bool reversing = a.pre_scale.animating &&
                        s.pre_scale_x == a.pre_scale_x_reversing_start &&
                        s.pre_scale_y == a.pre_scale_y_reversing_start;
                    const float old_tgt_x = a.pre_scale_x_tgt;
                    const float old_tgt_y = a.pre_scale_y_tgt;
                    const float factor = reversing ? transition_reversing_shortening_factor(a.pre_scale) : 1.0F;
                    a.pre_scale_x_from = a.pre_scale_x_cur;
                    a.pre_scale_y_from = a.pre_scale_y_cur;
                    a.pre_scale_x_tgt = s.pre_scale_x;
                    a.pre_scale_y_tgt = s.pre_scale_y;
                    a.pre_scale_x_reversing_start = reversing ? old_tgt_x : a.pre_scale_x_from;
                    a.pre_scale_y_reversing_start = reversing ? old_tgt_y : a.pre_scale_y_from;
                    a.pre_scale = make_transition_anim(tr, factor);
                }
                else
                {
                    a.pre_scale_x_cur = a.pre_scale_x_tgt = a.pre_scale_x_reversing_start = s.pre_scale_x;
                    a.pre_scale_y_cur = a.pre_scale_y_tgt = a.pre_scale_y_reversing_start = s.pre_scale_y;
                    a.pre_scale = PanoramaPropAnim{};
                }
            }
        }
    }
}

void capture_node(PanoramaNode& node)
{
    node.style_fresh = false;
    capture_node_values(node);
    for (const auto& child : node.children)
    {
        capture_node(*child);
    }
}

// Selective capture: only nodes the cascade actually recomputed (style_fresh).
// Untouched nodes hold the per-frame INTERPOLATED value in node.computed (the
// advance pass writes it back), which a re-capture would mistake for a new
// cascade target and cancel the in-flight transition at its current value.
void capture_recomputed_node(PanoramaNode& node)
{
    if (node.style_fresh)
    {
        node.style_fresh = false;
        capture_node_values(node);
    }
    for (const auto& child : node.children)
    {
        capture_recomputed_node(*child);
    }
}

void merge_result(PanoramaAnimationAdvanceResult& dst, const PanoramaAnimationAdvanceResult& src)
{
    dst.visual_changed = dst.visual_changed || src.visual_changed;
    dst.layout_changed = dst.layout_changed || src.layout_changed;
    dst.active = dst.active || src.active;
}

// `ends` accumulates transitions that completed during this advance (the
// `animating` flag flipped false because the active time reached the duration —
// advance_progress applies the end value in the same step). Retargets/snaps in
// the capture pass replace the PanoramaPropAnim without ever completing, so they
// never report here (WebCore: transitioncancel, not transitionend).
PanoramaAnimationAdvanceResult advance_node(PanoramaNode& node, float dt, std::vector<PanoramaTransitionEnd>& ends)
{
    PanoramaAnimState& a = node.anim;
    PanoramaAnimationAdvanceResult result;

    const bool opacity_was_animating = a.opacity.animating;
    const bool pos_was_animating = a.pos.animating;
    const bool color_was_animating = a.color.animating;
    const bool bg_was_animating = a.bg.animating;
    const bool background_image_opacity_was_animating = a.background_image_opacity.animating;
    const bool wash_was_animating = a.wash.animating;
    const bool brightness_was_animating = a.brightness.animating;
    const bool transform_was_animating = a.transform.animating;
    const bool width_was_animating = a.width.animating;
    const bool height_was_animating = a.height.animating;
    const bool border_width_was_animating = a.border_width.animating;
    const bool border_color_was_animating = a.border_color.animating;
    const bool box_shadow_was_animating = a.box_shadow.animating;
    const bool blur_was_animating = a.blur.animating;
    const bool clip_was_animating = a.clip.animating;
    const bool pre_scale_was_animating = a.pre_scale.animating;

    advance_float(a.opacity_cur, a.opacity_tgt, a.opacity_from, a.opacity, dt);
    node.computed.opacity = a.opacity_cur;

    if (a.pos.animating)
    {
        const float e = advance_progress(a.pos, dt);
        if (e < 0.0F)
        {
            a.pos_x_cur = a.pos_x_from;
            a.pos_y_cur = a.pos_y_from;
        }
        else if (e >= 1.0F)
        {
            a.pos_x_cur = a.pos_x_tgt;
            a.pos_y_cur = a.pos_y_tgt;
        }
        else
        {
            a.pos_x_cur = a.pos_x_from + (a.pos_x_tgt - a.pos_x_from) * e;
            a.pos_y_cur = a.pos_y_from + (a.pos_y_tgt - a.pos_y_from) * e;
        }
    }
    else
    {
        a.pos_x_cur = a.pos_x_tgt;
        a.pos_y_cur = a.pos_y_tgt;
    }
    node.computed.has_position = a.pos_has;
    node.computed.pos_x = a.pos_x_cur;
    node.computed.pos_y = a.pos_y_cur;
    node.computed.pos_x_percent = a.pos_x_percent;
    node.computed.pos_y_percent = a.pos_y_percent;

    advance_color(a.color_cur, a.color_tgt, a.color_from, a.color, dt);
    node.computed.color = a.color_cur;
    advance_color(a.bg_cur, a.bg_tgt, a.bg_from, a.bg, dt);
    node.computed.background_color = a.bg_cur;
    advance_float(a.background_image_opacity_cur,
        a.background_image_opacity_tgt,
        a.background_image_opacity_from,
        a.background_image_opacity,
        dt);
    node.computed.background_image_opacity = a.background_image_opacity_cur;

    advance_color(a.wash_cur, a.wash_tgt, a.wash_from, a.wash, dt);
    node.computed.wash_color = a.wash_cur;
    advance_float(a.brightness_cur, a.brightness_tgt, a.brightness_from, a.brightness, dt);
    node.computed.brightness = a.brightness_cur;

    // Transform (paint-only; does not affect layout in this engine).
    if (a.transform.animating)
    {
        const float e = advance_progress(a.transform, dt);
        if (e < 0.0F)
        {
            a.transform_cur = a.transform_from;
        }
        else if (e >= 1.0F)
        {
            a.transform_cur = a.transform_tgt;
        }
        else
        {
            a.transform_cur = lerp_transform(a.transform_from, a.transform_tgt, e);
        }
    }
    else
    {
        a.transform_cur = a.transform_tgt;
    }
    node.computed.transform = a.transform_cur;

    advance_length(a.width_cur, a.width_tgt, a.width_from, a.width, dt);
    node.computed.width = a.width_cur;
    advance_length(a.height_cur, a.height_tgt, a.height_from, a.height, dt);
    node.computed.height = a.height_cur;

    advance_float(a.border_width_cur, a.border_width_tgt, a.border_width_from, a.border_width, dt);
    node.computed.border_width = a.border_width_cur;
    advance_color(a.border_color_cur, a.border_color_tgt, a.border_color_from, a.border_color, dt);
    node.computed.border_color = a.border_color_cur;

    advance_box_shadow(a.box_shadow_cur, a.box_shadow_tgt, a.box_shadow_from, a.box_shadow, dt);
    node.computed.box_shadow = a.box_shadow_cur;
    advance_blur(a.blur_cur, a.blur_tgt, a.blur_from, a.blur, dt);
    node.computed.blur = a.blur_cur;
    advance_clip(a.clip_cur, a.clip_tgt, a.clip_from, a.clip, dt);
    node.computed.clip = a.clip_cur;

    if (a.pre_scale.animating)
    {
        const float e = advance_progress(a.pre_scale, dt);
        const float t = e < 0.0F ? 0.0F : (e > 1.0F ? 1.0F : e);
        a.pre_scale_x_cur = a.pre_scale_x_from + (a.pre_scale_x_tgt - a.pre_scale_x_from) * t;
        a.pre_scale_y_cur = a.pre_scale_y_from + (a.pre_scale_y_tgt - a.pre_scale_y_from) * t;
    }
    else
    {
        a.pre_scale_x_cur = a.pre_scale_x_tgt;
        a.pre_scale_y_cur = a.pre_scale_y_tgt;
    }
    node.computed.pre_scale_x = a.pre_scale_x_cur;
    node.computed.pre_scale_y = a.pre_scale_y_cur;

    // Completed transitions, keyed by the same canonical property names the
    // capture pass feeds find_transition (what `transition-property` matches and
    // what CS:GO PropertyTransitionEnd handlers compare propertyName against).
    const auto ended = [&](bool was, const PanoramaPropAnim& anim, const char* property) {
        if (was && !anim.animating)
        {
            ends.push_back(PanoramaTransitionEnd{&node, property});
        }
    };
    ended(opacity_was_animating, a.opacity, "opacity");
    ended(pos_was_animating, a.pos, "position");
    ended(color_was_animating, a.color, "color");
    ended(bg_was_animating, a.bg, "background-color");
    ended(background_image_opacity_was_animating, a.background_image_opacity, "background-img-opacity");
    ended(wash_was_animating, a.wash, "wash-color");
    ended(brightness_was_animating, a.brightness, "brightness");
    ended(transform_was_animating, a.transform, "transform");
    ended(width_was_animating, a.width, "width");
    ended(height_was_animating, a.height, "height");
    // Both border slots transition under the single "border" key; report it once.
    if ((border_width_was_animating && !a.border_width.animating) ||
        (border_color_was_animating && !a.border_color.animating))
    {
        ends.push_back(PanoramaTransitionEnd{&node, "border"});
    }
    ended(box_shadow_was_animating, a.box_shadow, "box-shadow");
    ended(blur_was_animating, a.blur, "blur");
    ended(clip_was_animating, a.clip, "clip");
    ended(pre_scale_was_animating, a.pre_scale, "pre-transform-scale2d");

    result.visual_changed = opacity_was_animating || a.opacity.animating || pos_was_animating || a.pos.animating ||
        color_was_animating || a.color.animating || bg_was_animating || a.bg.animating || wash_was_animating ||
        a.wash.animating || background_image_opacity_was_animating || a.background_image_opacity.animating ||
        brightness_was_animating || a.brightness.animating || transform_was_animating || a.transform.animating ||
        width_was_animating || a.width.animating || height_was_animating || a.height.animating ||
        border_width_was_animating || a.border_width.animating || border_color_was_animating ||
        a.border_color.animating || box_shadow_was_animating || a.box_shadow.animating || blur_was_animating ||
        a.blur.animating || clip_was_animating || a.clip.animating || pre_scale_was_animating ||
        a.pre_scale.animating;
    result.layout_changed = pos_was_animating || a.pos.animating || width_was_animating || a.width.animating ||
        height_was_animating || a.height.animating;
    result.active = a.opacity.animating || a.pos.animating || a.color.animating || a.bg.animating || a.wash.animating ||
        a.background_image_opacity.animating || a.brightness.animating || a.transform.animating || a.width.animating ||
        a.height.animating || a.border_width.animating || a.border_color.animating || a.box_shadow.animating ||
        a.blur.animating || a.clip.animating || a.pre_scale.animating;

    for (const auto& child : node.children)
    {
        merge_result(result, advance_node(*child, dt, ends));
    }

    return result;
}
}

void panorama_capture_anim_targets(PanoramaNode& root)
{
    capture_node(root);
}

void panorama_capture_anim_targets_recomputed(PanoramaNode& root)
{
    capture_recomputed_node(root);
}

void panorama_sync_anim_dimensions(PanoramaNode& node)
{
    PanoramaAnimState& a = node.anim;
    a.width_cur = a.width_tgt = a.width_from = node.computed.width;
    a.width_reversing_start = node.computed.width;
    a.width = PanoramaPropAnim{};
    a.height_cur = a.height_tgt = a.height_from = node.computed.height;
    a.height_reversing_start = node.computed.height;
    a.height = PanoramaPropAnim{};
}

PanoramaAnimationAdvanceResult panorama_advance_anim(PanoramaNode& root, float dt)
{
    std::vector<PanoramaTransitionEnd> ends;
    PanoramaAnimationAdvanceResult result = advance_node(root, dt, ends);
    result.transition_ends = std::move(ends);
    return result;
}

// ---- @keyframes animation runtime -------------------------------------------
namespace
{
PanoramaColor lerp_color(PanoramaColor from, PanoramaColor to, float e)
{
    PanoramaColor out;
    out.r = lerp_channel(from.r, to.r, e);
    out.g = lerp_channel(from.g, to.g, e);
    out.b = lerp_channel(from.b, to.b, e);
    out.a = lerp_channel(from.a, to.a, e);
    return out;
}

// Interpolates two transform op-lists. When the lists have the same shape (same
// length and matching per-index op types) the components lerp; otherwise there is
// no meaningful correspondence, so we snap to the nearer endpoint.
PanoramaTransform lerp_transform(const PanoramaTransform& from, const PanoramaTransform& to, float e)
{
    if (from.ops.size() != to.ops.size())
    {
        return e < 0.5F ? from : to;
    }
    PanoramaTransform out;
    out.ops.reserve(from.ops.size());
    for (std::size_t i = 0; i < from.ops.size(); ++i)
    {
        if (from.ops[i].type != to.ops[i].type)
        {
            return e < 0.5F ? from : to;
        }
        PanoramaTransformOp op = from.ops[i];
        op.x = from.ops[i].x + (to.ops[i].x - from.ops[i].x) * e;
        op.y = from.ops[i].y + (to.ops[i].y - from.ops[i].y) * e;
        out.ops.push_back(op);
    }
    return out;
}

// The keyframe-timeline position [0,1] for a given iteration index and intra-
// iteration fraction under a play direction.
float directional_position(long iteration, float frac, PanoramaAnimDirection direction)
{
    const bool odd = (iteration & 1L) != 0L;
    switch (direction)
    {
    case PanoramaAnimDirection::Reverse:
        return 1.0F - frac;
    case PanoramaAnimDirection::Alternate:
        return odd ? 1.0F - frac : frac;
    case PanoramaAnimDirection::AlternateReverse:
        return odd ? frac : 1.0F - frac;
    case PanoramaAnimDirection::Normal:
    default:
        return frac;
    }
}

struct ChannelSegment
{
    const PanoramaKeyframeStop* lo = nullptr;
    const PanoramaKeyframeStop* hi = nullptr;
    float frac = 0.0F; // position within [lo.offset, hi.offset]
};

// Finds the two stops that bracket timeline position `t` among those declaring
// `channel`. Clamps to the first/last declaring stop outside their range.
ChannelSegment channel_segment(const PanoramaKeyframes& keyframes, std::uint32_t channel, float t)
{
    ChannelSegment seg;
    for (const PanoramaKeyframeStop& stop : keyframes.stops)
    {
        if ((stop.channels & channel) == 0)
        {
            continue;
        }
        if (stop.offset <= t)
        {
            seg.lo = &stop;
        }
        if (stop.offset >= t)
        {
            seg.hi = &stop;
            break; // stops are sorted ascending: this is the first stop at/after t
        }
    }
    if (seg.lo == nullptr)
    {
        seg.lo = seg.hi; // t precedes the first declaring stop
    }
    if (seg.hi == nullptr)
    {
        seg.hi = seg.lo; // t follows the last declaring stop
    }
    if (seg.lo != nullptr && seg.hi != nullptr && seg.hi != seg.lo)
    {
        const float span = seg.hi->offset - seg.lo->offset;
        seg.frac = span > 0.0F ? (t - seg.lo->offset) / span : 0.0F;
    }
    return seg;
}

// Writes every animated channel of `keyframes` at timeline position `t` into the
// node's computed style. `default_easing` is the animation-level timing function,
// overridden per segment by a stop's own animation-timing-function.
void apply_keyframes_at(PanoramaNode& node, const PanoramaKeyframes& keyframes, float t, PanoramaEasing default_easing)
{
    PanoramaComputedStyle& computed = node.computed;
    const auto eval = [&](std::uint32_t channel, auto&& writer) {
        if ((keyframes.channels & channel) == 0)
        {
            return;
        }
        const ChannelSegment seg = channel_segment(keyframes, channel, t);
        if (seg.lo == nullptr || seg.hi == nullptr)
        {
            return;
        }
        const PanoramaEasing easing = seg.lo->has_easing ? seg.lo->easing : default_easing;
        const float e = seg.lo == seg.hi ? 0.0F : ease(easing, seg.frac);
        writer(seg.lo->resolved, seg.hi->resolved, e);
    };

    eval(PanoramaAnimOpacity, [&](const PanoramaComputedStyle& a, const PanoramaComputedStyle& b, float e) {
        computed.opacity = std::clamp(a.opacity + (b.opacity - a.opacity) * e, 0.0F, 1.0F);
    });
    eval(PanoramaAnimBrightness, [&](const PanoramaComputedStyle& a, const PanoramaComputedStyle& b, float e) {
        computed.brightness = std::max(0.0F, a.brightness + (b.brightness - a.brightness) * e);
    });
    eval(PanoramaAnimBackgroundColor, [&](const PanoramaComputedStyle& a, const PanoramaComputedStyle& b, float e) {
        computed.background_color = lerp_color(a.background_color, b.background_color, e);
    });
    eval(PanoramaAnimBackgroundImageOpacity, [&](const PanoramaComputedStyle& a, const PanoramaComputedStyle& b, float e) {
        const float opacity = a.background_image_opacity + (b.background_image_opacity - a.background_image_opacity) * e;
        computed.background_image_opacity = std::clamp(opacity, 0.0F, 1.0F);
    });
    eval(PanoramaAnimColor, [&](const PanoramaComputedStyle& a, const PanoramaComputedStyle& b, float e) {
        computed.color = lerp_color(a.color, b.color, e);
    });
    eval(PanoramaAnimWashColor, [&](const PanoramaComputedStyle& a, const PanoramaComputedStyle& b, float e) {
        computed.wash_color = lerp_color(a.wash_color, b.wash_color, e);
    });
    eval(PanoramaAnimTransform, [&](const PanoramaComputedStyle& a, const PanoramaComputedStyle& b, float e) {
        computed.transform = lerp_transform(a.transform, b.transform, e);
    });
    eval(PanoramaAnimPosition, [&](const PanoramaComputedStyle& a, const PanoramaComputedStyle& b, float e) {
        computed.has_position = true;
        computed.pos_x = a.pos_x + (b.pos_x - a.pos_x) * e;
        computed.pos_y = a.pos_y + (b.pos_y - a.pos_y) * e;
        computed.pos_z = a.pos_z + (b.pos_z - a.pos_z) * e;
        computed.pos_x_percent = a.pos_x_percent;
        computed.pos_y_percent = a.pos_y_percent;
    });
    eval(PanoramaAnimBoxShadow, [&](const PanoramaComputedStyle& a, const PanoramaComputedStyle& b, float e) {
        computed.box_shadow = lerp_box_shadow(a.box_shadow, b.box_shadow, e);
    });
    eval(PanoramaAnimBlur, [&](const PanoramaComputedStyle& a, const PanoramaComputedStyle& b, float e) {
        computed.blur = lerp_blur(a.blur, b.blur, e);
    });
    eval(PanoramaAnimClip, [&](const PanoramaComputedStyle& a, const PanoramaComputedStyle& b, float e) {
        computed.clip = lerp_clip(a.clip, b.clip, e);
    });
}

PanoramaAnimationAdvanceResult advance_keyframe_node(
    PanoramaNode& node, const std::unordered_map<std::string, PanoramaKeyframes>& registry, float dt)
{
    PanoramaAnimationAdvanceResult result;
    PanoramaNode::PanoramaKeyframeRuntime& runtime = node.keyframe_anim;
    const PanoramaComputedStyle& style = node.computed;
    const std::string& name = style.animation_name;

    if (name.empty())
    {
        if (!runtime.active_name.empty())
        {
            runtime = PanoramaNode::PanoramaKeyframeRuntime{};
            result.visual_changed = true; // the node reverts to its static cascade value
        }
    }
    else
    {
        const auto it = registry.find(name);
        if (it != registry.end() && !it->second.stops.empty())
        {
            const PanoramaKeyframes& keyframes = it->second;
            if (runtime.active_name != name)
            {
                runtime.active_name = name;
                runtime.elapsed = 0.0F;
                runtime.finished = false;
            }
            runtime.elapsed += dt;

            const float duration = style.animation_duration;
            const float local = runtime.elapsed - style.animation_delay;
            const float iterations = style.animation_iteration_count;
            const bool infinite = iterations < 0.0F;
            const PanoramaAnimDirection direction = style.animation_direction;
            const PanoramaAnimFillMode fill = style.animation_fill_mode;

            bool apply = false;
            bool active = true;
            float timeline = 0.0F;

            if (local < 0.0F)
            {
                // Delay phase: a backwards/both fill shows the first iteration's start.
                if (fill == PanoramaAnimFillMode::Backwards || fill == PanoramaAnimFillMode::Both)
                {
                    timeline = directional_position(0, 0.0F, direction);
                    apply = true;
                }
            }
            else if (duration <= 0.0F)
            {
                // Zero duration: snap to the end; forwards/both fills hold it.
                if (fill == PanoramaAnimFillMode::Forwards || fill == PanoramaAnimFillMode::Both)
                {
                    timeline = directional_position(0, 1.0F, direction);
                    apply = true;
                }
                runtime.finished = true;
                active = infinite;
            }
            else
            {
                const float total = local / duration;
                if (!infinite && total >= iterations)
                {
                    // Finished. Hold the final value only with a forwards/both fill.
                    runtime.finished = true;
                    active = false;
                    if (fill == PanoramaAnimFillMode::Forwards || fill == PanoramaAnimFillMode::Both)
                    {
                        long end_iteration = static_cast<long>(std::floor(iterations));
                        float end_frac = iterations - static_cast<float>(end_iteration);
                        if (end_frac <= 0.0F)
                        {
                            end_iteration -= 1; // integer count: end of the previous iteration
                            end_frac = 1.0F;
                        }
                        timeline = directional_position(end_iteration < 0 ? 0 : end_iteration, end_frac, direction);
                        apply = true;
                    }
                }
                else
                {
                    const long iteration = static_cast<long>(std::floor(total));
                    const float frac = total - static_cast<float>(iteration);
                    timeline = directional_position(iteration, frac, direction);
                    apply = true;
                }
            }

            if (apply)
            {
                apply_keyframes_at(node, keyframes, timeline, style.animation_easing);
                result.visual_changed = true;
                if ((keyframes.channels & PanoramaAnimPosition) != 0)
                {
                    result.layout_changed = true;
                }
            }
            result.active = active;
        }
        else
        {
            runtime.active_name = name; // known name, no usable keyframes: nothing to do
        }
    }

    for (const auto& child : node.children)
    {
        merge_result(result, advance_keyframe_node(*child, registry, dt));
    }
    return result;
}
}

PanoramaAnimationAdvanceResult panorama_advance_keyframes(
    PanoramaNode& root, const std::unordered_map<std::string, PanoramaKeyframes>& keyframes, float dt)
{
    return advance_keyframe_node(root, keyframes, dt);
}

namespace
{
// WebCore ScrollAnimationSmooth: a 1-D spring-mass system with critical damping
// per axis — d2x/dt2 + 2*zeta*omega*dx/dt + omega^2*(x - target) = 0 — solved
// with semi-implicit Euler at a fixed 4ms physics step (stable for any frame
// dt). omega approximates the iOS ScrollView 0.998 deceleration rate; zeta = 1
// is critical damping (fastest settle, no overshoot). Times are in
// milliseconds, matching WebCore's units (velocity is px/ms).
constexpr float kScrollSpringOmega = 0.022F; // stiffness (rad/ms), the WebCore non-Darwin value
constexpr float kScrollSpringZeta = 1.0F;    // damping ratio (>= 1 avoids overshoot)
constexpr float kScrollFixedStepMs = 4.0F;   // 250Hz physics
constexpr int kScrollMaxIterations = 400;    // safety cap: 400 * 4ms = 1.6s per frame
constexpr float kScrollPosEpsilon = 1.0F;    // px
constexpr float kScrollVelEpsilon = 0.02F;   // px/ms

PanoramaAnimationAdvanceResult advance_scroll_node(PanoramaNode& node, float dt_ms)
{
    PanoramaAnimationAdvanceResult result;
    auto& anim = node.scroll_anim;
    if (anim.active)
    {
        // Layout re-derives the scroll range every pass; a shrunk range pulls
        // the destination back in (WebCore updateScrollExtents).
        anim.dest_x = std::clamp(anim.dest_x, 0.0F, node.max_scroll_x);
        anim.dest_y = std::clamp(anim.dest_y, 0.0F, node.max_scroll_y);

        float x = node.scroll_offset_x;
        float y = node.scroll_offset_y;
        const float disp_before_x = x - anim.dest_x;
        const float disp_before_y = y - anim.dest_y;

        float elapsed = dt_ms;
        int iterations = 0;
        while (elapsed > 0.0F && iterations < kScrollMaxIterations)
        {
            const float step = std::min(elapsed, kScrollFixedStepMs);

            // 1. spring acceleration (critically-damped)
            const float ax = -kScrollSpringOmega * kScrollSpringOmega * (x - anim.dest_x) -
                2.0F * kScrollSpringZeta * kScrollSpringOmega * anim.velocity_x;
            const float ay = -kScrollSpringOmega * kScrollSpringOmega * (y - anim.dest_y) -
                2.0F * kScrollSpringZeta * kScrollSpringOmega * anim.velocity_y;

            // 2. integrate velocity, 3. integrate position (semi-implicit Euler)
            anim.velocity_x += ax * step;
            anim.velocity_y += ay * step;
            x += anim.velocity_x * step;
            y += anim.velocity_y * step;

            elapsed -= step;
            ++iterations;
        }

        // 4. stop on overshoot or once both position and velocity are within
        // tolerance — then snap to the destination.
        const float disp_after_x = x - anim.dest_x;
        const float disp_after_y = y - anim.dest_y;
        const auto sign_flip = [](float a, float b) { return (a > 0.0F) != (b > 0.0F); };
        const bool in_tolerance = std::fabs(disp_after_x) <= kScrollPosEpsilon &&
            std::fabs(disp_after_y) <= kScrollPosEpsilon && std::fabs(anim.velocity_x) <= kScrollVelEpsilon &&
            std::fabs(anim.velocity_y) <= kScrollVelEpsilon;
        if (sign_flip(disp_before_x, disp_after_x) || sign_flip(disp_before_y, disp_after_y) || in_tolerance)
        {
            x = anim.dest_x;
            y = anim.dest_y;
            anim.active = false;
            anim.velocity_x = 0.0F;
            anim.velocity_y = 0.0F;
        }
        else
        {
            result.active = true;
        }

        if (panorama_set_scroll_offset(node, x, y))
        {
            result.layout_changed = true;
            result.visual_changed = true;
        }
    }

    for (const auto& child : node.children)
    {
        merge_result(result, advance_scroll_node(*child, dt_ms));
    }
    return result;
}
}

PanoramaAnimationAdvanceResult panorama_advance_scroll_animations(PanoramaNode& root, float dt)
{
    return advance_scroll_node(root, dt * 1000.0F);
}
}
