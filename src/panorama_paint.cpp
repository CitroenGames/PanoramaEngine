#include "ui/panorama/panorama_paint.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <utility>

namespace openstrike
{
namespace
{
PanoramaColor scale_alpha(PanoramaColor color, float opacity)
{
    const float a = static_cast<float>(color.a) * std::clamp(opacity, 0.0F, 1.0F);
    color.a = static_cast<std::uint8_t>(std::clamp(a, 0.0F, 255.0F) + 0.5F);
    return color;
}

PanoramaColor texture_tint(const PanoramaComputedStyle& style, float opacity)
{
    const auto bright = [&](std::uint8_t c) {
        return static_cast<std::uint8_t>(std::clamp(static_cast<float>(c) * style.brightness, 0.0F, 255.0F) + 0.5F);
    };
    return scale_alpha({bright(style.wash_color.r), bright(style.wash_color.g), bright(style.wash_color.b), style.wash_color.a},
        opacity);
}

std::uint8_t lerp_channel(std::uint8_t a, std::uint8_t b, float t)
{
    return static_cast<std::uint8_t>(
        std::clamp(static_cast<float>(a) + (static_cast<float>(b) - static_cast<float>(a)) * t, 0.0F, 255.0F) + 0.5F);
}

// Gradient-stop interpolation in PREMULTIPLIED alpha, like WebCore's legacy
// -webkit-gradient rendering (GradientRendererCG interpolates premultiplied;
// gradientAlphaPremultiplication defaults on): a fade toward a transparent
// stop must not pull the colour toward the transparent stop's hue — white →
// rgba(0,0,0,0) stays white while fading instead of greying mid-ramp.
PanoramaColor lerp_color_premultiplied(PanoramaColor a, PanoramaColor b, float t)
{
    const float u = std::clamp(t, 0.0F, 1.0F);
    const float aa = static_cast<float>(a.a) / 255.0F;
    const float ba = static_cast<float>(b.a) / 255.0F;
    const float out_a = aa + (ba - aa) * u;
    if (out_a <= 0.0001F)
    {
        // Fully transparent: take the more-opaque endpoint's hue (the limit of
        // the premultiplied ramp) so the rasterizer's straight-alpha lerp
        // toward this vertex does not drag the visible side's hue with it —
        // WebCore replaces transparent stops with same-hue transparent for
        // exactly this reason (alphaTransformStopsToEmulateAlphaPremultiplication).
        const PanoramaColor& hue = aa >= ba ? a : b;
        return {hue.r, hue.g, hue.b, 0};
    }
    const auto channel = [&](std::uint8_t ca, std::uint8_t cb) {
        const float pa = static_cast<float>(ca) * aa;
        const float pb = static_cast<float>(cb) * ba;
        const float premultiplied = pa + (pb - pa) * u;
        return static_cast<std::uint8_t>(std::clamp(premultiplied / out_a, 0.0F, 255.0F) + 0.5F);
    };
    return {channel(a.r, b.r), channel(a.g, b.g), channel(a.b, b.b),
        static_cast<std::uint8_t>(std::clamp(out_a * 255.0F, 0.0F, 255.0F) + 0.5F)};
}

PanoramaColor gradient_color_at(const PanoramaGradient& gradient, float offset, float opacity)
{
    if (gradient.stops.empty())
    {
        return {};
    }
    const float t = std::clamp(offset, 0.0F, 1.0F);
    // Zero-width segments (duplicate-offset stops — the CS:GO
    // `from(transparent), color-stop(0, X)` trick) are hard transitions with no
    // interior, so only segments with positive width participate in sampling.
    // WebCore/skia sample gradients at pixel centres, which hit a zero-width stop
    // with probability zero; a per-vertex evaluator samples the exact endpoint
    // every time, and returning the zero-width side would smear that colour across
    // the whole triangle (e.g. a wedge over every hovered DropDown).
    for (std::size_t i = 1; i < gradient.stops.size(); ++i)
    {
        const PanoramaGradientStop& prev = gradient.stops[i - 1];
        const PanoramaGradientStop& next = gradient.stops[i];
        const float span = next.offset - prev.offset;
        if (span <= 0.0001F)
        {
            continue;
        }
        if (t <= prev.offset)
        {
            return scale_alpha(prev.color, opacity);
        }
        if (t <= next.offset)
        {
            return scale_alpha(lerp_color_premultiplied(prev.color, next.color, (t - prev.offset) / span), opacity);
        }
    }
    return scale_alpha(gradient.stops.back().color, opacity);
}

// Minimal UTF-8 decode: returns the next codepoint and advances `i`. Malformed
// bytes are emitted as U+FFFD so paint never desyncs from the byte stream.
char32_t next_codepoint(std::string_view text, std::size_t& i)
{
    const auto byte = [&](std::size_t k) { return static_cast<unsigned char>(text[k]); };
    const unsigned char lead = byte(i);
    if (lead < 0x80)
    {
        ++i;
        return lead;
    }
    int extra = 0;
    char32_t cp = 0;
    if ((lead & 0xE0) == 0xC0)
    {
        extra = 1;
        cp = lead & 0x1F;
    }
    else if ((lead & 0xF0) == 0xE0)
    {
        extra = 2;
        cp = lead & 0x0F;
    }
    else if ((lead & 0xF8) == 0xF0)
    {
        extra = 3;
        cp = lead & 0x07;
    }
    else
    {
        ++i;
        return 0xFFFD;
    }
    if (i + extra >= text.size())
    {
        i = text.size();
        return 0xFFFD;
    }
    for (int k = 0; k < extra; ++k)
    {
        const unsigned char cont = byte(i + 1 + static_cast<std::size_t>(k));
        if ((cont & 0xC0) != 0x80)
        {
            ++i;
            return 0xFFFD;
        }
        cp = (cp << 6) | (cont & 0x3F);
    }
    i += static_cast<std::size_t>(extra) + 1;
    return cp;
}

struct Matrix2D
{
    float a = 1.0F;
    float b = 0.0F;
    float c = 0.0F;
    float d = 1.0F;
    float e = 0.0F;
    float f = 0.0F;
};

Matrix2D multiply(const Matrix2D& lhs, const Matrix2D& rhs)
{
    return {
        lhs.a * rhs.a + lhs.c * rhs.b,
        lhs.b * rhs.a + lhs.d * rhs.b,
        lhs.a * rhs.c + lhs.c * rhs.d,
        lhs.b * rhs.c + lhs.d * rhs.d,
        lhs.a * rhs.e + lhs.c * rhs.f + lhs.e,
        lhs.b * rhs.e + lhs.d * rhs.f + lhs.f,
    };
}

Matrix2D translation(float x, float y)
{
    Matrix2D m;
    m.e = x;
    m.f = y;
    return m;
}

Matrix2D scale(float x, float y)
{
    Matrix2D m;
    m.a = x;
    m.d = y;
    return m;
}

Matrix2D rotation(float degrees)
{
    constexpr float kPi = 3.1415926535F;
    const float radians = degrees * kPi / 180.0F;
    const float cs = std::cos(radians);
    const float sn = std::sin(radians);
    Matrix2D m;
    m.a = cs;
    m.b = sn;
    m.c = -sn;
    m.d = cs;
    return m;
}

PanoramaPaintVertex transform_vertex(float x, float y, float u, float v, PanoramaColor color, const Matrix2D& matrix)
{
    return {
        matrix.a * x + matrix.c * y + matrix.e,
        matrix.b * x + matrix.d * y + matrix.f,
        u,
        v,
        color,
    };
}

// Maps a layout-space rect through the accumulated transform and returns its
// axis-aligned bounds. The engine's clips are framebuffer scissors, so the
// transformed clip must be re-expressed axis-aligned: exact for the
// translate/scale transforms the menus animate with, a conservative bounding
// box under rotation. WebCore concatenates the transform into the CTM before
// the descendant clips apply (RenderLayer::paintLayerByApplyingTransform), so
// a sliding panel's overflow clip travels with it — an untransformed scissor
// would crop slide-in panels at their resting position.
void map_rect_through(const Matrix2D& m, float& x0, float& y0, float& x1, float& y1)
{
    const float xs[4] = {
        m.a * x0 + m.c * y0 + m.e,
        m.a * x1 + m.c * y0 + m.e,
        m.a * x1 + m.c * y1 + m.e,
        m.a * x0 + m.c * y1 + m.e,
    };
    const float ys[4] = {
        m.b * x0 + m.d * y0 + m.f,
        m.b * x1 + m.d * y0 + m.f,
        m.b * x1 + m.d * y1 + m.f,
        m.b * x0 + m.d * y1 + m.f,
    };
    x0 = std::min(std::min(xs[0], xs[1]), std::min(xs[2], xs[3]));
    x1 = std::max(std::max(xs[0], xs[1]), std::max(xs[2], xs[3]));
    y0 = std::min(std::min(ys[0], ys[1]), std::min(ys[2], ys[3]));
    y1 = std::max(std::max(ys[0], ys[1]), std::max(ys[2], ys[3]));
}

bool is_identity(const Matrix2D& m)
{
    return m.a == 1.0F && m.b == 0.0F && m.c == 0.0F && m.d == 1.0F && m.e == 0.0F && m.f == 0.0F;
}

Matrix2D node_transform_matrix(const PanoramaNode& node, const Matrix2D& parent)
{
    const PanoramaComputedStyle& s = node.computed;
    const bool has_pre_scale = s.pre_scale_x != 1.0F || s.pre_scale_y != 1.0F;
    if (s.transform.empty() && !has_pre_scale)
    {
        return parent;
    }

    const PanoramaLayoutBox& box = node.layout;
    const float origin_x = box.x + (s.transform_origin.x_percent ? box.width * s.transform_origin.x / 100.0F
                                                                  : s.transform_origin.x);
    const float origin_y = box.y + (s.transform_origin.y_percent ? box.height * s.transform_origin.y / 100.0F
                                                                  : s.transform_origin.y);
    Matrix2D local = translation(origin_x, origin_y);
    for (const PanoramaTransformOp& op : s.transform.ops)
    {
        Matrix2D op_matrix;
        switch (op.type)
        {
        case PanoramaTransformOp::Type::Translate:
            op_matrix = translation(op.x_percent ? box.width * op.x / 100.0F : op.x,
                op.y_percent ? box.height * op.y / 100.0F : op.y);
            break;
        case PanoramaTransformOp::Type::Scale:
            op_matrix = scale(op.x, op.y);
            break;
        case PanoramaTransformOp::Type::Rotate:
            op_matrix = rotation(op.x);
            break;
        }
        local = multiply(local, op_matrix);
    }
    if (has_pre_scale)
    {
        // Applied before `transform` (innermost), about the same origin.
        local = multiply(local, scale(s.pre_scale_x, s.pre_scale_y));
    }
    local = multiply(local, translation(-origin_x, -origin_y));
    return multiply(parent, local);
}

// Dest rectangle + texture coordinates for a background image, computed from
// background-size / background-position and the texture's natural aspect. `cover`
// (image larger than the box on an axis) crops via the uv range; `contain`/small
// `fixed` (image smaller) draws a positioned sub-rect with uv 0..1.
// Resolves CSS background-size into the drawn size of ONE tile (WebCore
// BackgroundPainter::calculateFillTileSize equivalent).
void compute_background_tile_size(float box_w, float box_h, float aspect, const PanoramaBackgroundSize& size,
    float& drawn_w, float& drawn_h)
{
    drawn_w = box_w;
    drawn_h = box_h;
    if (box_w <= 0.0F || box_h <= 0.0F)
    {
        return;
    }

    const float box_aspect = box_w / box_h;
    switch (size.type)
    {
    case PanoramaBackgroundSizeType::Stretch:
        break;
    case PanoramaBackgroundSizeType::Contain:
        if (aspect > 0.0F)
        {
            if (box_aspect > aspect)
            {
                drawn_h = box_h;
                drawn_w = box_h * aspect;
            }
            else
            {
                drawn_w = box_w;
                drawn_h = box_w / aspect;
            }
        }
        break;
    case PanoramaBackgroundSizeType::Cover:
        if (aspect > 0.0F)
        {
            if (box_aspect > aspect)
            {
                drawn_w = box_w;
                drawn_h = box_w / aspect;
            }
            else
            {
                drawn_h = box_h;
                drawn_w = box_h * aspect;
            }
        }
        break;
    case PanoramaBackgroundSizeType::Fixed:
    {
        const auto resolve = [](const PanoramaLength& length, float basis) -> float {
            if (length.type == PanoramaLengthType::Pixels)
            {
                return length.value;
            }
            if (length.type == PanoramaLengthType::Percent)
            {
                return basis * length.value / 100.0F;
            }
            return -1.0F; // auto: derive from aspect
        };
        const float fixed_w = resolve(size.width, box_w);
        const float fixed_h = resolve(size.height, box_h);
        if (fixed_w < 0.0F && fixed_h < 0.0F)
        {
            drawn_w = box_w;
            drawn_h = box_h;
        }
        else if (fixed_w < 0.0F)
        {
            drawn_h = fixed_h;
            drawn_w = aspect > 0.0F ? fixed_h * aspect : box_w;
        }
        else if (fixed_h < 0.0F)
        {
            drawn_w = fixed_w;
            drawn_h = aspect > 0.0F ? fixed_w / aspect : box_h;
        }
        else
        {
            drawn_w = fixed_w;
            drawn_h = fixed_h;
        }
        break;
    }
    }
}

// Resolves one axis of background-position to the tile's box-relative offset.
float background_axis_offset(float drawn, float box, bool percent, bool from_end, bool side_offset, float value)
{
    if (side_offset)
    {
        const float resolved = percent ? box * value / 100.0F : value;
        return from_end ? box - drawn - resolved : resolved;
    }
    if (percent)
    {
        return (box - drawn) * (value / 100.0F);
    }
    return from_end ? box - drawn - value : value;
}

// One axis of the background tile grid: `count` tiles `tile` wide, the first
// starting at `start` (box-relative; may be negative = cropped by the box),
// each `step` apart (step > tile only for `space`). Semantics follow WebCore
// BackgroundPainter::calculateBackgroundImageGeometry.
struct BackgroundAxisTiles
{
    float start = 0.0F;
    float step = 0.0F;
    float tile = 0.0F;
    int count = 1;
};

BackgroundAxisTiles compute_background_axis_tiles(PanoramaBackgroundRepeat mode, float box, float tile, float anchor)
{
    BackgroundAxisTiles out{anchor, tile, tile, 1};
    if (tile <= 0.01F || box <= 0.0F)
    {
        return out;
    }

    if (mode == PanoramaBackgroundRepeat::Round)
    {
        // Rescale the tile so a whole number fits exactly, then tile like repeat.
        const int num = std::max(1, static_cast<int>(std::lround(box / tile)));
        out.tile = out.step = box / static_cast<float>(num);
    }
    else if (mode == PanoramaBackgroundRepeat::Space)
    {
        const int num = static_cast<int>(box / tile);
        if (num > 1)
        {
            // Whole tiles pinned to both box edges, the leftover split into
            // equal gaps; background-position is ignored on a spaced axis.
            out.start = 0.0F;
            out.step = tile + (box - static_cast<float>(num) * tile) / static_cast<float>(num - 1);
            out.count = num;
            return out;
        }
        mode = PanoramaBackgroundRepeat::NoRepeat; // no room for two tiles
    }

    if (mode == PanoramaBackgroundRepeat::NoRepeat)
    {
        return out;
    }

    // Repeat (and the resized round): the grid is anchored so one tile edge
    // lands at the computed position, extended across the whole box. The
    // first tile starts in (-step, 0] so the box is covered from its edge.
    float first = std::fmod(out.start, out.step);
    if (first > 0.0F)
    {
        first -= out.step;
    }
    out.start = first;
    out.count = std::min(1024, static_cast<int>(std::ceil((box - first) / out.step)));
    return out;
}

// An axis-aligned clip rectangle in design space; `active == false` means unclipped.
struct ClipRect
{
    bool active = false;
    float x0 = 0.0F;
    float y0 = 0.0F;
    float x1 = 0.0F;
    float y1 = 0.0F;
};

constexpr float kClipInfinity = 1000000.0F;

ClipRect intersect_clip(const ClipRect& current, float x0, float y0, float x1, float y1)
{
    if (!current.active)
    {
        return {true, x0, y0, x1, y1};
    }
    return {true, std::max(current.x0, x0), std::max(current.y0, y0), std::min(current.x1, x1), std::min(current.y1, y1)};
}

ClipRect intersect_axis_clip(const ClipRect& current, float x0, float y0, float x1, float y1, bool clip_x, bool clip_y)
{
    if (!clip_x && !clip_y)
    {
        return current;
    }

    if (!clip_x)
    {
        x0 = current.active ? current.x0 : -kClipInfinity;
        x1 = current.active ? current.x1 : kClipInfinity;
    }
    if (!clip_y)
    {
        y0 = current.active ? current.y0 : -kClipInfinity;
        y1 = current.active ? current.y1 : kClipInfinity;
    }
    return intersect_clip(current, x0, y0, x1, y1);
}

ClipRect overflow_content_clip(const PanoramaNode& node, const ClipRect& current, const Matrix2D& matrix)
{
    const PanoramaLayoutBox& box = node.layout;
    const PanoramaComputedStyle& style = node.computed;
    const bool clip_x = style.overflow_clip_x;
    const bool clip_y = style.overflow_clip_y;
    float x0 = box.x + std::max(0.0F, style.border_left());
    float y0 = box.y + std::max(0.0F, style.border_top());
    float x1 = box.x + std::max(0.0F, box.width - std::max(0.0F, style.border_right()));
    float y1 = box.y + std::max(0.0F, box.height - std::max(0.0F, style.border_bottom()));
    if (!is_identity(matrix))
    {
        // The children's pixels are emitted through this matrix; the scissor
        // must clip in the same (screen) space or a translated subtree gets
        // cropped at its untransformed position.
        map_rect_through(matrix, x0, y0, x1, y1);
    }
    return intersect_axis_clip(current, x0, y0, x1, y1, clip_x, clip_y);
}

// Per-corner border radii in CSS corner order (WebCore FloatRoundedRect::Radii).
// All paint-side rounded geometry takes one of these; a uniform radius is just
// four equal corners.
struct CornerRadii
{
    float tl = 0.0F;
    float tr = 0.0F;
    float br = 0.0F;
    float bl = 0.0F;

