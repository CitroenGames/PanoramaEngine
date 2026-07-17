#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Panorama style model: the value types, the computed-style struct, and the
// stylesheet/cascade machinery used to assign a ComputedStyle to every node in a
// PanoramaNode tree.
//
// We own the property set, so Panorama's own units (fit-children,
// fill-parent-flow, width/height-percentage) and box model are first-class.
namespace panorama
{
struct PanoramaNode;

// How a width/height (or related length) is expressed. Panorama mixes ordinary
// CSS units with its own intrinsic/flow units.
enum class PanoramaLengthType
{
    Auto,           // unset: resolved per node kind (containers fill, leaves fit)
    FitChildren,    // shrink to fit content (explicit `fit-children`)
    Pixels,         // absolute px
    Percent,        // percent of the parent content box in the SAME axis
    WidthPercent,   // width-percentage(N%): N% of this element's OWN resolved width (used on the height axis)
    HeightPercent,  // height-percentage(N%): N% of this element's OWN resolved height (used on the width axis; Panorama's square-icon idiom, cf. WebCore aspect-ratio)
    FillParentFlow, // fill-parent-flow(ratio): share leftover flow space by ratio
};

struct PanoramaLength
{
    PanoramaLengthType type = PanoramaLengthType::Auto;
    float value = 0.0F; // px for Pixels; the percentage number for *Percent; the ratio for FillParentFlow

    [[nodiscard]] bool is_definite() const noexcept
    {
        return type == PanoramaLengthType::Pixels || type == PanoramaLengthType::Percent ||
               type == PanoramaLengthType::WidthPercent || type == PanoramaLengthType::HeightPercent;
    }
};

struct PanoramaColor
{
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 0; // default fully transparent

    [[nodiscard]] bool visible() const noexcept { return a != 0; }
};

enum class PanoramaFlow
{
    None,
    Right,
    Left,
    Down,
    Up,
    RightWrap,
    Down_Wrap,
};

enum class PanoramaHAlign
{
    Left,
    Center,
    Right,
};

enum class PanoramaVAlign
{
    Top,
    Middle,
    Bottom,
};

enum class PanoramaTextTransform
{
    None,
    Uppercase,
    Lowercase,
};

// Compositing mode for an element against the backdrop (CSS mix-blend-mode /
// Panorama -mix-blend-mode). Source colours are premultiplied.
enum class PanoramaBlendMode
{
    Normal,   // premultiplied over
    Additive, // src + dst (CS:GO srgbadditive/additive)
    Screen,   // src + dst*(1-src)
    Multiply, // src * dst
    Opaque,   // src, ignoring dst
};

// How text that does not fit its constrained box is handled. Ellipsis truncates
// before paint, Shrink uses the text-provider-computed reduced font size, Clip
// installs a content scissor, and NoClip renders overflow.
enum class PanoramaTextOverflow
{
    Clip,
    Ellipsis,
    Shrink,
    NoClip,
};

// A CSS timing function. Represented uniformly as a cubic-bezier with implicit
// endpoints (0,0) and (1,1); the keyword curves (ease/ease-in/ease-out/
// ease-in-out) and `cubic-bezier(x1,y1,x2,y2)` are just specific control points
// (see parse_easing). `linear` fast-paths the identity curve so the common
// no-easing transition skips the solver. `evaluate(t)` maps progress t in [0,1]
// to the eased output fraction via a Newton/bisection solve of the bezier
// (ported from WebKit's UnitBezier).
struct PanoramaEasing
{
    float x1 = 0.0F;
    float y1 = 0.0F;
    float x2 = 1.0F;
    float y2 = 1.0F;
    bool linear = true;

    [[nodiscard]] float evaluate(float t) const;
};

// `animation-direction`: which way each iteration plays.
enum class PanoramaAnimDirection
{
    Normal,
    Reverse,
    Alternate,
    AlternateReverse,
};

// `animation-fill-mode`: which keyframe value (if any) is retained outside the
// active interval (before `animation-delay` elapses / after the run finishes).
enum class PanoramaAnimFillMode
{
    None,
    Forwards,
    Backwards,
    Both,
};

// Animatable keyframe channels. A stop declaring `x`/`y`/`z`/`position` sets the
// single Position channel (all three coordinates interpolate together, mirroring
// the CSS-transition `position` handling).
enum PanoramaAnimChannel : std::uint32_t
{
    PanoramaAnimOpacity = 1u << 0,
    PanoramaAnimBrightness = 1u << 1,
    PanoramaAnimBackgroundColor = 1u << 2,
    PanoramaAnimColor = 1u << 3,
    PanoramaAnimWashColor = 1u << 4,
    PanoramaAnimTransform = 1u << 5,
    PanoramaAnimPosition = 1u << 6,
    PanoramaAnimBackgroundImageOpacity = 1u << 7,
    PanoramaAnimBoxShadow = 1u << 8,
    PanoramaAnimBlur = 1u << 9,
    PanoramaAnimClip = 1u << 10,
};

// One parsed `transition-*` entry: animate `property` over `duration` seconds
// after `delay` seconds using `easing`. `property` == "all" matches any.
struct PanoramaTransition
{
    std::string property;
    float duration = 0.0F;
    float delay = 0.0F;
    PanoramaEasing easing{};
};

struct PanoramaTransformOp
{
    enum class Type
    {
        Translate,
        Scale,
        Rotate,
    };

    Type type = Type::Translate;
    float x = 0.0F;
    float y = 0.0F;
    bool x_percent = false;
    bool y_percent = false;
};

struct PanoramaTransform
{
    std::vector<PanoramaTransformOp> ops;