    [[nodiscard]] bool any() const { return tl > 0.5F || tr > 0.5F || br > 0.5F || bl > 0.5F; }
    [[nodiscard]] float max_radius() const { return std::max(std::max(tl, tr), std::max(br, bl)); }

    // WebCore Radii::expand: only already-rounded corners grow (a sharp corner
    // stays sharp under spread), floored at 0.
    [[nodiscard]] CornerRadii expanded(float d) const
    {
        const auto e = [d](float r) { return r > 0.0F ? std::max(0.0F, r + d) : 0.0F; };
        return {e(tl), e(tr), e(br), e(bl)};
    }

    // Uniform shrink on every corner, floored at 0 (inner border / blur core).
    [[nodiscard]] CornerRadii shrunk(float d) const
    {
        const auto s = [d](float r) { return std::max(0.0F, r - d); };
        return {s(tl), s(tr), s(br), s(bl)};
    }

    // Uniform grow on every corner — the outer isophote of a blurred fill is
    // rounded even where the fill is sharp.
    [[nodiscard]] CornerRadii grown(float d) const { return {tl + d, tr + d, br + d, bl + d}; }

    // WebCore FloatRoundedRect calcConstraintScaleFor / RenderStyle
    // getRoundedBorderFor: if any edge's adjacent radii overlap, scale ALL radii
    // by the smallest factor that fits, preserving corner proportions.
    [[nodiscard]] CornerRadii constrained(float w, float h) const
    {
        w = std::max(w, 0.0F);
        h = std::max(h, 0.0F);
        float factor = 1.0F;
        const auto consider = [&factor](float sum, float edge) {
            if (sum > edge && sum > 0.0F)
            {
                factor = std::min(factor, edge / sum);
            }
        };
        consider(tl + tr, w);
        consider(bl + br, w);
        consider(tl + bl, h);
        consider(tr + br, h);
        if (factor >= 1.0F)
        {
            return {std::max(0.0F, tl), std::max(0.0F, tr), std::max(0.0F, br), std::max(0.0F, bl)};
        }
        return {std::max(0.0F, tl * factor), std::max(0.0F, tr * factor), std::max(0.0F, br * factor),
            std::max(0.0F, bl * factor)};
    }
};

constexpr CornerRadii uniform_radii(float r)
{
    return {r, r, r, r};
}

// Appends the boundary of a rounded rect as 4 corner arcs swept clockwise
// (`segments` steps each => 4*(segments+1) points). Two contours generated with
// the same `segments` share a parameterization, so DrawListBuilder::add_rounded_ring
// can stitch them 1:1 into a band. Degenerate sizes collapse cleanly (a zero-size
// rect yields a point contour).
void rounded_contour_points(
    float x, float y, float w, float h, const CornerRadii& radii, int segments, std::vector<std::pair<float, float>>& out)
{
    w = std::max(w, 0.0F);
    h = std::max(h, 0.0F);
    const CornerRadii r = radii.constrained(w, h);
    constexpr float kPi = 3.1415926535F;
    const float corners[4][4] = {
        {x + r.tl, y + r.tl, kPi, r.tl},                 // top-left:     180 -> 270
        {x + w - r.tr, y + r.tr, 1.5F * kPi, r.tr},      // top-right:    270 -> 360
        {x + w - r.br, y + h - r.br, 0.0F, r.br},        // bottom-right:   0 ->  90
        {x + r.bl, y + h - r.bl, 0.5F * kPi, r.bl},      // bottom-left:   90 -> 180
    };
    out.reserve(out.size() + static_cast<std::size_t>(4 * (segments + 1)));
    for (const auto& corner : corners)
    {
        for (int s = 0; s <= segments; ++s)
        {
            const float a = corner[2] + 0.5F * kPi * (static_cast<float>(s) / static_cast<float>(segments));
            out.emplace_back(corner[0] + corner[3] * std::cos(a), corner[1] + corner[3] * std::sin(a));
        }
    }
}

// ---- clip: radial(...) wedge clipping -----------------------------------------
//
// `clip: radial(cx cy, start, sweep)` hides the wedge swept clockwise from
// `start` (0deg = 12 o'clock) over `sweep` degrees about the centre. The VISIBLE
// region is the complementary wedge, decomposed into <= 2 convex wedges (each
// <= 180deg = the intersection of two half-planes through the centre). Painted
// geometry is post-processed: every emitted triangle is Sutherland-Hodgman
// clipped against each wedge's half-planes (positions, uvs and colours lerp).

// Half-plane through (cx,cy) with normal (nx,ny); keeps P with (P-C)·n >= 0.
struct WedgeHalfPlane
{
    float cx = 0.0F;
    float cy = 0.0F;
    float nx = 0.0F;
    float ny = 0.0F;

    [[nodiscard]] float distance(const PanoramaPaintVertex& v) const
    {
        return (v.x - cx) * nx + (v.y - cy) * ny;
    }
};

using WedgePlanes = std::array<WedgeHalfPlane, 2>;

PanoramaPaintVertex lerp_paint_vertex(const PanoramaPaintVertex& a, const PanoramaPaintVertex& b, float t)
{
    const auto lerp_channel = [t](std::uint8_t x, std::uint8_t y) {
        const float v = static_cast<float>(x) + (static_cast<float>(y) - static_cast<float>(x)) * t;
        return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F) + 0.5F);
    };
    PanoramaPaintVertex out;
    out.x = a.x + (b.x - a.x) * t;
    out.y = a.y + (b.y - a.y) * t;
    out.u = a.u + (b.u - a.u) * t;
    out.v = a.v + (b.v - a.v) * t;
    out.color.r = lerp_channel(a.color.r, b.color.r);
    out.color.g = lerp_channel(a.color.g, b.color.g);
    out.color.b = lerp_channel(a.color.b, b.color.b);
    out.color.a = lerp_channel(a.color.a, b.color.a);
    return out;
}