    [[nodiscard]] bool empty() const noexcept { return ops.empty(); }
};

struct PanoramaTransformOrigin
{
    float x = 50.0F;
    float y = 50.0F;
    bool x_percent = true;
    bool y_percent = true;
};

// How a background image is scaled into its box (CSS background-size / object-fit).
enum class PanoramaBackgroundSizeType
{
    // Natural image size on both axes (`auto auto`). This is CSS/Panorama's
    // documented default: the painter draws the texture at its intrinsic pixel
    // size (background_texture_natural_width/height), falling back to box-fill
    // when that size is not yet known.
    Auto,
    // Fill the box on both axes (`100% 100%`).
    Stretch,
    Contain, // scale to fit inside the box, preserving aspect (letterboxed)
    Cover,   // scale to cover the box, preserving aspect (cropped)
    Fixed,   // explicit width/height (px or % of the box); Auto length = aspect-derived
};

struct PanoramaBackgroundSize
{
    PanoramaBackgroundSizeType type = PanoramaBackgroundSizeType::Auto;
    PanoramaLength width;  // used when type == Fixed
    PanoramaLength height; // used when type == Fixed
};

// CSS background-position: a fraction (percent) or pixel offset on each axis.
struct PanoramaBackgroundPosition
{
    float x = 0.0F;
    float y = 0.0F;
    bool x_percent = true; // percent aligns p% of the image to p% of the box
    bool y_percent = true;
    bool x_from_end = false;    // side-offset syntax: `right 32px`
    bool y_from_end = false;    // side-offset syntax: `bottom 10px`
    bool x_side_offset = false; // true when x is a distance from left/right, not percent alignment
    bool y_side_offset = false; // true when y is a distance from top/bottom, not percent alignment
};

// CSS background-repeat tiling mode for one axis (WebCore FillRepeat).
enum class PanoramaBackgroundRepeat
{
    Repeat,   // tile the image across the whole box (the CSS default)
    NoRepeat, // a single tile at the computed background-position
    Space,    // whole tiles only; the leftover is distributed as gaps between them
    Round,    // tile rescaled so a whole number of tiles fits exactly
};

struct PanoramaBackgroundRepeatXY
{
    PanoramaBackgroundRepeat x = PanoramaBackgroundRepeat::Repeat;
    PanoramaBackgroundRepeat y = PanoramaBackgroundRepeat::Repeat;
};

enum class PanoramaGradientType
{
    None,
    Linear,
    Radial,
};

struct PanoramaGradientStop
{
    float offset = 0.0F;
    PanoramaColor color;
};

struct PanoramaGradient
{
    PanoramaGradientType type = PanoramaGradientType::None;
    float x0 = 0.0F;
    float y0 = 0.0F;
    float x1 = 100.0F;
    float y1 = 0.0F;
    // Radial shape (Panorama: `gradient(radial, cx cy, ox oy, rx ry, stops...)`).
    // All percentages of the box: ellipse centred at (cx+ox, cy+oy) with radii rx/ry.
    float radial_center_x = 50.0F;
    float radial_center_y = 50.0F;
    float radial_offset_x = 0.0F;
    float radial_offset_y = 0.0F;
    float radial_radius_x = 50.0F;
    float radial_radius_y = 50.0F;
    std::vector<PanoramaGradientStop> stops;

    [[nodiscard]] bool present() const noexcept
    {
        return type != PanoramaGradientType::None && stops.size() >= 2;
    }
};

// A drop shadow for text (Panorama: `text-shadow: h v [blur] [strength] color`).
// Blur is approximated with a small ring of offset glyph copies (no pixel-buffer
// gaussian); `strength` scales the shadow's opacity the way Panorama's strength
// multiplier darkens it. A zero-blur shadow renders as a single hard offset copy.
struct PanoramaTextShadow
{
    bool present = false;
    float offset_x = 0.0F;
    float offset_y = 0.0F;
    float blur = 0.0F;
    float strength = 1.0F;
    PanoramaColor color;
};

// A box drop shadow (CSS/Panorama box-shadow: `[fill|inset] color h v blur spread`,
// colour-last CSS order also accepted). The blur is approximated by a linear alpha
// falloff (no GPU gaussian). Plain (outset) shadows are knocked out under the border
// box like WebCore's paintBoxShadow, so translucent panels don't darken; `fill`
// draws the shadow under the panel as well; `inset` draws an inner-edge shadow.
struct PanoramaBoxShadow
{
    bool present = false;
    bool inset = false;
    bool fill = false; // Panorama `fill`: shadow also paints under the panel
    float offset_x = 0.0F;
    float offset_y = 0.0F;
    float blur = 0.0F;
    float spread = 0.0F;
    PanoramaColor color;
};

// Panorama `blur: gaussian(stdX px, stdY px, passes)` — a backdrop blur. std_x/std_y
// are the gaussian standard deviations (px); `passes` is the iteration count (may be
// fractional). Parsed + stored here; the actual GPU blur pass is a separate (visual)
// piece of work. `present == false` means no blur.
struct PanoramaBlur
{
    bool present = false;
    float std_x = 0.0F;
    float std_y = 0.0F;
    float passes = 1.0F;
};

// Panorama `clip:` — a render-time clip region on the panel (does not affect
// layout or hit testing). Two forms, both animatable (SpinnerRotate, countdown
// wipes, the radial radio menu):
//   clip: rect( top, right, bottom, left )      edges of the VISIBLE rect in %
//                                               of the border box (CSS rect order)
//   clip: radial( cx cy, start, sweep )         hides the wedge swept CLOCKWISE
//                                               from `start` (0deg = 12 o'clock)
//                                               over `sweep` degrees about the
//                                               centre (% of the box). (0,0) =
//                                               fully visible, (0,360) = fully
//                                               hidden. Ground truth: CS:GO's
//                                               RadialRadioSegment slices — each
//                                               hides 315.04deg leaving its 45deg
//                                               slice visible at start-22.5deg.
enum class PanoramaClipType
{
    None,
    Rect,
    Radial,
};

struct PanoramaClip
{
    PanoramaClipType type = PanoramaClipType::None;
    // rect(): visible-edge positions in % of the border box.
    float rect_top = 0.0F;
    float rect_right = 100.0F;
    float rect_bottom = 100.0F;
    float rect_left = 0.0F;
    // radial(): hidden-wedge centre (% of the box) + start/sweep in degrees.
    float radial_center_x = 50.0F;
    float radial_center_y = 50.0F;
    float radial_start = 0.0F;
    float radial_sweep = 0.0F;
};

// CSS custom-property map with copy-on-write storage (WebCore's RenderStyle keeps
// inherited substructures behind DataRef the same way). Children inherit the
// parent's map by sharing the same storage; the map is cloned only when a node
// declares (or erases) its own custom property, so the common case — thousands of
// nodes inheriting a handful of root-level `--vars` — costs one shared_ptr copy
// per node instead of a full map copy.
class PanoramaCustomProperties
{
public:
    using Map = std::unordered_map<std::string, std::string>;

    [[nodiscard]] bool empty() const noexcept { return map_ == nullptr || map_->empty(); }

    // Returns the value for `name`, or null if not declared.
    [[nodiscard]] const std::string* find(const std::string& name) const
    {
        if (map_ == nullptr)
        {
            return nullptr;
        }
        const auto it = map_->find(name);
        return it != map_->end() ? &it->second : nullptr;
    }

    void set(const std::string& name, std::string value) { detach()[name] = std::move(value); }

    void erase(const std::string& name)
    {
        if (map_ == nullptr || map_->find(name) == map_->end())
        {
            return; // nothing to erase; do not detach shared storage
        }
        detach().erase(name);
    }

    // True when both maps share identical storage (or are both empty). Used as a
    // cheap inherited-state equality test, like WebCore's inheritedDataShared().
    [[nodiscard]] bool shares_storage(const PanoramaCustomProperties& other) const noexcept
    {
        return map_ == other.map_ || (empty() && other.empty());
    }

    // Stable identity of the underlying storage, for hashing.
    [[nodiscard]] const void* storage_key() const noexcept { return empty() ? nullptr : map_.get(); }

    // CPUMT-49: read-only content access (as opposed to storage_key()'s identity),
    // for equivalence tests that must compare two independently-computed styles'
    // custom-property VALUES rather than whether they happen to share storage.
    [[nodiscard]] const Map& entries() const noexcept
    {
        static const Map kEmpty;
        return map_ != nullptr ? *map_ : kEmpty;
    }

private:
    [[nodiscard]] Map& detach()
    {
        if (map_ == nullptr)
        {
            map_ = std::make_shared<Map>();
        }
        else if (map_.use_count() > 1)
        {
            map_ = std::make_shared<Map>(*map_);
        }
        return *map_;
    }

    std::shared_ptr<Map> map_; // null = empty
};

// Edge metrics in pixels, order: top, right, bottom, left (CSS order).
struct PanoramaEdges
{
    float top = 0.0F;
    float right = 0.0F;
    float bottom = 0.0F;
    float left = 0.0F;
};

// Bit positions for PanoramaComputedStyle::margin_pct_mask (shared by the style
// parser and the layout's percent-margin resolution).
inline constexpr std::uint8_t kPanoramaMarginTopPct = 1 << 0;
inline constexpr std::uint8_t kPanoramaMarginRightPct = 1 << 1;
inline constexpr std::uint8_t kPanoramaMarginBottomPct = 1 << 2;
inline constexpr std::uint8_t kPanoramaMarginLeftPct = 1 << 3;

struct PanoramaComputedStyle
{
    PanoramaLength width;
    PanoramaLength height;
    PanoramaLength min_width{PanoramaLengthType::Pixels, 0.0F};
    PanoramaLength max_width;  // Auto here means "no max"
    PanoramaLength min_height{PanoramaLengthType::Pixels, 0.0F};
    PanoramaLength max_height; // Auto here means "no max"

    PanoramaEdges margin;
    // Percentage margins (e.g. `.wedge { margin-left: 50% }`) resolve against the
    // containing block's WIDTH at layout time (CSS rule), so the authored percent
    // numbers are kept here and folded into `margin` (px) by the layout — never
    // overwritten by the layout, so a relayout without a fresh cascade still
    // resolves correctly. A set bit in margin_pct_mask marks that edge as percent.
    PanoramaEdges margin_pct;
    std::uint8_t margin_pct_mask = 0; // bit 0=top, 1=right, 2=bottom, 3=left
    PanoramaEdges padding;

    PanoramaFlow flow = PanoramaFlow::None;
    PanoramaHAlign halign = PanoramaHAlign::Left;
    PanoramaVAlign valign = PanoramaVAlign::Top;

    bool has_position = false;
    float pos_x = 0.0F;
    float pos_y = 0.0F;
    float pos_z = 0.0F;
    bool pos_x_percent = false; // position x given as % of parent content width
    bool pos_y_percent = false; // position y given as % of parent content height