// Clips `poly` in place against one half-plane (Sutherland-Hodgman step).
void clip_polygon_against_plane(
    std::vector<PanoramaPaintVertex>& poly, std::vector<PanoramaPaintVertex>& scratch, const WedgeHalfPlane& plane)
{
    scratch.clear();
    const std::size_t n = poly.size();
    for (std::size_t i = 0; i < n; ++i)
    {
        const PanoramaPaintVertex& cur = poly[i];
        const PanoramaPaintVertex& nxt = poly[(i + 1) % n];
        const float dc = plane.distance(cur);
        const float dn = plane.distance(nxt);
        if (dc >= 0.0F)
        {
            scratch.push_back(cur);
        }
        if ((dc >= 0.0F) != (dn >= 0.0F))
        {
            const float denom = dc - dn;
            const float t = denom != 0.0F ? dc / denom : 0.0F;
            scratch.push_back(lerp_paint_vertex(cur, nxt, t));
        }
    }
    poly.swap(scratch);
}

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

class DrawListBuilder
{
public:
    DrawListBuilder(PanoramaDrawList& list, const PanoramaGlyphSource& glyphs, PanoramaPaintScratch* scratch)
        : list_(list), glyphs_(glyphs), scratch_(scratch)
    {
    }

    // The document root, used to resolve CSGOBlurTarget `blurrects` names (ids or
    // classes of overlay panels) to live layout boxes at paint time.
    void set_document_root(const PanoramaNode* root) { document_root_ = root; }

    void set_clip(const ClipRect& clip) { clip_ = clip; }
    void set_blend(PanoramaBlendMode blend) { blend_ = blend; }

    void add_rect(float x, float y, float w, float h, PanoramaColor color, PanoramaTextureId texture,
        const Matrix2D& matrix,
        float u0 = 0.0F, float v0 = 0.0F, float u1 = 1.0F, float v1 = 1.0F)
    {
        if (w <= 0.0F || h <= 0.0F || color.a == 0)
        {
            return;
        }
        PanoramaDrawCommand& cmd = current_command(texture);
        const int base = static_cast<int>(cmd.vertices.size());
        cmd.vertices.push_back(transform_vertex(x, y, u0, v0, color, matrix));
        cmd.vertices.push_back(transform_vertex(x + w, y, u1, v0, color, matrix));
        cmd.vertices.push_back(transform_vertex(x + w, y + h, u1, v1, color, matrix));
        cmd.vertices.push_back(transform_vertex(x, y + h, u0, v1, color, matrix));
        cmd.indices.push_back(base + 0);
        cmd.indices.push_back(base + 1);
        cmd.indices.push_back(base + 2);
        cmd.indices.push_back(base + 0);
        cmd.indices.push_back(base + 2);
        cmd.indices.push_back(base + 3);
    }

    // Emits a rounded rectangle as a center-fan with tessellated corner arcs, with a
    // per-vertex colour callback (used by gradient fills: a linear gradient is an
    // affine function of position, so per-vertex evaluation is exact on triangles).
    // Texture coordinates are mapped linearly across the rect so a textured rounded
    // fill (e.g. a circular avatar) clips to the shape. Falls back to a sharp quad
    // (still per-vertex shaded) for tiny radii.
    template <typename ColorAt>
    void add_rounded_rect_shaded(float x, float y, float w, float h, const CornerRadii& radii,
        PanoramaTextureId texture, const Matrix2D& matrix, ColorAt&& color_at,
        float u0 = 0.0F, float v0 = 0.0F, float u1 = 1.0F, float v1 = 1.0F)
    {
        if (w <= 0.0F || h <= 0.0F)
        {
            return;
        }
        PanoramaDrawCommand& cmd = current_command(texture);
        const auto push = [&](float px, float py) {
            const float u = u0 + (px - x) / w * (u1 - u0);
            const float v = v0 + (py - y) / h * (v1 - v0);
            cmd.vertices.push_back(transform_vertex(px, py, u, v, color_at(px, py), matrix));
        };

        const CornerRadii r = radii.constrained(w, h);
        if (!r.any())
        {
            const int base = static_cast<int>(cmd.vertices.size());
            push(x, y);
            push(x + w, y);
            push(x + w, y + h);
            push(x, y + h);
            cmd.indices.push_back(base + 0);
            cmd.indices.push_back(base + 1);
            cmd.indices.push_back(base + 2);
            cmd.indices.push_back(base + 0);
            cmd.indices.push_back(base + 2);
            cmd.indices.push_back(base + 3);
            return;
        }

        constexpr float kPi = 3.1415926535F;
        const int segments = std::clamp(static_cast<int>(std::ceil(r.max_radius() / 2.0F)), 2, 16);
        const int center_index = static_cast<int>(cmd.vertices.size());

        push(x + w * 0.5F, y + h * 0.5F); // fan center

        // Four corner centers, each swept 90 degrees clockwise (screen y-down),
        // each with its own radius (sharp corners degenerate to a point arc).
        const float corners[4][4] = {
            {x + r.tl, y + r.tl, kPi, r.tl},                 // top-left:     180 -> 270
            {x + w - r.tr, y + r.tr, 1.5F * kPi, r.tr},      // top-right:    270 -> 360
            {x + w - r.br, y + h - r.br, 0.0F, r.br},        // bottom-right:   0 ->  90
            {x + r.bl, y + h - r.bl, 0.5F * kPi, r.bl},      // bottom-left:   90 -> 180
        };
        int ring_count = 0;
        for (const auto& corner : corners)
        {
            for (int s = 0; s <= segments; ++s)
            {
                const float a = corner[2] + 0.5F * kPi * (static_cast<float>(s) / static_cast<float>(segments));
                push(corner[0] + corner[3] * std::cos(a), corner[1] + corner[3] * std::sin(a));
                ++ring_count;
            }
        }

        const int first_ring = center_index + 1;
        for (int s = 0; s < ring_count; ++s)
        {
            cmd.indices.push_back(center_index);
            cmd.indices.push_back(first_ring + s);
            cmd.indices.push_back(first_ring + (s + 1) % ring_count);
        }
    }

    template <typename ColorAt>
    void add_rounded_rect_shaded(float x, float y, float w, float h, float radius,
        PanoramaTextureId texture, const Matrix2D& matrix, ColorAt&& color_at,
        float u0 = 0.0F, float v0 = 0.0F, float u1 = 1.0F, float v1 = 1.0F)
    {
        add_rounded_rect_shaded(x, y, w, h, uniform_radii(radius), texture, matrix,
            std::forward<ColorAt>(color_at), u0, v0, u1, v1);
    }

    void add_rounded_rect(float x, float y, float w, float h, const CornerRadii& radii, PanoramaColor color,
        PanoramaTextureId texture, const Matrix2D& matrix,
        float u0 = 0.0F, float v0 = 0.0F, float u1 = 1.0F, float v1 = 1.0F)
    {
        if (color.a == 0)
        {
            return;
        }
        add_rounded_rect_shaded(
            x, y, w, h, radii, texture, matrix, [color](float, float) { return color; }, u0, v0, u1, v1);
    }

    void add_rounded_rect(float x, float y, float w, float h, float radius, PanoramaColor color,
        PanoramaTextureId texture, const Matrix2D& matrix,
        float u0 = 0.0F, float v0 = 0.0F, float u1 = 1.0F, float v1 = 1.0F)
    {
        add_rounded_rect(x, y, w, h, uniform_radii(radius), color, texture, matrix, u0, v0, u1, v1);
    }

    // A regular cols x rows vertex grid over [x,y,w,h] with a per-vertex colour
    // callback. Used for gradient fields that are NOT affine in position (radial
    // distance, clamped oblique linear): the grid keeps the per-triangle linear
    // interpolation error small without needing a texture.
    template <typename ColorAt>
    void add_gradient_grid(float x, float y, float w, float h, const Matrix2D& matrix, ColorAt&& color_at)
    {
        if (w <= 0.0F || h <= 0.0F)
        {
            return;
        }
        const int cols = std::clamp(static_cast<int>(std::ceil(w / 8.0F)), 4, 64);
        const int rows = std::clamp(static_cast<int>(std::ceil(h / 8.0F)), 4, 64);
        PanoramaDrawCommand& cmd = current_command(0);
        const int base = static_cast<int>(cmd.vertices.size());
        for (int j = 0; j <= rows; ++j)
        {
            const float fy = static_cast<float>(j) / static_cast<float>(rows);
            const float py = y + h * fy;
            for (int i = 0; i <= cols; ++i)
            {
                const float fx = static_cast<float>(i) / static_cast<float>(cols);
                const float px = x + w * fx;
                cmd.vertices.push_back(transform_vertex(px, py, fx, fy, color_at(px, py), matrix));
            }
        }
        const int stride = cols + 1;
        for (int j = 0; j < rows; ++j)
        {
            for (int i = 0; i < cols; ++i)
            {
                const int tl = base + j * stride + i;
                cmd.indices.push_back(tl);
                cmd.indices.push_back(tl + 1);
                cmd.indices.push_back(tl + stride + 1);
                cmd.indices.push_back(tl);
                cmd.indices.push_back(tl + stride + 1);
                cmd.indices.push_back(tl + stride);
            }
        }
    }

    // A stitched band between two rounded-rect contours (inner colour at the inner
    // contour, outer colour at the outer), used for box-shadow falloff that follows
    // the border shape. The inner contour must lie inside the outer one.
    void add_rounded_ring(float ix, float iy, float iw, float ih, const CornerRadii& iradii, PanoramaColor inner_color,
        float ox, float oy, float ow, float oh, const CornerRadii& oradii, PanoramaColor outer_color,
        const Matrix2D& matrix)
    {
        if (inner_color.a == 0 && outer_color.a == 0)
        {
            return;
        }
        const int segments =
            std::clamp(static_cast<int>(std::ceil(std::max(iradii.max_radius(), oradii.max_radius()) / 2.0F)), 2, 16);
        std::vector<std::pair<float, float>> inner_pts;
        std::vector<std::pair<float, float>> outer_pts;
        rounded_contour_points(ix, iy, iw, ih, iradii, segments, inner_pts);
        rounded_contour_points(ox, oy, ow, oh, oradii, segments, outer_pts);

        PanoramaDrawCommand& cmd = current_command(0);
        const int base = static_cast<int>(cmd.vertices.size());
        for (const auto& p : inner_pts)
        {
            cmd.vertices.push_back(transform_vertex(p.first, p.second, 0.0F, 0.0F, inner_color, matrix));
        }
        for (const auto& p : outer_pts)
        {
            cmd.vertices.push_back(transform_vertex(p.first, p.second, 0.0F, 0.0F, outer_color, matrix));
        }
        const int n = static_cast<int>(inner_pts.size());
        for (int i = 0; i < n; ++i)
        {
            const int j = (i + 1) % n;
            cmd.indices.push_back(base + i);
            cmd.indices.push_back(base + n + i);
            cmd.indices.push_back(base + n + j);
            cmd.indices.push_back(base + i);
            cmd.indices.push_back(base + n + j);
            cmd.indices.push_back(base + j);
        }
    }