    PanoramaColor background_color;
    std::string background_image;
    float background_image_opacity = 1.0F; // Panorama background-img-opacity, not inherited
    PanoramaGradient background_gradient;
    PanoramaBackgroundSize background_size;
    PanoramaBackgroundPosition background_position;
    PanoramaBackgroundRepeatXY background_repeat;
    PanoramaColor color{0xF2, 0xF2, 0xF2, 0xFF};
    PanoramaColor border_color;
    // Image/icon tint (multiplies the texture), inherited; `none` -> white = as-is.
    PanoramaColor wash_color{0xFF, 0xFF, 0xFF, 0xFF};
    float brightness = 1.0F;  // inherited per-element RGB multiplier (fills/gradient/border/text/wash)
    float saturation = 1.0F;  // inherited per-element HSV saturation scale (1=identity, 0=grey, <0 invert)
    PanoramaBoxShadow box_shadow; // not inherited
    PanoramaBlur blur;            // backdrop blur (parsed; GPU pass pending), not inherited
    PanoramaClip clip;            // render-time clip region (rect/radial wipes), not inherited
    float border_width = 0.0F;
    // Per-side borders (border-top/-right/-bottom/-left and their -width/-color
    // longhands; WebCore paints each side as its own edge band). When
    // border_per_side is false the uniform border_width/border_color above are
    // authoritative and the side fields mirror them lazily at paint time.
    bool border_per_side = false;
    float border_width_top = 0.0F;
    float border_width_right = 0.0F;
    float border_width_bottom = 0.0F;
    float border_width_left = 0.0F;
    PanoramaColor border_color_top;
    PanoramaColor border_color_right;
    PanoramaColor border_color_bottom;
    PanoramaColor border_color_left;
    [[nodiscard]] float border_top() const noexcept { return border_per_side ? border_width_top : border_width; }
    [[nodiscard]] float border_right() const noexcept { return border_per_side ? border_width_right : border_width; }
    [[nodiscard]] float border_bottom() const noexcept { return border_per_side ? border_width_bottom : border_width; }
    [[nodiscard]] float border_left() const noexcept { return border_per_side ? border_width_left : border_width; }
    [[nodiscard]] PanoramaColor border_top_color() const noexcept
    {
        return border_per_side ? border_color_top : border_color;
    }
    [[nodiscard]] PanoramaColor border_right_color() const noexcept
    {
        return border_per_side ? border_color_right : border_color;
    }
    [[nodiscard]] PanoramaColor border_bottom_color() const noexcept
    {
        return border_per_side ? border_color_bottom : border_color;
    }
    [[nodiscard]] PanoramaColor border_left_color() const noexcept
    {
        return border_per_side ? border_color_left : border_color;
    }
    float border_radius = 0.0F;        // px, or a percentage value when border_radius_percent
    bool border_radius_percent = false; // border-radius: 50% (e.g. circular avatars)
    // Asymmetric `border-radius: a b c d` (CSS corner order TL TR BR BL, px only;
    // a percentage form stays on the uniform fields above). Paint constrains the
    // four radii per WebCore FloatRoundedRect so adjacent corners never overlap.
    bool border_radius_per_corner = false;
    float border_radius_tl = 0.0F;
    float border_radius_tr = 0.0F;
    float border_radius_br = 0.0F;
    float border_radius_bl = 0.0F;

    float font_size = 18.0F;
    // Inherited text properties.
    int font_weight = 400;
    // font-style: italic ("italics" in Panorama CSS). Rendered as a synthetic
    // oblique (WebCore FontCascade::syntheticObliqueAngle() == 14 degrees).
    bool font_italic = false;
    PanoramaTextTransform text_transform = PanoramaTextTransform::None;
    PanoramaHAlign text_align = PanoramaHAlign::Left; // alignment of text within its content box
    // white-space: nowrap — suppress word wrapping (labels wrap to their content
    // width by default, as in real Panorama). Inherited (CSS white-space).
    bool white_space_nowrap = false;
    float letter_spacing = 0.0F; // extra px added after each glyph's advance
    float line_height = 0.0F;    // px line-box height; 0 = auto (font metrics). Inherited.
    PanoramaTextShadow text_shadow; // inherited
    // Panorama img-shadow: a drop shadow under an Image panel's texture, rendered
    // as an offset silhouette (texture alpha x shadow colour). Not inherited.
    PanoramaTextShadow img_shadow;
    PanoramaTextOverflow text_overflow = PanoramaTextOverflow::Clip; // not inherited
    // CSS text-decoration line flags. Not inherited (WebCore decorations
    // propagate by painting across descendants, not by style inheritance;
    // labels here are leaf text nodes, so per-node flags suffice).
    bool text_decoration_underline = false;
    bool text_decoration_line_through = false;
    float opacity = 1.0F;
    int z_index = 0; // paint order among siblings (higher = on top); default 0
    bool visible = true; // visibility: collapse / .hidden -> false
    // overflow per axis: true = clip children to this box (squish/scroll/clip),
    // false = noclip. Not inherited. Panorama's DEFAULT is `squish` — children
    // are confined to the box — which is why CS:GO styles opt OUT with explicit
    // `overflow: noclip` (e.g. .map-selection-btn__top-icon-row) and why the
    // play menu's presets sidebar fully disappears when its width animates to 0.
    bool overflow_clip_x = true;
    bool overflow_clip_y = true;
    // squish additionally SHRINKS oversized children to the parent's definite
    // content box instead of merely clipping their paint (the 96px navbar tabs
    // squish into their 64px `--short` row and keep their selection underline).
    bool overflow_squish_x = true;
    bool overflow_squish_y = true;
    // scroll makes the axis SCROLLABLE (WebCore overflow:scroll): children keep
    // their natural size (no squish), paint clips, and a persistent per-node
    // scroll offset shifts them. CS:GO's `.vscroll` (settings tabs, news list,
    // chat history) is `overflow: squish scroll`.
    bool overflow_scroll_x = false;
    bool overflow_scroll_y = false;
    PanoramaBlendMode blend_mode = PanoramaBlendMode::Normal; // -mix-blend-mode, not inherited
    PanoramaTransform transform;
    PanoramaTransformOrigin transform_origin;
    // Panorama pre-transform-scale2d: a scale about the transform-origin applied
    // before `transform` (hover-zoom). 1 = none. Not inherited.
    float pre_scale_x = 1.0F;
    float pre_scale_y = 1.0F;
    // Panorama ui-scale / ui-scale-x / ui-scale-y: a scale about the transform-
    // origin, composed innermost like pre-transform-scale2d. 1 = none. Not inherited.
    float ui_scale_x = 1.0F;
    float ui_scale_y = 1.0F;
    // Panorama pre-transform-rotate2d: a rotation (degrees) about the transform-
    // origin applied before `transform`. 0 = none. Not inherited.
    float pre_rotate = 0.0F;

    // Parsed `transition-*` longhands as parallel lists (CSS repeats shorter lists).
    std::vector<std::string> transition_properties;
    std::vector<float> transition_durations;
    std::vector<float> transition_delays;
    std::vector<PanoramaEasing> transition_easings;

    // `animation-*` longhands. `animation_name` empty / "none" means no @keyframes
    // animation. `animation_iteration_count` < 0 means infinite.
    std::string animation_name;
    float animation_duration = 0.0F;
    float animation_delay = 0.0F;
    float animation_iteration_count = 1.0F;
    // CSS initial animation-timing-function is `ease` (cubic-bezier(.25,.1,.25,1)),
    // not linear (WebCore Animation::initialTimingFunction).
    PanoramaEasing animation_easing{0.25F, 0.1F, 0.25F, 1.0F, false};
    PanoramaAnimDirection animation_direction = PanoramaAnimDirection::Normal;
    PanoramaAnimFillMode animation_fill_mode = PanoramaAnimFillMode::None;

    // CSS custom properties (`--name`) inherit and resolve `var(--name, fallback)`
    // at computed-value time. Values remain token strings because Panorama's typed
    // property parsers consume the final substituted value per property. Storage is
    // copy-on-write: inheriting shares the parent's map.
    PanoramaCustomProperties custom_properties;

    // Fills `out` with the transition covering `property` (or an "all" entry) and
    // returns true if one exists with a positive duration.
    [[nodiscard]] bool find_transition(std::string_view property, PanoramaTransition& out) const;
};

// ---- selectors / rules -------------------------------------------------------

enum class PanoramaAttributeMatch
{
    Exists,
    Exact,
    Includes,
    DashMatch,
    Prefix,
    Suffix,
    Substring,
};

struct PanoramaAttributeSelector
{
    std::string name; // lowercased attribute name
    PanoramaAttributeMatch match = PanoramaAttributeMatch::Exists;
    std::string value;
    bool case_insensitive = false;
};

struct PanoramaSelector;

// One compound selector segment, e.g. `Button.foo#bar:hover`.
struct PanoramaSimpleSelector
{
    bool universal = false;
    std::string type;                  // lowercased tag, empty if none
    std::string id;                    // without '#', case-preserved
    std::vector<std::string> classes;  // without '.', case-preserved
    std::vector<PanoramaAttributeSelector> attributes;
    std::vector<std::string> pseudos;  // without ':' (hover, active, selected, ...)
    // Each :not(...) contributes one selector-list group. The group matches if any
    // alternative matches; its specificity is the maximum alternative specificity.
    std::vector<std::vector<std::shared_ptr<PanoramaSelector>>> not_selector_groups;
};

enum class PanoramaCombinator
{
    Descendant,
    Child,
    AdjacentSibling,
    GeneralSibling,
};

// A descendant chain of compounds; the last element is the subject (the matched
// node), earlier elements must match ancestors.
struct PanoramaSelector
{
    std::vector<PanoramaSimpleSelector> compounds;
    std::vector<PanoramaCombinator> combinators; // relationship compounds[i] -> compounds[i + 1]
    // Precomputed (ids, classes+pseudos, types) specificity, filled at parse time
    // so the cascade does not recompute it for every matching node.
    std::array<int, 3> specificity{0, 0, 0};
    // SelectorFilter fast-reject hashes (WebCore css/SelectorFilter.{h,cpp}):
    // salted hashes of identifiers this selector requires on some ANCESTOR of the
    // subject (compounds left of a descendant/child combinator; compounds left of
    // sibling combinators are siblings, not ancestors, and contribute nothing).
    // 0-terminated; filled at parse time. If every required hash is absent from
    // the cascade's ancestor bloom filter, the selector cannot match and the full
    // (ancestor-walking) match test is skipped.
    std::array<std::uint32_t, 4> ancestor_hashes{0, 0, 0, 0};
};

struct PanoramaDeclaration
{
    std::string property; // lowercased, except CSS custom properties preserve case
    std::string value;    // trimmed, raw
    bool important = false;
    // `value` with `@define`d identifiers substituted. Computed once when the sheet
    // is finalized (handles forward references) so the hot cascade path skips the
    // per-node string rescan that `resolve_value` would otherwise do.
    std::string resolved_value;
};

struct PanoramaRule
{
    std::vector<PanoramaSelector> selectors;
    std::vector<PanoramaDeclaration> declarations;
    int source_order = 0;
    // Index into the sheet's per-source layout-scope sets (see add_source).
    std::uint16_t source_index = 0;
};

// One `@keyframes` stop. `offset` is in [0,1] (`from`==0, `to`==1, `N%`==N/100).
// `easing`/`has_easing` come from an `animation-timing-function` declaration
// inside the stop and govern the segment that STARTS at this stop (CSS semantics).
// `resolved`/`channels` are derived once the sheet is finalized (after @define
// substitution): `resolved` holds the typed values, `channels` flags which
// properties this stop actually declares.
struct PanoramaKeyframeStop
{
    float offset = 0.0F;
    std::vector<PanoramaDeclaration> declarations;
    bool has_easing = false;
    PanoramaEasing easing{};