    // Draws a node's background texture into the box [bx,by,bw,bh] honoring
    // background-size / background-position / background-repeat (cover crops
    // via uv; contain letterboxes; repeat tiles the box, WebCore
    // BackgroundPainter semantics).
    void emit_background_image(const PanoramaNode& node, float bx, float by, float bw, float bh,
        const CornerRadii& radii, PanoramaColor tint, const Matrix2D& matrix)
    {
        if (bw <= 0.0F || bh <= 0.0F)
        {
            return;
        }
        const PanoramaComputedStyle& s = node.computed;
        float tile_w = bw;
        float tile_h = bh;
        compute_background_tile_size(bw, bh, node.background_texture_aspect, s.background_size, tile_w, tile_h);
        if (tile_w <= 0.01F || tile_h <= 0.01F)
        {
            return;
        }

        const PanoramaBackgroundPosition& pos = s.background_position;
        const float ax = background_axis_offset(tile_w, bw, pos.x_percent, pos.x_from_end, pos.x_side_offset, pos.x);
        const float ay = background_axis_offset(tile_h, bh, pos.y_percent, pos.y_from_end, pos.y_side_offset, pos.y);

        const PanoramaBackgroundRepeatXY repeat = s.background_repeat;
        const BackgroundAxisTiles tx = compute_background_axis_tiles(repeat.x, bw, tile_w, ax);
        // WebCore: rounding one axis rescales an aspect-derived other axis so
        // the image keeps its proportions.
        if (repeat.x == PanoramaBackgroundRepeat::Round && repeat.y != PanoramaBackgroundRepeat::Round &&
            s.background_size.type == PanoramaBackgroundSizeType::Fixed &&
            s.background_size.height.type == PanoramaLengthType::Auto && tile_w > 0.0F)
        {
            tile_h *= tx.tile / tile_w;
        }
        const BackgroundAxisTiles ty = compute_background_axis_tiles(repeat.y, bh, tile_h, ay);

        // A single tile keeps the node's rounded corners (this is the default
        // stretch background and every explicit no-repeat). Actual tiling
        // paints square corners — same approximation as radial gradients;
        // real content does not combine tiling with border-radius.
        const bool single = tx.count == 1 && ty.count == 1;
        const CornerRadii tile_radii = single ? radii : CornerRadii{};
        for (int iy = 0; iy < ty.count; ++iy)
        {
            const float y = ty.start + static_cast<float>(iy) * ty.step;
            const float cy0 = std::max(y, 0.0F);
            const float cy1 = std::min(y + ty.tile, bh);
            if (cy1 <= cy0)
            {
                continue;
            }
            const float v0 = (cy0 - y) / ty.tile;
            const float v1 = (cy1 - y) / ty.tile;
            for (int ix = 0; ix < tx.count; ++ix)
            {
                const float x = tx.start + static_cast<float>(ix) * tx.step;
                const float cx0 = std::max(x, 0.0F);
                const float cx1 = std::min(x + tx.tile, bw);
                if (cx1 <= cx0)
                {
                    continue;
                }
                const float u0 = (cx0 - x) / tx.tile;
                const float u1 = (cx1 - x) / tx.tile;
                add_rounded_rect(bx + cx0, by + cy0, cx1 - cx0, cy1 - cy0, tile_radii, tint, node.background_texture,
                    matrix, u0, v0, u1, v1);
            }
        }
    }

    // A solid-fill quad with an independent colour at each corner (for gradient edges).
    void add_gradient_quad(float x, float y, float w, float h, PanoramaColor c_tl, PanoramaColor c_tr,
        PanoramaColor c_br, PanoramaColor c_bl, const Matrix2D& matrix)
    {
        if (w <= 0.0F || h <= 0.0F || (c_tl.a == 0 && c_tr.a == 0 && c_br.a == 0 && c_bl.a == 0))
        {
            return;
        }
        PanoramaDrawCommand& cmd = current_command(0);
        const int base = static_cast<int>(cmd.vertices.size());
        cmd.vertices.push_back(transform_vertex(x, y, 0.0F, 0.0F, c_tl, matrix));
        cmd.vertices.push_back(transform_vertex(x + w, y, 1.0F, 0.0F, c_tr, matrix));
        cmd.vertices.push_back(transform_vertex(x + w, y + h, 1.0F, 1.0F, c_br, matrix));
        cmd.vertices.push_back(transform_vertex(x, y + h, 0.0F, 1.0F, c_bl, matrix));
        cmd.indices.push_back(base + 0);
        cmd.indices.push_back(base + 1);
        cmd.indices.push_back(base + 2);
        cmd.indices.push_back(base + 0);
        cmd.indices.push_back(base + 2);
        cmd.indices.push_back(base + 3);
    }

    bool emit_background_gradient(const PanoramaGradient& gradient, float x, float y, float w, float h,
        const CornerRadii& radii, float opacity, const Matrix2D& matrix)
    {
        if (w <= 0.0F || h <= 0.0F)
        {
            return false;
        }
        // A gradient whose every stop is fully transparent paints nothing; skip it
        // so the grid/fan paths don't emit thousands of invisible vertices.
        bool any_visible_stop = false;
        for (const PanoramaGradientStop& stop : gradient.stops)
        {
            if (stop.color.a != 0)
            {
                any_visible_stop = true;
                break;
            }
        }
        if (!any_visible_stop)
        {
            return true;
        }
        if (gradient.type == PanoramaGradientType::Radial)
        {
            // Ellipse centred at center+offset with radii rx/ry (all % of the box);
            // t is the normalized elliptical distance. Painted as a vertex grid (the
            // field is not affine, so a plain quad would interpolate it wrongly).
            // A border-radius is not carved out here (the gradient paints square
            // corners); CS:GO's radial fills are not corner-rounded in practice.
            const float cx = x + w * (gradient.radial_center_x + gradient.radial_offset_x) / 100.0F;
            const float cy = y + h * (gradient.radial_center_y + gradient.radial_offset_y) / 100.0F;
            const float rx = w * gradient.radial_radius_x / 100.0F;
            const float ry = h * gradient.radial_radius_y / 100.0F;
            add_gradient_grid(x, y, w, h, matrix, [&](float px, float py) {
                // Degenerate (zero) radii: everything is outside the ellipse.
                if (rx <= 0.0001F || ry <= 0.0001F)
                {
                    return gradient_color_at(gradient, 1.0F, opacity);
                }
                const float nx = (px - cx) / rx;
                const float ny = (py - cy) / ry;
                return gradient_color_at(gradient, std::sqrt(nx * nx + ny * ny), opacity);
            });
            return true;
        }
        if (gradient.type != PanoramaGradientType::Linear)
        {
            return false;
        }

        const float dx = gradient.x1 - gradient.x0;
        const float dy = gradient.y1 - gradient.y0;
        const bool axis_aligned = std::fabs(dx) < 0.001F || std::fabs(dy) < 0.001F;
        const float len_sq = dx * dx + dy * dy;
        if (len_sq < 0.0001F)
        {
            // Coincident points: CoreGraphics draws nothing for a zero-length
            // legacy gradient (Gradient::isZeroSize exists for exactly this), and
            // shipped CS:GO sheets rely on it (.IconButton's `50% 0%, 50% 0%`
            // wash, the HudSpecplayer `0% 0%, 0% 0%` strip).
            return true;
        }

        if (!axis_aligned || radii.any())
        {
            // Oblique direction and/or rounded corners: evaluate the gradient per
            // vertex. The stop points resolve to PIXEL coordinates of the box and
            // t projects onto the pixel-space axis (WebCore StyleGradientImage::
            // computeEndPoint resolves percentages against the pixel size, so
            // iso-colour lines are perpendicular to the on-screen axis; percent-
            // space projection would skew the band angle on non-square panels).
            // Linear gradients are affine in position, so the rounded-rect fan
            // interpolates them exactly between stops; oblique gradients use the
            // grid so clamped ends and interior stops stay sharp.
            const float p0x = x + w * gradient.x0 / 100.0F;
            const float p0y = y + h * gradient.y0 / 100.0F;
            const float pdx = (x + w * gradient.x1 / 100.0F) - p0x;
            const float pdy = (y + h * gradient.y1 / 100.0F) - p0y;
            const float pixel_len_sq = pdx * pdx + pdy * pdy;
            const auto color_at = [&, p0x, p0y, pdx, pdy, pixel_len_sq](float px, float py) {
                const float t = ((px - p0x) * pdx + (py - p0y) * pdy) / pixel_len_sq;
                return gradient_color_at(gradient, t, opacity);
            };
            if (radii.any())
            {
                add_rounded_rect_shaded(x, y, w, h, radii, 0, matrix, color_at);
            }
            else
            {
                add_gradient_grid(x, y, w, h, matrix, color_at);
            }
            return true;
        }

        const bool horizontal = std::fabs(dx) >= std::fabs(dy);
        const float axis_delta = horizontal ? dx : dy;
        const float axis_start = horizontal ? gradient.x0 : gradient.y0;

        std::vector<std::pair<float, PanoramaColor>> axis_stops;
        axis_stops.reserve(gradient.stops.size());
        for (const PanoramaGradientStop& stop : gradient.stops)
        {
            const float position = std::clamp((axis_start + axis_delta * stop.offset) / 100.0F, 0.0F, 1.0F);
            axis_stops.push_back({position, scale_alpha(stop.color, opacity)});
        }
        // A negative axis direction (e.g. `100% 0% -> 0% 0%`) maps the stops to
        // descending positions, which flips the relative order of duplicate-offset
        // (hard) stops; reverse before the stable sort so ties keep the gradient's
        // own left/right limit semantics instead of inverting the hard transition.
        if (axis_delta < 0.0F)
        {
            std::reverse(axis_stops.begin(), axis_stops.end());
        }
        std::stable_sort(axis_stops.begin(), axis_stops.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.first < rhs.first;
        });
        if (axis_stops.empty())
        {
            return true;
        }
        if (axis_stops.back().first - axis_stops.front().first < 0.001F)
        {
            add_rect(x, y, w, h, axis_stops.front().second, 0, matrix);
            return true;
        }

        const auto draw_range = [&](float a, float b, PanoramaColor ca, PanoramaColor cb) {
            const float start = std::clamp(a, 0.0F, 1.0F);
            const float end = std::clamp(b, 0.0F, 1.0F);
            if (end - start <= 0.0001F)
            {
                return;
            }
            // Stops interpolate in premultiplied alpha (WebCore legacy-gradient
            // semantics) but the rasterizer lerps vertex colours in straight
            // alpha; when both alpha and hue change across the span the two
            // disagree (the straight lerp greys toward a transparent stop's
            // hue), so subdivide and evaluate the premultiplied ramp per cut.
            const bool needs_subdivision =
                ca.a != cb.a && (ca.r != cb.r || ca.g != cb.g || ca.b != cb.b);
            const int segments = needs_subdivision ? 8 : 1;
            if (needs_subdivision)
            {
                // A fully transparent stop adopts the visible side's hue (see
                // lerp_color_premultiplied) so the rasterizer's straight lerp
                // toward it cannot grey the ramp.
                if (ca.a == 0)
                {
                    ca = PanoramaColor{cb.r, cb.g, cb.b, 0};
                }
                if (cb.a == 0)
                {
                    cb = PanoramaColor{ca.r, ca.g, ca.b, 0};
                }
            }
            for (int seg = 0; seg < segments; ++seg)
            {
                const float f0 = static_cast<float>(seg) / static_cast<float>(segments);
                const float f1 = static_cast<float>(seg + 1) / static_cast<float>(segments);
                const float s0 = start + (end - start) * f0;
                const float s1 = start + (end - start) * f1;
                const PanoramaColor c0 = seg == 0 ? ca : lerp_color_premultiplied(ca, cb, f0);
                const PanoramaColor c1 = seg == segments - 1 ? cb : lerp_color_premultiplied(ca, cb, f1);
                if (horizontal)
                {
                    add_gradient_quad(x + w * s0, y, w * (s1 - s0), h, c0, c1, c1, c0, matrix);
                }
                else
                {
                    add_gradient_quad(x, y + h * s0, w, h * (s1 - s0), c0, c0, c1, c1, matrix);
                }
            }
        };