    PanoramaComputedStyle resolved;
    std::uint32_t channels = 0;
};

// A parsed `@keyframes <name>` rule: stops sorted by offset, plus the union of
// channels declared across all stops (so the runtime iterates only live channels).
struct PanoramaKeyframes
{
    std::vector<PanoramaKeyframeStop> stops;
    std::uint32_t channels = 0;
};

class PanoramaStyleSheet
{
public:
    PanoramaStyleSheet() = default;
    // Non-copyable: caches key on (instance id, generation); a copy would alias the
    // identity and could serve stale styles after the copies diverge.
    PanoramaStyleSheet(const PanoramaStyleSheet&) = delete;
    PanoramaStyleSheet& operator=(const PanoramaStyleSheet&) = delete;

    // The implicit layout scope of the root document: rules carrying it apply
    // to EVERY node (matching Valve, where the document's own sheets style the
    // whole tree). Hosts that don't use layout scoping just call add_source
    // with the default and get the old global behaviour.
    static constexpr std::uint16_t kRootLayoutScope = 1;

    // Parses a Panorama CSS source and appends its rules. May be called multiple
    // times to accumulate several stylesheets; source order is preserved across
    // calls so later sheets win ties.
    //
    // `layout_scope` records WHICH layout file included this sheet: real
    // Panorama scopes a layout's <styles> to the panels that layout creates
    // (CS:GO's hudmoney.xml includes buymenu.css, whose `#HudBottomRight
    // { padding-bottom: 96px; }` must not reach the HUD's own panel — the real
    // client renders the weapon panel flush to the bottom). A rule applies to a
    // node when its sheet's scope set contains kRootLayoutScope or the
    // style_scope_mark of the node or one of its ancestors. Returns the source
    // index for add_source_scope.
    std::uint16_t add_source(std::string_view css, std::uint16_t layout_scope = kRootLayoutScope);

    // Re-include of an already-parsed sheet by ANOTHER layout: widens the
    // sheet's scope set without reparsing (its cascade position is unchanged —
    // the first include wins ties, which is also why csgostyles.css re-included
    // by every CS:GO module must NOT be re-appended after the module's own
    // sheet).
    void add_source_scope(std::uint16_t source_index, std::uint16_t layout_scope);

    // Drops all rules/defines/keyframes and bumps the generation so dependent
    // caches revalidate. The non-copyable identity (instance id) is kept — use
    // this instead of assigning a fresh sheet.
    void clear();

    [[nodiscard]] const std::vector<PanoramaRule>& rules() const noexcept { return rules_; }

    // Parsed `@keyframes` rules, keyed by the normalized CSS identifier/string
    // name. The keyframe runtime reads this to drive `animation-name` panels.
    [[nodiscard]] const std::unordered_map<std::string, PanoramaKeyframes>& keyframes() const noexcept
    {
        return keyframes_;
    }

    // Substitutes `@define`d variables referenced as bare identifiers in a value
    // (e.g. `blurBackgroundColor` -> `rgba(40, 40, 40, 0.3)`), recursively. Public
    // for testing.
    [[nodiscard]] std::string resolve_value(std::string_view value) const;

    // Computes and stores a ComputedStyle on every node in the tree rooted at
    // `root`, applying the cascade (specificity + source order) plus each node's
    // inline style. Pseudo-class rules (:hover/:active/...) are skipped here; only
    // the base state is resolved.
    void compute(PanoramaNode& root) const;

    // Partial recompute honoring PanoramaNode::mark_style_dirty: only marked
    // subtrees are recomputed (a marked node's whole subtree, because descendant
    // combinators let its state affect any descendant; plus, when the sheet uses
    // sibling combinators, the marked node's following siblings). Clean subtrees
    // keep their computed styles — callers must have marked every node whose
    // cascade inputs changed (input hit-testing and the DOM helpers do). Focus
    // changes must use compute() instead: :focus-within lets a descendant's focus
    // alter ANCESTOR matches, which marking does not model.
    void compute_invalidated(PanoramaNode& root) const;

    // Style sharing (WebCore style/StyleSharingResolver): a node whose immediately
    // preceding sibling carries identical style-affecting state (tag, id, classes,
    // attributes, inline style, pseudo-class state and scope) produces a
    // byte-identical computed style — siblings share the same parent (hence the
    // same inherited inputs) and the same ancestor chain (hence identical
    // descendant/child-combinator matches). compute() then copies that style
    // outright instead of gathering candidates and re-matching. Enabled by default;
    // a test can disable it to assert the shared path matches the matched-from-
    // scratch path, and PanoramaDiagnostics::disable_style_sharing disables it
    // process-wide for diagnostic A/B comparisons.
    void set_style_sharing_enabled(bool enabled) { style_sharing_enabled_ = enabled; }

    // CPUMT-49: split entry points for forking a full compute() across worker
    // threads. HOST THREAD ONLY, and must complete (including seed_sharing_flags,
    // which these two call between them) BEFORE any worker starts — sharing_active_/
    // sharing_focus_within_active_ are mutable, non-thread_local members; a worker
    // calling compute()/compute_invalidated()/seed_sharing_flags() concurrently with
    // another thread would race on them. Workers must call ONLY
    // compute_forked_subtree() below.
    //
    // compute_root_style computes ONLY `root`'s own style (seeding the ancestor
    // filter + sharing flags exactly like compute(), but skipping the recursion
    // into children) so the caller can then fork compute_forked_subtree() over
    // root's own children.
    void compute_root_style(PanoramaNode& root) const;

    // CPUMT-49: computes `node`'s entire subtree (itself + every descendant),
    // safe to call concurrently for DISJOINT subtrees from worker threads once
    // compute_root_style has already run on the host thread. Primes THIS
    // THREAD's (thread_local) ancestor filter by walking node's real parent
    // chain, so concurrent calls never share filter state. Always passes
    // prev_sibling=nullptr for `node` itself: style sharing across the fork
    // boundary (i.e. between the root's own children, which may be computed by
    // different workers, or in an order the serial pass would not produce) is
    // intentionally not attempted — output-identical either way, since sharing
    // is only ever a shortcut for output full computation would produce
    // identically, never required for correctness. Sharing WITHIN `node`'s own
    // subtree (between its children, grandchildren, ...) is unaffected and
    // still applies normally.
    void compute_forked_subtree(PanoramaNode& node) const;

private:
    void compute_node(PanoramaNode& node, const PanoramaNode* prev_sibling = nullptr) const;
    // CPUMT-49: compute_node's actual body, parameterized on whether to recurse
    // into children. compute_node(node, prev) == compute_node_impl(node, prev,
    // true) — behavior-identical to the pre-CPUMT-49 compute_node for every
    // existing caller. compute_root_style calls this with recurse=false.
    void compute_node_impl(PanoramaNode& node, const PanoramaNode* prev_sibling, bool recurse) const;
    // True when `node` can reuse `candidate`'s (a preceding sibling, already
    // computed this pass) computed style. Conservative: any uncertainty returns
    // false and the node is matched normally.
    [[nodiscard]] bool can_share_style(const PanoramaNode& node, const PanoramaNode& candidate) const;
    // Derives the per-pass style-sharing flags (sharing_active_ /
    // sharing_focus_within_active_) before a compute()/compute_invalidated() walk.
    void seed_sharing_flags(const PanoramaNode& root) const;
    void compute_invalidated_node(PanoramaNode& node) const;
    std::string resolve_value_impl(std::string_view value, int depth) const;

    // Buckets a rule (by its subject compound's most selective component) so the
    // cascade only tests rules that can possibly match a given node, instead of the
    // whole sheet. Stores rule indices (stable across rules_ reallocation).
    void index_rule(int rule_index);
    // Re-substitutes @defines into every declaration's `resolved_value`. Run after
    // each add_source so a later sheet's @define still affects earlier rules. Also
    // (re)derives each keyframe stop's `resolved` style and `channels`.
    void resolve_all_values();

    std::vector<PanoramaRule> rules_;
    // Per-add_source layout-scope sets, indexed by PanoramaRule::source_index.
    std::vector<std::vector<std::uint16_t>> source_scopes_;
    std::unordered_map<std::string, std::string> defines_; // @define name -> value
    std::unordered_map<std::string, PanoramaKeyframes> keyframes_; // @keyframes name -> stops
    int next_source_order_ = 0;
    // Bumped by every add_source: parsed-declaration caches keyed on a (sheet,
    // generation) pair (e.g. each node's inline-style cache) revalidate when the
    // sheet content — including @defines, which alter resolved values — changes.
    std::uint64_t generation_ = 0;
    // Never-reused identity for caches that outlive any one sheet (a destroyed
    // sheet's address can be recycled; this cannot).
    [[nodiscard]] static std::uint64_t next_sheet_instance_id() noexcept;
    std::uint64_t instance_id_ = next_sheet_instance_id();

    // True when any selector (including :not arguments) uses a sibling
    // combinator: a node's state change can then affect FOLLOWING siblings, so
    // compute_invalidated widens dirty marks to them. Derived in add_source.
    // Also disables style sharing: two siblings with identical state can still get
    // different styles via `.a + .b` / `.a ~ .b`.
    bool has_sibling_rules_ = false;

    // True when any selector uses :focus-within (a.k.a. descendantfocus): a node's
    // computed style then depends on whether a DESCENDANT is focused, which is not
    // captured by the sibling-local style-sharing comparison. Derived in add_source.
    bool has_focus_within_rules_ = false;

    // Style-sharing master switch and the per-pass diagnostic/sharing flags
    // compute() seeds before recursing (mutable: compute() is const, mirroring the
    // thread_local cascade scratch this class already keeps).
    bool style_sharing_enabled_ = true;
    mutable bool style_index_disabled_ = false;        // full rule scan for this pass
    mutable bool sharing_active_ = false;              // sharing in effect for this pass
    mutable bool sharing_focus_within_active_ = false; // a focus-within rule exists AND a node is focused