        if (axis_stops.front().first > 0.0F)
        {
            draw_range(0.0F, axis_stops.front().first, axis_stops.front().second, axis_stops.front().second);
        }
        for (std::size_t i = 1; i < axis_stops.size(); ++i)
        {
            draw_range(axis_stops[i - 1].first, axis_stops[i].first, axis_stops[i - 1].second, axis_stops[i].second);
        }
        if (axis_stops.back().first < 1.0F)
        {
            draw_range(axis_stops.back().first, 1.0F, axis_stops.back().second, axis_stops.back().second);
        }
        return true;
    }

    void emit_background_fill(const PanoramaComputedStyle& style, float x, float y, float w, float h,
        const CornerRadii& radii, float opacity, const Matrix2D& matrix)
    {
        if (w <= 0.0F || h <= 0.0F)
        {
            return;
        }
        if (style.background_gradient.present())
        {
            if (emit_background_gradient(style.background_gradient, x, y, w, h, radii, opacity, matrix))
            {
                return;
            }
            const PanoramaColor fallback = gradient_color_at(style.background_gradient, 0.5F, opacity);
            if (fallback.a > 0)
            {
                add_rounded_rect(x, y, w, h, radii, fallback, 0, matrix);
            }
            return;
        }
        if (style.background_color.visible())
        {
            add_rounded_rect(x, y, w, h, radii, scale_alpha(style.background_color, opacity), 0, matrix);
        }
    }

    // Box-shadow, following WebCore's BackgroundPainter::paintBoxShadow (Normal
    // style): the shadow fill is the border box moved by the offset and inflated by
    // the spread, with already-rounded corners expanded by the spread (sharp corners
    // stay sharp: FloatRoundedRect::Radii::expand only grows non-zero radii). The
    // fill is gaussian-blurred with sigma = blur/2 (ShadowData::paintingExtent), so
    // coverage is ~50% AT the fill edge, ~1 at `blur` inside and ~0 at `blur`
    // outside; this painter approximates that profile with a linear coverage ramp
    // across [-blur, +blur] built from stitched rounded-contour rings. Finally
    // WebCore clips out the rounded border box so the shadow never darkens the
    // panel itself; Panorama's `fill` keyword opts out of that knockout (the shadow
    // also paints under the panel — what CS:GO's translucent panels rely on).
    // `radii` is the node's resolved (constrained) border radii in px.
    void emit_box_shadow(const PanoramaNode& node, const PanoramaLayoutBox& box, const CornerRadii& radii,
        float opacity, const Matrix2D& matrix, bool inset_phase)
    {
        const PanoramaBoxShadow& shadow = node.computed.box_shadow;
        if (!shadow.present || shadow.inset != inset_phase)
        {
            return;
        }
        const PanoramaColor full = scale_alpha(shadow.color, opacity);
        if (full.a == 0)
        {
            return;
        }
        PanoramaColor clear = full;
        clear.a = 0;

        if (shadow.inset)
        {
            // WebCore BackgroundPainter::paintInsetBoxShadow: the lit hole is the
            // border box inset by `spread` and MOVED by the shadow offset
            // (areaCastingShadowInHole), so the shadow is thicker on the sides the
            // offset pushes the hole away from. Per side, the falloff is centred
            // on the hole boundary at distance `dist` from the box edge, extending
            // ±blur: a solid plateau to (dist - blur), then a linear ramp to
            // (dist + blur). Drawn over the background, clipped to the box.
            if (box.width <= 0.0F || box.height <= 0.0F)
            {
                return;
            }
            const float b = shadow.blur;
            const float dist_top = shadow.spread + shadow.offset_y;
            const float dist_bottom = shadow.spread - shadow.offset_y;
            const float dist_left = shadow.spread + shadow.offset_x;
            const float dist_right = shadow.spread - shadow.offset_x;
            if (std::max(std::max(dist_top, dist_bottom), std::max(dist_left, dist_right)) + b <= 0.5F)
            {
                return;
            }
            const ClipRect saved = clip_;
            set_clip(intersect_clip(saved, box.x, box.y, box.x + box.width, box.y + box.height));
            // side: 0 = top, 1 = bottom, 2 = left, 3 = right.
            const auto edge = [&](float dist, int side) {
                const float solid = std::max(0.0F, dist - b);
                const float total = dist + b;
                if (total <= 0.5F)
                {
                    return;
                }
                switch (side)
                {
                case 0:
                    add_rect(box.x, box.y, box.width, solid, full, 0, matrix);
                    add_gradient_quad(box.x, box.y + solid, box.width, total - solid, full, full, clear, clear, matrix);
                    break;
                case 1:
                    add_rect(box.x, box.y + box.height - solid, box.width, solid, full, 0, matrix);
                    add_gradient_quad(box.x, box.y + box.height - total, box.width, total - solid, clear, clear, full,
                        full, matrix);
                    break;
                case 2:
                    add_rect(box.x, box.y, solid, box.height, full, 0, matrix);
                    add_gradient_quad(box.x + solid, box.y, total - solid, box.height, full, clear, clear, full, matrix);
                    break;
                default:
                    add_rect(box.x + box.width - solid, box.y, solid, box.height, full, 0, matrix);
                    add_gradient_quad(box.x + box.width - total, box.y, total - solid, box.height, clear, full, full,
                        clear, matrix);
                    break;
                }
            };
            edge(dist_top, 0);
            edge(dist_bottom, 1);
            edge(dist_left, 2);
            edge(dist_right, 3);
            set_clip(saved);
            return;
        }

        // The shadow fill (WebCore's fillRect): border box + offset, inflated by spread.
        const float sx = box.x + shadow.offset_x - shadow.spread;
        const float sy = box.y + shadow.offset_y - shadow.spread;
        const float sw = box.width + 2.0F * shadow.spread;
        const float sh = box.height + 2.0F * shadow.spread;
        if (sw <= 0.0F || sh <= 0.0F)
        {
            return;
        }
        // WebCore Radii::expand: spread grows only the already-rounded corners,
        // then the shape constrains to its own box.
        const CornerRadii shape_radius = radii.expanded(std::max(0.0F, shadow.spread)).constrained(sw, sh);
        const float b = shadow.blur > 0.5F ? shadow.blur : 0.0F;

        // Full-coverage core contour, `blur` inside the fill edge; collapses to the
        // centre when the blur exceeds the fill's half-extent. The outer isophote of
        // a blurred fill is rounded even for sharp fills, so the outer contour's
        // radius grows by `blur`.
        float core_x = sx + b;
        float core_w = sw - 2.0F * b;
        if (core_w < 0.0F)
        {
            core_x = sx + 0.5F * sw;
            core_w = 0.0F;
        }
        float core_y = sy + b;
        float core_h = sh - 2.0F * b;
        if (core_h < 0.0F)
        {
            core_y = sy + 0.5F * sh;
            core_h = 0.0F;
        }
        const CornerRadii core_radius = shape_radius.shrunk(b);
        const CornerRadii outer_radius = shape_radius.grown(b);

        const auto emit_core_and_falloff = [&]() {
            if (b <= 0.0F)
            {
                add_rounded_rect(sx, sy, sw, sh, shape_radius, full, 0, matrix);
                return;
            }
            if (core_w > 0.0F && core_h > 0.0F)
            {
                add_rounded_rect(core_x, core_y, core_w, core_h, core_radius, full, 0, matrix);
            }
            add_rounded_ring(core_x, core_y, core_w, core_h, core_radius, full,
                sx - b, sy - b, sw + 2.0F * b, sh + 2.0F * b, outer_radius, clear, matrix);
        };

        if (shadow.fill)
        {
            emit_core_and_falloff();
            return;
        }

        if (shadow.offset_x == 0.0F && shadow.offset_y == 0.0F && shadow.spread >= 0.0F)
        {
            // Concentric case (CS:GO's shadowOffset and most hover glows): the
            // rounded border box is a contour of the same family, so the knockout is
            // geometric — the rings simply START at the border contour. This is the
            // exact analogue of WebCore's clipOutRoundedRect, and what keeps a
            // circular button's halo round instead of a black square frame.
            if (b <= 0.0F)
            {
                if (shadow.spread > 0.0F)
                {
                    add_rounded_ring(box.x, box.y, box.width, box.height, radii, full,
                        sx, sy, sw, sh, shape_radius, full, matrix);
                }
                // Offset-less, blur-less, spread-less shadows are invisible
                // (WebCore skips them outright).
                return;
            }
            if (shadow.spread >= b)
            {
                // Border -> core at full coverage, then core -> outer fading out.
                add_rounded_ring(box.x, box.y, box.width, box.height, radii, full,
                    core_x, core_y, core_w, core_h, core_radius, full, matrix);
                add_rounded_ring(core_x, core_y, core_w, core_h, core_radius, full,
                    sx - b, sy - b, sw + 2.0F * b, sh + 2.0F * b, outer_radius, clear, matrix);
                return;
            }
            // The border contour sits inside the falloff band: coverage there is
            // (blur + spread) / (2 * blur), reaching 0 at `blur` outside the fill.
            PanoramaColor edge = full;
            edge.a = static_cast<std::uint8_t>(std::clamp(
                static_cast<float>(full.a) * (b + shadow.spread) / (2.0F * b), 0.0F, 255.0F) + 0.5F);
            add_rounded_ring(box.x, box.y, box.width, box.height, radii, edge,
                sx - b, sy - b, sw + 2.0F * b, sh + 2.0F * b, outer_radius, clear, matrix);
            return;
        }

        // Offset shadows: same rounded geometry, knocked out with four scissor
        // regions around the border box (a rect approximation of WebCore's
        // clipOutRoundedRect — exact for the square panels CS:GO uses offset
        // shadows on).
        const ClipRect saved = clip_;
        const struct
        {
            float x0, y0, x1, y1;
        } regions[4] = {
            {-kClipInfinity, -kClipInfinity, kClipInfinity, box.y},                          // above
            {-kClipInfinity, box.y + box.height, kClipInfinity, kClipInfinity},              // below
            {-kClipInfinity, box.y, box.x, box.y + box.height},                              // left
            {box.x + box.width, box.y, kClipInfinity, box.y + box.height},                   // right
        };
        for (const auto& region : regions)
        {
            const ClipRect region_clip = intersect_clip(saved, region.x0, region.y0, region.x1, region.y1);
            if (region_clip.x1 <= region_clip.x0 || region_clip.y1 <= region_clip.y0)
            {
                continue;
            }
            set_clip(region_clip);
            emit_core_and_falloff();
        }
        set_clip(saved);
    }

    void paint(const PanoramaNode& node, float opacity, const Matrix2D& parent_transform, const ClipRect& clip)
    {
        const PanoramaComputedStyle& s = node.computed;
        if (!s.visible)
        {
            return;
        }
        const float node_opacity = opacity * s.opacity;
        if (node_opacity <= 0.0F)
        {
            return;
        }
        const PanoramaLayoutBox& box = node.layout;
        const Matrix2D transform = node_transform_matrix(node, parent_transform);

        // Panorama `clip:` — a render-time clip on the panel AND its subtree (does
        // not affect layout or hit testing). rect() narrows the scissor; radial()
        // marks the geometry appended below and wedge-clips it afterwards.
        ClipRect self_clip = clip;
        if (s.clip.type == PanoramaClipType::Rect)
        {
            float rx0 = box.x + box.width * (s.clip.rect_left / 100.0F);
            float ry0 = box.y + box.height * (s.clip.rect_top / 100.0F);
            float rx1 = box.x + box.width * (s.clip.rect_right / 100.0F);
            float ry1 = box.y + box.height * (s.clip.rect_bottom / 100.0F);
            if (rx1 <= rx0 || ry1 <= ry0)
            {
                return; // empty visible rect: the panel is fully wiped
            }
            if (!is_identity(transform))
            {
                // The clipped geometry is emitted through `transform`; clip in
                // the same screen space (see map_rect_through).
                map_rect_through(transform, rx0, ry0, rx1, ry1);
            }
            self_clip = intersect_clip(self_clip, rx0, ry0, rx1, ry1);
        }
        const bool radial_clip = s.clip.type == PanoramaClipType::Radial && s.clip.radial_sweep > 0.0F;
        if (radial_clip && s.clip.radial_sweep >= 360.0F)
        {
            return; // the hidden wedge covers the full circle
        }
        std::size_t radial_cmd_start = 0;
        std::size_t radial_back_index_start = 0;
        if (radial_clip)
        {
            radial_cmd_start = list_.commands.size();
            radial_back_index_start = radial_cmd_start > 0 ? list_.commands.back().indices.size() : 0;
        }

        // This node's own background/border/text/image paint within the inherited clip
        // (overflow clips a node's CHILDREN, not the node itself) and use this node's
        // blend mode (mix-blend-mode is per-element, not inherited — children set their
        // own at their paint() entry).
        set_clip(self_clip);
        set_blend(s.blend_mode);

        // Resolve border-radius (percentage is relative to the smaller box dimension,
        // so `50%` on a square yields a circle). Per-corner radii (`a b c d`) carry
        // through, constrained per WebCore so adjacent corners never overlap.
        CornerRadii radii;
        if (s.border_radius_per_corner)
        {
            radii = {s.border_radius_tl, s.border_radius_tr, s.border_radius_br, s.border_radius_bl};
        }
        else
        {
            const float r = s.border_radius_percent
                ? (s.border_radius / 100.0F) * std::min(box.width, box.height)
                : s.border_radius;
            radii = uniform_radii(r);
        }
        radii = radii.constrained(box.width, box.height);
        const bool rounded = radii.any();
        const float bw_top = std::max(0.0F, s.border_top());
        const float bw_right = std::max(0.0F, s.border_right());
        const float bw_bottom = std::max(0.0F, s.border_bottom());
        const float bw_left = std::max(0.0F, s.border_left());
        const bool has_border = (bw_top > 0.0F && s.border_top_color().visible()) ||
            (bw_right > 0.0F && s.border_right_color().visible()) ||
            (bw_bottom > 0.0F && s.border_bottom_color().visible()) ||
            (bw_left > 0.0F && s.border_left_color().visible());
        // Outset box-shadow paints behind the background/border.
        emit_box_shadow(node, box, radii, node_opacity, transform, false);

        // WebCore paint order (BackgroundPainter then BorderPainter): the
        // background fills the WHOLE border box first (background-clip:
        // border-box), and the border paints on top as the band between the
        // outer and inner rounded rects (paintBorderSides clips out the inner
        // border). Painting the border as a full backdrop instead would tint
        // every translucent background with the border colour.
        emit_background_fill(s, box.x, box.y, box.width, box.height, radii, node_opacity, transform);
        if (node.background_texture != 0)
        {
            const PanoramaColor tint = texture_tint(s, node_opacity * s.background_image_opacity);
            emit_background_image(node, box.x, box.y, box.width, box.height, radii, tint, transform);
        }

        if (has_border && rounded)
        {
            // Rounded borders keep the uniform-width band (per-side widths with
            // a radius do not appear in shipped CS:GO sheets); the band is the
            // region between the outer contour and the inner contour (outer
            // radii minus border width, clamped at 0 — WebCore innerBorder).
            const float bw = std::max(std::max(bw_top, bw_bottom), std::max(bw_left, bw_right));
            PanoramaColor bc = s.border_top_color();
            if (!bc.visible())
            {
                bc = s.border_bottom_color().visible() ? s.border_bottom_color()
                    : s.border_left_color().visible() ? s.border_left_color() : s.border_right_color();
            }
            bc = scale_alpha(bc, node_opacity);
            const float inner_w = box.width - 2.0F * bw;
            const float inner_h = box.height - 2.0F * bw;
            if (inner_w <= 0.0F || inner_h <= 0.0F)
            {
                // The border swallows the whole box.
                add_rounded_rect(box.x, box.y, box.width, box.height, radii, bc, 0, transform);
            }
            else
            {
                add_rounded_ring(box.x + bw, box.y + bw, inner_w, inner_h, radii.shrunk(bw), bc,
                    box.x, box.y, box.width, box.height, radii, bc, transform);
            }
        }
        else if (has_border)
        {
            // Square borders are four edge bands in their own side colours
            // (border-bottom separators, tab underlines). The side bands span
            // the full width on top/bottom and sit between them on left/right,
            // so translucent borders never double-blend at the corners.
            const auto side = [&](float x, float y, float w, float h, PanoramaColor color) {
                if (w > 0.0F && h > 0.0F && color.visible())
                {
                    add_rect(x, y, w, h, scale_alpha(color, node_opacity), 0, transform);
                }
            };
            side(box.x, box.y, box.width, bw_top, s.border_top_color());
            side(box.x, box.y + box.height - bw_bottom, box.width, bw_bottom, s.border_bottom_color());
            side(box.x, box.y + bw_top, bw_left, box.height - bw_top - bw_bottom, s.border_left_color());
            side(box.x + box.width - bw_right, box.y + bw_top, bw_right, box.height - bw_top - bw_bottom,
                s.border_right_color());
        }

        // Inset box-shadow paints over the background/border (clipped to the box).
        emit_box_shadow(node, box, radii, node_opacity, transform, true);

        // WebCore clips contents after painting the background/border (see
        // RenderBox::pushContentsClip / overflowClipRect). Apply the same content
        // clip to this node's image/text and descendants, but not to its own box
        // decorations.
        const ClipRect content_clip = overflow_content_clip(node, self_clip, transform);

        // Image content (e.g. a rasterized SVG icon) fills the content box. CS:GO
        // icons are white masks tinted by the (inherited) wash-color and brightness,
        // which is how nav buttons recolour their icon on hover/select.
        if (node.paint_texture != 0)
        {
            set_clip(content_clip);
            // `scaling=` placement: where inside the content box the texture quad
            // goes. Stretch fills the box; the other modes centre an aspect-true
            // (or natural-size) rect, clipped by the content clip above.
            float img_x = box.content_x;
            float img_y = box.content_y;
            float img_w = box.content_width;
            float img_h = box.content_height;
            {
                const float nat_w = node.paint_texture_natural_width;
                const float nat_h = node.paint_texture_natural_height;
                const float aspect = nat_h > 0.0F ? nat_w / nat_h : 0.0F;
                switch (node.paint_texture_scaling)
                {
                case PanoramaImageScaling::Stretch:
                    break;
                case PanoramaImageScaling::None:
                    if (nat_w > 0.0F && nat_h > 0.0F)
                    {
                        img_w = nat_w;
                        img_h = nat_h;
                    }
                    break;
                case PanoramaImageScaling::StretchPreserveAspect:
                    if (aspect > 0.0F && img_w > 0.0F && img_h > 0.0F)
                    {
                        if (img_w / img_h > aspect)
                        {
                            img_w = img_h * aspect;
                        }
                        else
                        {
                            img_h = img_w / aspect;
                        }
                    }
                    break;
                case PanoramaImageScaling::StretchXPreserveAspect:
                    if (aspect > 0.0F)
                    {
                        img_h = img_w / aspect;
                    }
                    break;
                case PanoramaImageScaling::StretchYPreserveAspect:
                    if (aspect > 0.0F)
                    {
                        img_w = img_h * aspect;
                    }
                    break;
                }
                img_x = box.content_x + (box.content_width - img_w) * 0.5F;
                img_y = box.content_y + (box.content_height - img_h) * 0.5F;
            }
            // img-shadow: an offset silhouette of the texture under the image (the
            // vertex colour multiplies the texel, so a black shadow colour yields a
            // true alpha silhouette). Blur is approximated with diagonal taps.
            if (s.img_shadow.present)
            {
                // As with text-shadow: strength may exceed 1, scale_alpha clamps.
                PanoramaColor silhouette = s.img_shadow.color;
                silhouette.a = static_cast<std::uint8_t>(std::clamp(
                    static_cast<float>(silhouette.a) * std::clamp(node_opacity, 0.0F, 1.0F) * s.img_shadow.strength,
                    0.0F, 255.0F) + 0.5F);
                if (silhouette.a != 0)
                {
                    const auto draw_silhouette = [&](float dx, float dy, PanoramaColor c) {
                        add_rect(img_x + s.img_shadow.offset_x + dx, img_y + s.img_shadow.offset_y + dy,
                            img_w, img_h, c, node.paint_texture, transform);
                    };
                    const float blur_radius = 0.5F * s.img_shadow.blur;
                    if (blur_radius > 0.25F)
                    {
                        PanoramaColor ring = silhouette;
                        ring.a = static_cast<std::uint8_t>(
                            std::clamp(static_cast<float>(silhouette.a) * 0.35F, 0.0F, 255.0F) + 0.5F);
                        const float diag = blur_radius * 0.70710678F;
                        draw_silhouette(diag, diag, ring);
                        draw_silhouette(diag, -diag, ring);
                        draw_silhouette(-diag, diag, ring);
                        draw_silhouette(-diag, -diag, ring);
                    }
                    draw_silhouette(0.0F, 0.0F, silhouette);
                }
            }
            const PanoramaColor tint = texture_tint(s, node_opacity);
            add_rect(img_x, img_y, img_w, img_h, tint, node.paint_texture, transform);
        }

        if (!node.text.empty() && panorama_node_paints_own_text(node))
        {
            // Toggle/radio buttons render their text via the internal control
            // label child (ensure_panorama_selection_control_internals).
            set_clip(content_clip);
            paint_text(node, node_opacity, transform);
        }

        // overflow clips descendants to the padding box. A single-axis clip uses a
        // very large opposite axis, which the backend clamps to the framebuffer, to
        // match WebCore's "expand to infinite" behavior for visible axes.
        const ClipRect child_clip = content_clip;

        // Children paint in ascending z-index (painter's algorithm: higher z on top),
        // keeping DOM order for equal z. z-index ordering is applied among direct
        // siblings only (no full CSS stacking contexts); this covers Panorama's use of
        // z-index to layer popups/tooltips/dropdowns over their siblings.
        bool needs_sort = false;
        for (const auto& child : node.children)
        {
            if (child->computed.z_index != 0)
            {
                needs_sort = true;
                break;
            }
        }
        if (!needs_sort)
        {
            for (const auto& child : node.children)
            {
                if (is_open_dropdown_popup_child(node, *child) && !paints_in_normal_dropdown_flow(node, *child))
                {
                    continue;
                }
                paint(*child, node_opacity, transform, child_clip);
            }
        }
        else
        {
            std::vector<const PanoramaNode*> ordered;
            ordered.reserve(node.children.size());
            for (const auto& child : node.children)
            {
                ordered.push_back(child.get());
            }
            std::stable_sort(ordered.begin(), ordered.end(),
                [](const PanoramaNode* a, const PanoramaNode* b) { return a->computed.z_index < b->computed.z_index; });
            for (const PanoramaNode* child : ordered)
            {
                if (is_open_dropdown_popup_child(node, *child) && !paints_in_normal_dropdown_flow(node, *child))
                {
                    continue;
                }
                paint(*child, node_opacity, transform, child_clip);
            }
        }

        // `blur: gaussian/fastgaussian(...)`: the node's subtree just finished
        // painting, so blurring the rendered region now is equivalent to blurring
        // the subtree's composited output (CS:GO's frosted-glass submenu
        // backgrounds — #MainMenuCore/#MainMenuBackground CSGOBlurTargets).
        //
        // CSGOBlurTarget panels carry a `blurrects` attribute: a whitespace list
        // of panel ids/classes whose CURRENT boxes restrict where the blur shows
        // (e.g. `mainmenu-content__blur-target` — frosted glass appears only
        // under the open submenu's content panel, not across the whole
        // background). Without the attribute the whole panel box blurs.
        if (s.blur.present && (s.blur.std_x > 0.0F || s.blur.std_y > 0.0F))
        {
            const auto emit_blur_rect = [&](float rx0, float ry0, float rx1, float ry1) {
                rx0 = std::max(rx0, box.x);
                ry0 = std::max(ry0, box.y);
                rx1 = std::min(rx1, box.x + box.width);
                ry1 = std::min(ry1, box.y + box.height);
                if (self_clip.active)
                {
                    rx0 = std::max(rx0, self_clip.x0);
                    ry0 = std::max(ry0, self_clip.y0);
                    rx1 = std::min(rx1, self_clip.x1);
                    ry1 = std::min(ry1, self_clip.y1);
                }
                if (rx1 <= rx0 || ry1 <= ry0)
                {
                    return;
                }
                PanoramaDrawCommand cmd = acquire_command();
                cmd.texture = 0;
                cmd.blend_mode = PanoramaBlendMode::Normal;
                cmd.scissor = true;
                cmd.scissor_x = rx0;
                cmd.scissor_y = ry0;
                cmd.scissor_width = rx1 - rx0;
                cmd.scissor_height = ry1 - ry0;
                cmd.blur_std_x = std::max(0.0F, s.blur.std_x);
                cmd.blur_std_y = std::max(0.0F, s.blur.std_y);
                cmd.blur_passes = std::max(1, static_cast<int>(s.blur.passes + 0.5F));
                list_.commands.push_back(std::move(cmd));
            };

            const auto rects_it = node.attributes.find("blurrects");
            if (rects_it != node.attributes.end() && !rects_it->second.empty() && document_root_ != nullptr)
            {
                std::istringstream names(rects_it->second);
                std::string name;
                while (names >> name)
                {
                    collect_blur_rects(*document_root_, name, emit_blur_rect);
                }
            }
            else
            {
                emit_blur_rect(box.x, box.y, box.x + box.width, box.y + box.height);
            }
        }

        // clip: radial(...) — wedge-clip everything this node's subtree emitted.
        // Runs innermost-first for nested radial clips (each paint() call clips
        // its own appended range on the way out).
        if (radial_clip)
        {
            apply_radial_clip(node, transform, radial_cmd_start, radial_back_index_start);
        }
    }

    // Walks the document for visible panels named by a CSGOBlurTarget blurrect
    // entry (matched by id OR class) and emits their boxes.
    template <typename EmitRect>
    void collect_blur_rects(const PanoramaNode& node, const std::string& name, const EmitRect& emit)
    {
        if (!node.computed.visible)
        {
            return;
        }
        if ((node.id == name || node.has_class(name)) && node.layout.width > 0.0F && node.layout.height > 0.0F)
        {
            emit(node.layout.x, node.layout.y, node.layout.x + node.layout.width,
                node.layout.y + node.layout.height);
        }
        for (const auto& child : node.children)
        {
            collect_blur_rects(*child, name, emit);
        }
    }

    void paint_dropdown_popups(const PanoramaNode& node, float opacity)
    {
        const PanoramaComputedStyle& s = node.computed;
        if (!s.visible)
        {
            return;
        }

        const float node_opacity = opacity * s.opacity;
        if (node.has_popup_layout && open_dropdown_header_child(node) != nullptr)
        {
            paint_dropdown_popup(node, node_opacity);
        }

        for (const auto& child : node.children)
        {
            paint_dropdown_popups(*child, node_opacity);
        }
    }

private:
    void paint_dropdown_popup(const PanoramaNode& dropdown, float opacity)
    {
        if (opacity <= 0.0F)
        {
            return;
        }

        const PanoramaLayoutBox& box = dropdown.popup_layout;
        set_clip(ClipRect{});
        set_blend(PanoramaBlendMode::Normal);

        // CS:GO's DropDownMenu uses contextMenuBackground rgba(44,44,44,0.98).
        add_rounded_rect(box.x, box.y, box.width, box.height, 2.0F, scale_alpha({44, 44, 44, 250}, opacity), 0, Matrix2D{});
        const PanoramaColor border = scale_alpha({70, 70, 70, 255}, opacity);
        add_rect(box.x, box.y, box.width, 1.0F, border, 0, Matrix2D{});
        add_rect(box.x, box.y + std::max(0.0F, box.height - 1.0F), box.width, 1.0F, border, 0, Matrix2D{});
        add_rect(box.x, box.y, 1.0F, box.height, border, 0, Matrix2D{});
        add_rect(box.x + std::max(0.0F, box.width - 1.0F), box.y, 1.0F, box.height, border, 0, Matrix2D{});

        for (const auto& child_owner : dropdown.children)
        {
            const PanoramaNode& child = *child_owner;
            if (!child.computed.visible || !child.has_popup_layout)
            {
                continue;
            }

            PanoramaNode popup_node;
            popup_node.tag = child.tag;
            popup_node.tag_lower = child.tag_lower;
            popup_node.id = child.id;
            popup_node.classes = child.classes;
            popup_node.attributes = child.attributes;
            popup_node.inline_style = child.inline_style;
            popup_node.text = child.text;
            popup_node.computed = child.computed;
            popup_node.layout = child.popup_layout;
            popup_node.paint_texture = child.paint_texture;
            popup_node.background_texture = child.background_texture;
            popup_node.background_texture_aspect = child.background_texture_aspect;
            popup_node.shrink_font_size = child.shrink_font_size;
            popup_node.visibility_override = child.visibility_override;
            popup_node.hovered = child.hovered;
            popup_node.active = child.active;
            popup_node.focused = child.focused;
            popup_node.selected = child.selected;
            if (popup_node.hovered && !popup_node.computed.background_color.visible())
            {
                popup_node.computed.background_color = {80, 80, 80, 255};
            }
            paint(popup_node, opacity, Matrix2D{}, ClipRect{});
        }
    }

    // Post-processes the draw commands appended while a `clip: radial(...)` node's
    // subtree painted: every triangle is clipped to the VISIBLE region (the
    // complement of the hidden wedge), decomposed into <= 2 convex wedges.
    void apply_radial_clip(
        const PanoramaNode& node, const Matrix2D& transform, std::size_t cmd_start, std::size_t back_index_start)
    {
        const PanoramaClip& c = node.computed.clip;
        const PanoramaLayoutBox& box = node.layout;
        const float visible = 360.0F - c.radial_sweep;
        if (visible <= 0.0F)
        {
            return; // fully hidden (paint() returns before emitting in this case)
        }

        // Local-space wedge: centre (% of the box) + boundary ray directions.
        // 0deg = 12 o'clock, sweeping clockwise; screen y grows down, so
        // dir(theta) = (sin theta, -cos theta).
        const float cx = box.x + box.width * (c.radial_center_x / 100.0F);
        const float cy = box.y + box.height * (c.radial_center_y / 100.0F);
        constexpr float kDegToRad = 3.1415926535F / 180.0F;
        const auto dir_x = [&](float deg) { return std::sin(deg * kDegToRad); };
        const auto dir_y = [&](float deg) { return -std::cos(deg * kDegToRad); };

        // The node's matrix maps the wedge into the space its vertices were emitted
        // in. A mirroring matrix (negative determinant — e.g. SpinnerRotate's
        // scaleX(-1)) flips orientation, which inverts the half-plane sides.
        const auto map_x = [&](float x, float y) { return transform.a * x + transform.c * y + transform.e; };
        const auto map_y = [&](float x, float y) { return transform.b * x + transform.d * y + transform.f; };
        const float det = transform.a * transform.d - transform.b * transform.c;
        const float flip = det < 0.0F ? -1.0F : 1.0F;
        const float wcx = map_x(cx, cy);
        const float wcy = map_y(cx, cy);

        // Visible wedge = [start + sweep, start + 360], split into convex spans.
        std::vector<WedgePlanes> wedges;
        float angle = c.radial_start + c.radial_sweep;
        float remaining = visible;
        while (remaining > 0.0F && wedges.size() < 2)
        {
            const float span = std::min(remaining, 180.0F);
            const float ax = map_x(cx + dir_x(angle), cy + dir_y(angle)) - wcx;
            const float ay = map_y(cx + dir_x(angle), cy + dir_y(angle)) - wcy;
            const float bx = map_x(cx + dir_x(angle + span), cy + dir_y(angle + span)) - wcx;
            const float by = map_y(cx + dir_x(angle + span), cy + dir_y(angle + span)) - wcy;
            WedgePlanes planes;
            planes[0] = {wcx, wcy, -ay * flip, ax * flip}; // clockwise side of the opening ray
            planes[1] = {wcx, wcy, by * flip, -bx * flip}; // counter-clockwise side of the closing ray
            wedges.push_back(planes);
            angle += span;
            remaining -= span;
        }

        if (cmd_start > 0)
        {
            clip_command_range(list_.commands[cmd_start - 1], back_index_start, wedges);
        }
        for (std::size_t i = cmd_start; i < list_.commands.size(); ++i)
        {
            clip_command_range(list_.commands[i], 0, wedges);
        }
    }

    // Rebuilds the triangles of `cmd` from `index_start` on, keeping only the parts
    // inside any of the visible wedges. Clipped vertices are appended (the original
    // appended vertices may go unreferenced, which is harmless).
    static void clip_command_range(
        PanoramaDrawCommand& cmd, std::size_t index_start, const std::vector<WedgePlanes>& wedges)
    {
        if (cmd.is_backdrop_blur() || cmd.indices.size() <= index_start)
        {
            return;
        }
        const std::vector<int> tris(cmd.indices.begin() + static_cast<std::ptrdiff_t>(index_start), cmd.indices.end());
        cmd.indices.resize(index_start);
        std::vector<PanoramaPaintVertex> poly;
        std::vector<PanoramaPaintVertex> scratch;
        for (std::size_t t = 0; t + 2 < tris.size(); t += 3)
        {
            for (const WedgePlanes& planes : wedges)
            {
                poly.clear();
                poly.push_back(cmd.vertices[static_cast<std::size_t>(tris[t])]);
                poly.push_back(cmd.vertices[static_cast<std::size_t>(tris[t + 1])]);
                poly.push_back(cmd.vertices[static_cast<std::size_t>(tris[t + 2])]);
                clip_polygon_against_plane(poly, scratch, planes[0]);
                if (poly.size() >= 3)
                {
                    clip_polygon_against_plane(poly, scratch, planes[1]);
                }
                if (poly.size() < 3)
                {
                    continue;
                }
                const int base = static_cast<int>(cmd.vertices.size());
                cmd.vertices.insert(cmd.vertices.end(), poly.begin(), poly.end());
                for (std::size_t k = 1; k + 1 < poly.size(); ++k)
                {
                    cmd.indices.push_back(base);
                    cmd.indices.push_back(base + static_cast<int>(k));
                    cmd.indices.push_back(base + static_cast<int>(k) + 1);
                }
            }
        }
    }

    [[nodiscard]] bool command_matches(const PanoramaDrawCommand& cmd, PanoramaTextureId texture) const
    {
        // Backdrop-blur commands are barriers: geometry must never batch into
        // (or after-merge with) them — order against the blur is semantic.
        if (cmd.is_backdrop_blur())
        {
            return false;
        }
        if (cmd.texture != texture || cmd.blend_mode != blend_ || cmd.scissor != clip_.active)
        {
            return false;
        }
        if (!clip_.active)
        {
            return true;
        }
        return cmd.scissor_x == clip_.x0 && cmd.scissor_y == clip_.y0 &&
            cmd.scissor_width == std::max(0.0F, clip_.x1 - clip_.x0) &&
            cmd.scissor_height == std::max(0.0F, clip_.y1 - clip_.y0);
    }

    PanoramaDrawCommand& current_command(PanoramaTextureId texture)
    {
        if (list_.commands.empty() || !command_matches(list_.commands.back(), texture))
        {
            PanoramaDrawCommand cmd = acquire_command();
            cmd.texture = texture;
            cmd.blend_mode = blend_;
            cmd.scissor = clip_.active;
            if (clip_.active)
            {
                cmd.scissor_x = clip_.x0;
                cmd.scissor_y = clip_.y0;
                cmd.scissor_width = std::max(0.0F, clip_.x1 - clip_.x0);
                cmd.scissor_height = std::max(0.0F, clip_.y1 - clip_.y0);
            }
            list_.commands.push_back(std::move(cmd));
        }
        return list_.commands.back();
    }

    PanoramaDrawCommand acquire_command()
    {
        PanoramaDrawCommand cmd;
        if (scratch_ != nullptr && reused_command_index_ < scratch_->reusable_commands.size())
        {
            cmd = std::move(scratch_->reusable_commands[reused_command_index_++]);
            cmd.vertices.clear();
            cmd.indices.clear();
            cmd.blur_std_x = 0.0F;
            cmd.blur_std_y = 0.0F;
            cmd.blur_passes = 0;
        }
        return cmd;
    }

    void paint_text(const PanoramaNode& node, float opacity, const Matrix2D& transform)
    {
        if (!glyphs_.glyph)
        {
            return;
        }
        const PanoramaComputedStyle& s = node.computed;
        const PanoramaColor color = scale_alpha(s.color, opacity);
        if (color.a == 0)
        {
            return;
        }
        // Sums a run's advance at a given font size (matches the draw loop's advance).
        const auto measure_run = [&](std::string_view text, float font, int weight) {
            float width = 0.0F;
            std::size_t k = 0;
            while (k < text.size())
            {
                const char32_t cp = next_codepoint(text, k);
                PanoramaGlyph g;
                if (glyphs_.glyph(cp, font, weight, g))
                {
                    width += g.advance + s.letter_spacing;
                }
            }
            return width;
        };

        // Display text is case-transformed once; the same transform is applied by the
        // layout measure so glyph placement and the measured box agree. No copy is
        // made when no transform applies (`display` views node.text directly).
        std::string transformed_storage;
        std::string_view display = panorama_transform_text_view(node.text, s.text_transform, transformed_storage);
        const bool html = node.is_html_text();
        const float available = node.layout.content_width;

        // text-overflow: shrink renders at the reduced font size the host computed and
        // pre-rasterized (node.shrink_font_size); reading the stored value (rather than
        // recomputing) guarantees the glyphs exist in the atlas.
        const float font = (s.text_overflow == PanoramaTextOverflow::Shrink && node.shrink_font_size > 0.0F)
            ? node.shrink_font_size
            : s.font_size;

        // Split the display text into styled runs. Plain labels are a single run at
        // the element's weight; html="true" labels honour inline <b>/<strong> (bold)
        // and <i>/<em> (italic) markup the way WebCore's UA stylesheet would. The
        // string_views below reference `display`, which outlives this function body.
        struct TextRun
        {
            std::string_view text;
            int weight;
            bool italic;
        };
        std::vector<TextRun> runs;
        if (html)
        {
            for (const PanoramaTextRun& run : panorama_parse_inline_markup(display))
            {
                runs.push_back({run.text, panorama_run_font_weight(s.font_weight, run.bold), run.italic || s.font_italic});
            }
        }
        else
        {
            runs.push_back({display, s.font_weight, s.font_italic});
        }

        const auto measure_runs = [&](float f) {
            float width = 0.0F;
            for (const TextRun& run : runs)
            {
                width += measure_run(run.text, f, run.weight);
            }
            return width;
        };

        // Multi-line text: the layout solver wrapped this label (WebCore line
        // breaking) and stored per-line segments referencing the SAME display
        // text + styled runs derived above; empty = single line.
        const bool multiline = !node.text_lines.empty();

        // text-overflow: ellipsis -> truncate to fit the box and append U+2026. (Only
        // bites when the box is narrower than the text; fit-children labels never do.)
        // Applies to single-run text only; ellipsis labels never wrap (layout keeps
        // them single-line), so the two paths are mutually exclusive.
        if (!html && !multiline && s.text_overflow == PanoramaTextOverflow::Ellipsis && available > 0.0F &&
            measure_run(display, font, s.font_weight) > available)
        {
            static constexpr std::string_view kEllipsis = "\xE2\x80\xA6"; // U+2026 HORIZONTAL ELLIPSIS
            const float budget = std::max(0.0F, available - measure_run(kEllipsis, font, s.font_weight));
            std::string fitted;
            float used = 0.0F;
            std::size_t k = 0;
            while (k < display.size())
            {
                const std::size_t start = k;
                const char32_t cp = next_codepoint(display, k);
                PanoramaGlyph g;
                const float advance = glyphs_.glyph(cp, font, s.font_weight, g) ? g.advance + s.letter_spacing : 0.0F;
                if (used + advance > budget)
                {
                    break;
                }
                used += advance;
                fitted.append(display, start, k - start);
            }
            fitted.append(kEllipsis);
            // `fitted` was built from `display` above; only now may the storage be
            // overwritten (it may back the old view).
            transformed_storage = std::move(fitted);
            display = transformed_storage;
            runs.clear();
            runs.push_back({display, s.font_weight, s.font_italic});
        }

        const float ascent = glyphs_.ascent ? glyphs_.ascent(font, s.font_weight) : font * 0.8F;
        // line-height centres the em box within the line box (best-effort: the font's
        // em height is approximated by font-size since descent is not exposed here).
        const float leading = s.line_height > 0.0F ? std::max(0.0F, s.line_height - font) * 0.5F : 0.0F;
        const float baseline_y = node.layout.content_y + leading + ascent;
        float pen_x = node.layout.content_x;

        // text-align: offset the pen within the content box (the layout measure uses
        // the same advances + letter-spacing, so this stays consistent). Wrapped
        // labels align each line individually inside draw_run below.
        if (!multiline && s.text_align != PanoramaHAlign::Left)
        {
            const float text_width = measure_runs(font);
            if (available > text_width)
            {
                const float slack = available - text_width;
                pen_x += s.text_align == PanoramaHAlign::Center ? slack * 0.5F : slack;
            }
        }

        // text-decoration geometry (WebCore InlineTextBoxStyle / TextDecorationThickness):
        // auto thickness = fontSize/16; the underline's top edge sits
        // gap = max(1, ceil(fontSize/32)) below the baseline; the line-through is
        // centred 2*ascent/3 below the text top, which at auto thickness puts its
        // top edge at baseline - ascent/3. Decorations are never skewed by italics.
        const bool decorated = s.text_decoration_underline || s.text_decoration_line_through;
        const float deco_thickness = font / 16.0F;
        const float underline_gap = std::max(1.0F, std::ceil(font / 32.0F));
        const float single_line_width = decorated && !multiline ? measure_runs(font) : 0.0F;
        const auto draw_decoration = [&](float start, float width, float baseline, float dx, float dy,
                                         PanoramaColor run_color, bool line_through) {
            if (width <= 0.0F)
            {
                return;
            }
            const float y = line_through ? baseline - ascent / 3.0F : baseline + underline_gap;
            add_rect(start + dx, y + dy, width, deco_thickness, run_color, 0, transform);
        };

        // Italic runs render as a synthetic oblique: a shear about the baseline by
        // WebCore's FontCascade::syntheticObliqueAngle() (14 degrees). Glyph advances
        // are unchanged, matching WebCore (the shear is paint-only).
        constexpr float kItalicSkew = 0.24932800F; // tan(14 degrees)
        const auto italic_about = [&](float baseline) {
            return multiply(transform, Matrix2D{1.0F, 0.0F, -kItalicSkew, 1.0F, kItalicSkew * baseline, 0.0F});
        };
        const Matrix2D italic_transform = italic_about(baseline_y);

        // Emits one glyph run from `pen` on `baseline`, returning the advanced pen.
        const auto draw_glyphs = [&](std::string_view text, int weight, const Matrix2D& run_transform, float pen,
                                     float baseline, float dx, float dy, PanoramaColor run_color) {
            std::size_t i = 0;
            while (i < text.size())
            {
                const char32_t cp = next_codepoint(text, i);
                PanoramaGlyph g;
                if (!glyphs_.glyph(cp, font, weight, g))
                {
                    continue;
                }
                if (g.valid)
                {
                    const float gx = pen + g.bearing_x + dx;
                    const float gy = baseline - g.bearing_y + dy;
                    add_rect(gx, gy, g.width, g.height, run_color, glyphs_.atlas_texture, run_transform, g.u0, g.v0, g.u1, g.v1);
                }
                pen += g.advance + s.letter_spacing;
            }
            return pen;
        };

        // Draws the label's text offset by (dx,dy) in `run_color`: a single
        // continuous pen across the styled runs, or — when the layout solver
        // wrapped — every stored line on its own baseline, aligned per line.
        const auto draw_run = [&](float dx, float dy, PanoramaColor run_color) {
            if (!multiline)
            {
                // Underline paints under the glyphs, line-through over them
                // (WebCore background vs foreground decorations).
                if (s.text_decoration_underline)
                {
                    draw_decoration(pen_x, single_line_width, baseline_y, dx, dy, run_color, false);
                }
                float pen = pen_x;
                for (const TextRun& run : runs)
                {
                    pen = draw_glyphs(
                        run.text, run.weight, run.italic ? italic_transform : transform, pen, baseline_y, dx, dy, run_color);
                }
                if (s.text_decoration_line_through)
                {
                    draw_decoration(pen_x, single_line_width, baseline_y, dx, dy, run_color, true);
                }
                return;
            }
            for (std::size_t li = 0; li < node.text_lines.size(); ++li)
            {
                const PanoramaTextWrapLine& line = node.text_lines[li];
                const float baseline = baseline_y + static_cast<float>(li) * node.text_line_advance;
                float pen = node.layout.content_x;
                if (s.text_align != PanoramaHAlign::Left && available > line.width)
                {
                    const float slack = available - line.width;
                    pen += s.text_align == PanoramaHAlign::Center ? slack * 0.5F : slack;
                }
                const Matrix2D line_italic = italic_about(baseline);
                const float pen_start = pen;
                if (s.text_decoration_underline)
                {
                    draw_decoration(pen_start, line.width, baseline, dx, dy, run_color, false);
                }
                for (const PanoramaTextWrapSegment& seg : line.segments)
                {
                    // Guard against a stale wrap (text mutated since layout).
                    if (seg.run < 0 || static_cast<std::size_t>(seg.run) >= runs.size())
                    {
                        continue;
                    }
                    const TextRun& run = runs[static_cast<std::size_t>(seg.run)];
                    if (seg.begin > run.text.size() || seg.end > run.text.size() || seg.begin >= seg.end)
                    {
                        continue;
                    }
                    pen = draw_glyphs(run.text.substr(seg.begin, seg.end - seg.begin), run.weight,
                        run.italic ? line_italic : transform, pen, baseline, dx, dy, run_color);
                }
                if (s.text_decoration_line_through)
                {
                    draw_decoration(pen_start, line.width, baseline, dx, dy, run_color, true);
                }
            }
        };

        // text-shadow: offset copies of the glyphs behind the text. `strength`
        // multiplies the shadow opacity; a blur radius is approximated with a ring
        // of taps around the offset (no pixel-buffer gaussian in this painter).
        if (s.text_shadow.present)
        {
            // Not scale_alpha: strength may exceed 1 (it saturates the shadow), while
            // scale_alpha clamps its factor to [0,1].
            PanoramaColor shadow = s.text_shadow.color;
            shadow.a = static_cast<std::uint8_t>(std::clamp(
                static_cast<float>(shadow.a) * std::clamp(opacity, 0.0F, 1.0F) * s.text_shadow.strength,
                0.0F, 255.0F) + 0.5F);
            if (shadow.a != 0)
            {
                const float blur_radius = 0.5F * s.text_shadow.blur;
                if (blur_radius > 0.25F)
                {
                    PanoramaColor ring = shadow;
                    ring.a = static_cast<std::uint8_t>(std::clamp(static_cast<float>(shadow.a) * 0.3F, 0.0F, 255.0F) + 0.5F);
                    const float diag = blur_radius * 0.70710678F;
                    const float taps[8][2] = {
                        {blur_radius, 0.0F}, {-blur_radius, 0.0F}, {0.0F, blur_radius}, {0.0F, -blur_radius},
                        {diag, diag}, {diag, -diag}, {-diag, diag}, {-diag, -diag},
                    };
                    for (const auto& tap : taps)
                    {
                        draw_run(s.text_shadow.offset_x + tap[0], s.text_shadow.offset_y + tap[1], ring);
                    }
                }
                draw_run(s.text_shadow.offset_x, s.text_shadow.offset_y, shadow);
            }
        }
        draw_run(0.0F, 0.0F, color);
    }

    PanoramaDrawList& list_;
    const PanoramaGlyphSource& glyphs_;
    PanoramaPaintScratch* scratch_ = nullptr;
    std::size_t reused_command_index_ = 0;
    ClipRect clip_;
    PanoramaBlendMode blend_ = PanoramaBlendMode::Normal;
    const PanoramaNode* document_root_ = nullptr;
};
}