    // Selector-matching acceleration index (subject component -> candidate rules).
    std::unordered_map<std::string, std::vector<int>> rules_by_id_;
    std::unordered_map<std::string, std::vector<int>> rules_by_class_;
    std::unordered_map<std::string, std::vector<int>> rules_by_type_;
    std::vector<int> rules_universal_; // subject has no id/class/type (e.g. `*`, `:hover`)
};

// Parses a single declaration value into the relevant computed field. Exposed for
// inline-style application and unit tests.
PanoramaLength parse_panorama_length(std::string_view value);
PanoramaColor parse_panorama_color(std::string_view value);

// Canonical property metadata for animation and transition matching. Aliases
// such as `background-image-opacity`, `wash-color-fast`, and `x`/`y`/`z` funnel
// into the same names used by transition capture and transition-end reporting.
[[nodiscard]] std::string panorama_canonical_transition_property_name(std::string_view property);
[[nodiscard]] std::uint32_t panorama_anim_channel_for_property(std::string_view property);

// Applies a `text-transform` to UTF-8 text (ASCII case mapping; non-ASCII bytes are
// left unchanged). Used by both the layout measure and the paint text path so the
// displayed glyphs and their measured width agree.
[[nodiscard]] std::string panorama_transform_text(std::string_view text, PanoramaTextTransform transform);

// Allocation-free variant for the hot measure/paint paths: returns `text` itself
// when no transform applies (the overwhelmingly common case), else writes the
// transformed text into `storage` and returns a view of it. The returned view is
// valid as long as both `text` and `storage` are.
[[nodiscard]] std::string_view panorama_transform_text_view(
    std::string_view text, PanoramaTextTransform transform, std::string& storage);

// A styled slice of label text produced by panorama_parse_inline_markup. `text`
// references a span of the parser's input (no copy), so the source string must
// outlive the returned runs.
struct PanoramaTextRun
{
    std::string_view text;
    bool bold = false;
    bool italic = false;
};

// Parses the minimal subset of inline HTML markup Panorama labels use when
// html="true": <b>/<i> and their closing tags (case-insensitive). The tag
// characters are stripped and the enclosed text is flagged bold/italic; nesting is
// tracked with depth counters. Anything that is not a recognized tag (including a
// bare '<') is emitted verbatim, so non-markup text round-trips unchanged.
[[nodiscard]] std::vector<PanoramaTextRun> panorama_parse_inline_markup(std::string_view text);

// Effective font weight for a run: bold spans render at >= 700 (the loaded bold
// face), other spans keep the element's computed weight. Shared by the layout
// measure, the paint path, and the host glyph rasterizer so all three agree.
[[nodiscard]] int panorama_run_font_weight(int base_weight, bool bold);
PanoramaTextShadow parse_panorama_text_shadow(std::string_view value);
PanoramaBoxShadow parse_panorama_box_shadow(std::string_view value);
PanoramaBlur parse_panorama_blur(std::string_view value);
PanoramaClip parse_panorama_clip(std::string_view value);

// Generates a normalized 1D gaussian kernel (2*radius+1 weights, symmetric, summing
// to ~1) for the given standard deviation. The reusable core of a separable gaussian
// blur pass. `radius` defaults to ceil(3*sigma) when <= 0.
[[nodiscard]] std::vector<float> panorama_gaussian_kernel(float sigma, int radius = 0);

// CPU reference separable-gaussian blur of a tightly-packed RGBA8 image (row-major,
// 4 bytes/pixel, width*height*4 bytes). Horizontal then vertical pass, clamp-to-edge,
// using panorama_gaussian_kernel(std_x/std_y). This is the exact oracle the GPU blur
// pass is validated against by readback, and it lets the engine blur small surfaces
// without a GPU. `std_*` <= 0 on an axis skips that axis. Returns the blurred copy.
[[nodiscard]] std::vector<unsigned char> panorama_blur_rgba(
    const std::vector<unsigned char>& rgba, int width, int height, float std_x, float std_y);

void apply_panorama_declaration(PanoramaComputedStyle& style, std::string_view property, std::string_view value);

// Cascade work counters (diagnostics): cumulative tallies of what compute() did,
// for benchmarks/regression hunts. Reset the struct, run a compute, read it back.
struct PanoramaCascadeStats
{
    std::uint64_t nodes = 0;               // compute_node visits
    std::uint64_t candidate_rules = 0;     // bucket-gathered candidate rules tested
    std::uint64_t selector_tests = 0;      // selector_matches invocations
    std::uint64_t simple_tests = 0;        // simple_matches invocations (incl. ancestors/siblings)
    std::uint64_t matched_rules = 0;       // rules that matched and entered the cascade
    std::uint64_t declarations_applied = 0; // sheet+inline declarations applied
    std::uint64_t filter_rejects = 0;      // selectors skipped by the ancestor bloom filter
    std::uint64_t shared_nodes = 0;        // nodes that reused a previous sibling's computed style
};
// CPUMT-49: panorama_cascade_stats() is thread_local (a plain non-thread_local
// static would race under a forked compute) — every existing increment call
// site works unchanged, since thread_local makes them transparently per-thread.
// A worker calls this right before returning to snapshot-and-reset ITS OWN
// instance; the host sums every worker's snapshot (plus its own instance) into
// the one cumulative figure callers expect.
[[nodiscard]] PanoramaCascadeStats& panorama_cascade_stats();
[[nodiscard]] PanoramaCascadeStats panorama_cascade_stats_take();
}