std::size_t PanoramaDrawList::total_vertices() const
{
    std::size_t count = 0;
    for (const PanoramaDrawCommand& cmd : commands)
    {
        count += cmd.vertices.size();
    }
    return count;
}

std::size_t PanoramaDrawList::total_indices() const
{
    std::size_t count = 0;
    for (const PanoramaDrawCommand& cmd : commands)
    {
        count += cmd.indices.size();
    }
    return count;
}

void build_panorama_draw_list(
    PanoramaDrawList& out,
    const PanoramaNode& root,
    const PanoramaGlyphSource& glyphs,
    PanoramaPaintScratch* scratch)
{
    if (scratch != nullptr)
    {
        scratch->reusable_commands.clear();
        scratch->reusable_commands.swap(out.commands);
        out.commands.reserve(scratch->reusable_commands.size());
    }
    else
    {
        out.commands.clear();
    }

    DrawListBuilder builder(out, glyphs, scratch);
    builder.set_document_root(&root);
    builder.paint(root, 1.0F, Matrix2D{}, ClipRect{});
    builder.paint_dropdown_popups(root, 1.0F);
}

PanoramaDrawList build_panorama_draw_list(const PanoramaNode& root, const PanoramaGlyphSource& glyphs)
{
    PanoramaDrawList list;
    build_panorama_draw_list(list, root, glyphs, nullptr);
    return list;
}
}
