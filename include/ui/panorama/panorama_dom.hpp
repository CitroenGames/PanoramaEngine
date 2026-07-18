#pragma once

#include "ui/panorama/panorama_style.hpp"
#include "ui/panorama/panorama_text_break.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Panorama DOM: a lightweight node tree built directly from layout XML by a
// self-contained parser. This is the tree the cascade, layout solver, and QuickJS
// `Panel` binding operate on.
namespace panorama
{
// Opaque, host-assigned texture identifier. 0 means "no texture" (solid fill).
// Pointer-sized so 64-bit handles
// are not truncated.
using PanoramaTextureId = std::uintptr_t;

// Image `scaling=` XML attribute (Valve names). Resolved by the host alongside
// the texture; honoured by paint when drawing paint_texture into the content box.
enum class PanoramaImageScaling : std::uint8_t
{
    Stretch,                // default: fill the content box (may distort)
    None,                   // draw at the natural design size, centred, clipped
    StretchPreserveAspect,  // largest aspect-true rect inside the box, centred
    StretchXPreserveAspect, // fill the box width, height from aspect, centred
    StretchYPreserveAspect, // fill the box height, width from aspect, centred
};

// Absolute, resolved geometry for a node, filled by the layout solver. All values
// are in UI pixels; (x, y) is the top-left of the border box.
struct PanoramaLayoutBox
{
    float x = 0.0F;
    float y = 0.0F;
    float width = 0.0F;  // border-box width
    float height = 0.0F; // border-box height

    float content_x = 0.0F;
    float content_y = 0.0F;
    float content_width = 0.0F;
    float content_height = 0.0F;
};

// Per-property transition runtime state. `cur` is what's displayed, `tgt` the
// cascade target, `from` the value when the active transition started.
struct PanoramaPropAnim
{
    bool animating = false;
    float elapsed = 0.0F;
    float duration = 0.0F;
    float delay = 0.0F;
    PanoramaEasing easing{};
    float reversing_shortening_factor = 1.0F;
};

// Persistent animation state for a node, surviving style recompute so CSS
// transitions interpolate smoothly between cascade targets.
struct PanoramaAnimState
{
    bool initialized = false;

    float opacity_cur = 1.0F;
    float opacity_tgt = 1.0F;
    float opacity_from = 1.0F;
    float opacity_reversing_start = 1.0F;
    PanoramaPropAnim opacity;

    float pos_x_cur = 0.0F;
    float pos_y_cur = 0.0F;
    float pos_x_tgt = 0.0F;
    float pos_y_tgt = 0.0F;
    float pos_x_from = 0.0F;
    float pos_y_from = 0.0F;
    float pos_x_reversing_start = 0.0F;
    float pos_y_reversing_start = 0.0F;
    bool pos_has = false;
    bool pos_x_percent = false;
    bool pos_y_percent = false;
    PanoramaPropAnim pos;

    PanoramaColor color_cur;
    PanoramaColor color_tgt;
    PanoramaColor color_from;
    PanoramaColor color_reversing_start;
    PanoramaPropAnim color;

    PanoramaColor bg_cur;
    PanoramaColor bg_tgt;
    PanoramaColor bg_from;
    PanoramaColor bg_reversing_start;
    PanoramaPropAnim bg;

    float background_image_opacity_cur = 1.0F;
    float background_image_opacity_tgt = 1.0F;
    float background_image_opacity_from = 1.0F;
    float background_image_opacity_reversing_start = 1.0F;
    PanoramaPropAnim background_image_opacity;

    PanoramaColor wash_cur{0xFF, 0xFF, 0xFF, 0xFF};
    PanoramaColor wash_tgt{0xFF, 0xFF, 0xFF, 0xFF};
    PanoramaColor wash_from{0xFF, 0xFF, 0xFF, 0xFF};
    PanoramaColor wash_reversing_start{0xFF, 0xFF, 0xFF, 0xFF};
    PanoramaPropAnim wash;

    float brightness_cur = 1.0F;
    float brightness_tgt = 1.0F;
    float brightness_from = 1.0F;
    float brightness_reversing_start = 1.0F;
    PanoramaPropAnim brightness;

    PanoramaTransform transform_cur;
    PanoramaTransform transform_tgt;
    PanoramaTransform transform_from;
    PanoramaTransform transform_reversing_start;
    PanoramaPropAnim transform;

    // width/height transitions (interpolated only between same-type definite lengths,
    // e.g. px<->px or %<->%; other combinations snap). These drive a relayout.
    PanoramaLength width_cur;
    PanoramaLength width_tgt;
    PanoramaLength width_from;
    PanoramaLength width_reversing_start;
    PanoramaPropAnim width;
    PanoramaLength height_cur;
    PanoramaLength height_tgt;
    PanoramaLength height_from;
    PanoramaLength height_reversing_start;
    PanoramaPropAnim height;

    float border_width_cur = 0.0F;
    float border_width_tgt = 0.0F;
    float border_width_from = 0.0F;
    float border_width_reversing_start = 0.0F;
    PanoramaPropAnim border_width;
    PanoramaColor border_color_cur;
    PanoramaColor border_color_tgt;
    PanoramaColor border_color_from;
    PanoramaColor border_color_reversing_start;
    PanoramaPropAnim border_color;

    PanoramaBoxShadow box_shadow_cur;
    PanoramaBoxShadow box_shadow_tgt;
    PanoramaBoxShadow box_shadow_from;
    PanoramaBoxShadow box_shadow_reversing_start;
    PanoramaPropAnim box_shadow;

    PanoramaBlur blur_cur;
    PanoramaBlur blur_tgt;
    PanoramaBlur blur_from;
    PanoramaBlur blur_reversing_start;
    PanoramaPropAnim blur;

    PanoramaClip clip_cur;
    PanoramaClip clip_tgt;
    PanoramaClip clip_from;
    PanoramaClip clip_reversing_start;
    PanoramaPropAnim clip;

    float pre_scale_x_cur = 1.0F;
    float pre_scale_y_cur = 1.0F;
    float pre_scale_x_tgt = 1.0F;
    float pre_scale_y_tgt = 1.0F;
    float pre_scale_x_from = 1.0F;
    float pre_scale_y_from = 1.0F;
    float pre_scale_x_reversing_start = 1.0F;
    float pre_scale_y_reversing_start = 1.0F;
    PanoramaPropAnim pre_scale;

    // True when any channel's transition is running. A stateless OR over the
    // `animating` flags (a maintained counter could drift out of sync); the
    // per-frame advance early-outs idle nodes with it.
    [[nodiscard]] bool any_channel_animating() const noexcept
    {
        return opacity.animating || pos.animating || color.animating || bg.animating ||
            background_image_opacity.animating || wash.animating || brightness.animating || transform.animating ||
            width.animating || height.animating || border_width.animating || border_color.animating ||
            box_shadow.animating || blur.animating || clip.animating || pre_scale.animating;
    }
};

// Cache of a node's parsed `inline_style` declarations, owned by the node and
// rebuilt lazily by the cascade. Valid only for the exact source text it was
// parsed from and the stylesheet state it was resolved against (@defines change
// declarations' resolved values), so the cascade revalidates by comparing
// `source` and the (sheet, sheet_generation) pair before reuse.
struct PanoramaInlineStyleCache
{
    const void* sheet = nullptr;
    std::uint64_t sheet_generation = 0;
    std::string source;
    std::vector<PanoramaDeclaration> declarations;
    bool valid = false;
};

struct PanoramaNode;

// Node lifetime observation -----------------------------------------------------
//
// Long-lived systems keep raw PanoramaNode* across frames (the script runtime's
// JS wrappers and per-panel maps, the input controller's hover/focus state, a
// host's caches). Panels are deleted at any time by script (DeleteAsync,
// RemoveAndDeleteChildren) or by layout reloads, so every such pointer is a
// use-after-free waiting to happen unless its holder is told. ~PanoramaNode
// notifies every registered observer — for each node individually, parents
// before their children — so holders drop matching pointers before they dangle.
//
// Observers must treat the node as identity only (compare/erase the pointer);
// the object is mid-destruction. The registry is process-global and unlocked:
// DOM MUTATION (construction/destruction/reparenting, this registry, JS/input/
// host writes) is single-threaded by design (see README) and MUST stay that
// way -- this class's own unlocked registry depends on it. CPUMT-49 forks
// PanoramaStyleSheet::compute() (a READ-mostly pass that only ever writes each
// node's OWN `computed`/style_dirty/style_fresh/descendant_style_dirty fields,
// never touching this registry, never adding/removing/reparenting nodes)
// across worker threads for the duration of one cascade; that fork's own
// thread-safety invariants live on PanoramaStyleSheet::compute_forked_subtree's
// doc comment, not here. Observers are expected to be few and long-lived (a
// runtime, an input controller, a host view).
class PanoramaNodeLifetimeObserver
{
public:
    virtual ~PanoramaNodeLifetimeObserver() = default;
    virtual void on_panorama_node_destroyed(PanoramaNode& node) = 0;
};

void panorama_add_node_lifetime_observer(PanoramaNodeLifetimeObserver& observer);
void panorama_remove_node_lifetime_observer(PanoramaNodeLifetimeObserver& observer);

// Tree guard (diagnostic, PanoramaDiagnostics::tree_guard) --------------------
//
// Hunts use-after-free tree corruption: every node carries a liveness canary
// that ~PanoramaNode flips, and scan_dead_links walks a document's children
// vectors looking for a child whose canary is no longer alive (or whose parent
// backlink no longer points at the node that owns it) — i.e. a node that was
// destroyed (or replaced) while still linked. The mutation context is a
// breadcrumb the runtime/input layers update before running script or
// structural ops, so a detection can name the mutation that caused it.
bool panorama_debug_tree_guard_enabled();
void panorama_debug_set_mutation_context(std::string context);
[[nodiscard]] const PanoramaNode* panorama_debug_scan_dead_links(const PanoramaNode& root, std::string& report);

// Scoped watch over a worklist of node pointers held across script execution
// (e.g. the onmouseout/onmouseover chains): entries whose node is destroyed
// mid-iteration — a handler may delete arbitrary panels — are nulled in place,
// so the iteration skips them instead of dangling.
class PanoramaScopedNodeWatch final : public PanoramaNodeLifetimeObserver
{
public:
    explicit PanoramaScopedNodeWatch(std::vector<PanoramaNode*> nodes);
    ~PanoramaScopedNodeWatch() override;
    PanoramaScopedNodeWatch(const PanoramaScopedNodeWatch&) = delete;
    PanoramaScopedNodeWatch& operator=(const PanoramaScopedNodeWatch&) = delete;

    [[nodiscard]] const std::vector<PanoramaNode*>& nodes() const noexcept { return nodes_; }

    void on_panorama_node_destroyed(PanoramaNode& node) override;

private:
    std::vector<PanoramaNode*> nodes_;
};

struct PanoramaNode
{
    // Tree-guard liveness canary (see panorama_debug_scan_dead_links): alive on
    // construction, flipped by ~PanoramaNode after observers ran. Freed-then-
    // zeroed or freed-then-reused memory fails the check either way.
    static constexpr std::uint32_t kLivenessAlive = 0x50414E4Fu; // 'PANO'
    static constexpr std::uint32_t kLivenessDead = 0xDEADDEADu;

    PanoramaNode() = default;
    ~PanoramaNode(); // notifies lifetime observers (see above)
    PanoramaNode(const PanoramaNode&) = delete;
    PanoramaNode& operator=(const PanoramaNode&) = delete;
    // Moves keep working (e.g. `root = PanoramaNode{};` to reset a tree). The
    // moved-from shell still notifies observers when it dies; observers track
    // addresses, and the moved-to object is a new address to them.
    PanoramaNode(PanoramaNode&&) = default;
    PanoramaNode& operator=(PanoramaNode&&) = default;

    std::string tag;                 // original tag name, e.g. "Panel", "Label", "Button"
    std::string tag_lower;           // lowercased tag for matching
    std::string id;                  // id attribute (without '#')
    std::vector<std::string> classes;
    std::unordered_map<std::string, std::string> attributes; // lowercased keys, excludes id/class/style/text
    std::string inline_style;        // raw style="" attribute text
    std::string text;                // text attribute or label body

    PanoramaNode* parent = nullptr;
    std::vector<std::unique_ptr<PanoramaNode>> children;

    std::uint32_t debug_liveness = kLivenessAlive; // tree-guard canary (see above)

    PanoramaComputedStyle computed; // filled by PanoramaStyleSheet::compute
    PanoramaLayoutBox layout;       // filled by the layout solver
    PanoramaLayoutBox popup_layout; // optional top-level popup geometry (dropdown menus)
    bool has_popup_layout = false;
    // Layout-derived aggregate including this node. The input controller uses it
    // to prune the popup-only hit-test without searching closed-menu subtrees.
    bool subtree_has_popup_layout = false;

    // Host-populated texture for image-like nodes (e.g. a rasterized SVG icon).
    // When non-zero, the paint layer emits a textured quad over the content box.
    PanoramaTextureId paint_texture = 0;
    PanoramaTextureId background_texture = 0;
    // Natural aspect (width / height) of background_texture, set by the host when the
    // image resolves; used for background-size contain/cover. 0 = unknown (stretch).
    float background_texture_aspect = 0.0F;
    // Natural pixel size (design units) of background_texture, set by the host when the
    // image resolves; used for background-size: auto (CSS default = intrinsic size).
    // 0 = unknown (paint falls back to stretching the box).
    float background_texture_natural_width = 0.0F;
    float background_texture_natural_height = 0.0F;
    // paint_texture's `scaling=` mode + natural size in design units (host-set;
    // natural size 0 = unknown, paint falls back to stretch).
    PanoramaImageScaling paint_texture_scaling = PanoramaImageScaling::Stretch;
    float paint_texture_natural_width = 0.0F;
    float paint_texture_natural_height = 0.0F;

    // Layout-scope mark (PanoramaDocumentSession): the id of the layout file
    // whose load created this node (or whose root was merged onto it via
    // BLoadLayout). Scoped stylesheets only style nodes whose ancestor-or-self
    // chain carries a matching mark; 0 = unmarked.
    std::uint16_t style_scope_mark = 0;

    // text-overflow: shrink — the reduced font size that makes the text fit the box.
    // Computed + stored by the font atlas/text provider (which also pre-rasterizes
    // glyphs at this size) and read by paint, so both use one identical value
    // (no measure drift). 0 = none.
    float shrink_font_size = 0.0F;

    // JS-set visibility override (Panel.visible). Applied on top of the cascade so
    // it survives style recompute; -1 = unset, 0 = forced hidden, 1 = forced shown.
    int visibility_override = -1;

    // Interaction pseudo-class state, driven by input hit-testing; read by the
    // cascade to apply `:hover` / `:active` / `:focus` / `:selected` rules.
    bool hovered = false;
    bool active = false;
    bool focused = false;
    bool selected = false;

    // Text-entry editing state (WebCore HTMLTextFormControlElement selection
    // model). Meaningful only on a focused <TextEntry>; both are byte offsets into
    // `text` (always on a UTF-8 codepoint boundary). The caret is `text_caret`;
    // the selection is the closed range [min, max] of caret and anchor, empty
    // (a bare caret) when they coincide. selectionStart = min, selectionEnd = max;
    // selectionDirection is forward when caret >= anchor. Clamped to text length
    // whenever the value changes (panorama_text_entry_set_value).
    int text_caret = 0;
    int text_selection_anchor = 0;

    // When true, paint draws the translucent selection wash over the byte range
    // [min,max] of text_caret/anchor on ANY node that has text — not just a
    // focused <TextEntry>. The host console sets it on its output labels to
    // render a drag-selection that spans multiple labels. Honours wrapped
    // (multi-line) labels. No caret is drawn (that stays TextEntry-only).
    bool selection_active = false;

    // Style invalidation (WebCore Style::Validity + descendant dirty bits):
    // `style_dirty` marks this node's whole subtree for recompute (a state/class/
    // attribute change here can alter any descendant's matches via descendant
    // combinators); `descendant_style_dirty` routes the partial recompute walk.
    // PanoramaStyleSheet::compute_invalidated honors these; compute() ignores
    // them (always full). New nodes start dirty.
    bool style_dirty = true;
    bool descendant_style_dirty = false;
    // Set by the cascade whenever this node's computed style is rebuilt; consumed
    // by panorama_capture_anim_targets_recomputed so transition re-targeting only
    // sees freshly cascaded values, never the interpolated values the animation
    // advance writes back into `computed` each frame.
    bool style_fresh = true;

    // Slice 3 (PanoramaDrawConstants campaign): dedup stamp for the per-advance
    // recomposite-dirty node list (see PanoramaRecompositeDirtyTracker /
    // PanoramaAnimationAdvanceResult::recomposite_changed in panorama_anim.hpp).
    // Set to the tracker's current `generation` the first time this node is
    // recorded during that generation, so a node touched by multiple channels
    // (or by both the transitions and @keyframes advance passes in the same
    // frame, when the caller reuses one tracker across both calls) is recorded
    // once. Sibling to `style_fresh` above but independent of it.
    std::uint32_t recomposite_dirty_stamp = 0;

    // Marks this subtree for recompute and flags the ancestor chain so
    // compute_invalidated can find it. Call after mutating anything the cascade
    // reads (classes, attributes, inline style, pseudo state).
    void mark_style_dirty();

    // CSS-transition runtime state (see panorama_anim.hpp).
    PanoramaAnimState anim;

    // @keyframes animation runtime (CSS animations), independent of the transition
    // state above. Tracks elapsed time for the currently-applied `animation-name`;
    // restarts when the name changes (e.g. a class toggle assigns a new animation).
    struct PanoramaKeyframeRuntime
    {
        std::string active_name;
        float elapsed = 0.0F;
        bool finished = false;
        // True once the timeline has written computed values that a later
        // non-applying frame must revert (see advance_keyframe_node).
        bool applied = false;
        // Registry resolution for active_name, valid only for the exact sheet
        // (instance id, generation) pair it was resolved against — the registry
        // is only mutated by add_source/clear, which bump the generation, so a
        // validated pointer cannot dangle. Instance ids are never 0; nullptr
        // with a non-zero instance records "no usable keyframes" for the name.
        const PanoramaKeyframes* keyframes = nullptr;
        std::uint64_t sheet_instance = 0;
        std::uint64_t sheet_generation = 0;
    };
    PanoramaKeyframeRuntime keyframe_anim;

    // Internal scratch used by the layout solver (min-content intrinsic size).
    float intrinsic_width = 0.0F;
    float intrinsic_height = 0.0F;
    // Single-line text metrics from the intrinsic pass (the same measure paint
    // advances reproduce); the resolve pass reads them to decide whether the
    // text overflows its resolved content width and must wrap.
    float intrinsic_text_width = 0.0F;
    float intrinsic_text_height = 0.0F;

    // Multi-line text (WebCore line breaking, see panorama_text_break.hpp):
    // filled by the layout solver when the label's text wraps in its resolved
    // content width (or contains forced '\n' breaks); empty = single line.
    // Segments are byte ranges of the case-transformed display text per styled
    // run (run 0 = the whole text for plain labels; one per markup run for
    // html="true" labels) — paint re-derives the identical runs and slices
    // them. `text_line_advance` is the per-line baseline step in px.
    std::vector<PanoramaTextWrapLine> text_lines;
    float text_line_advance = 0.0F;

    // overflow:scroll state (WebCore RenderLayerScrollableArea). `scroll_offset_*`
    // is the persistent scroll position; the layout solver clamps it to
    // [0, max_scroll_*] every pass (content/viewport changes shrink the range) and
    // shifts the children by it, mirroring WebCore folding scrollPosition() into
    // layer positions. `max_scroll_*` is layout-computed (maximumScrollOffset =
    // scroll extent - viewport); 0 when the node does not scroll on that axis.
    float scroll_offset_x = 0.0F;
    float scroll_offset_y = 0.0F;
    float max_scroll_x = 0.0F;
    float max_scroll_y = 0.0F;

    // Smooth-scroll runtime (WebCore ScrollAnimationSmooth): a critically-damped
    // spring per axis drives scroll_offset_* toward dest_* without overshoot.
    // Started/retargeted by panorama_smooth_scroll_to; advanced by
    // panorama_advance_scroll_animations. Velocities are px/ms (WebCore units).
    struct PanoramaScrollAnimation
    {
        bool active = false;
        float dest_x = 0.0F;
        float dest_y = 0.0F;
        float velocity_x = 0.0F;
        float velocity_y = 0.0F;
    };
    PanoramaScrollAnimation scroll_anim;

    // Parsed-inline-style cache, maintained by PanoramaStyleSheet::compute. Not
    // copied by clone (clones revalidate lazily on their first cascade).
    PanoramaInlineStyleCache inline_style_cache;

    [[nodiscard]] bool has_class(std::string_view klass) const;
    [[nodiscard]] PanoramaNode* find_by_id(std::string_view target_id);

    // True when the label carries html="true"; its `text` then contains a subset of
    // inline HTML markup (<b>/<i>) that the measure/paint paths interpret as styled
    // runs rather than literal characters.
    [[nodiscard]] bool is_html_text() const;
};

// The result of parsing a Panorama layout document. `root` is a synthetic node
// holding the document's top-level panels as children. Resource resolution (frame
// src, stylesheet/script includes) is intentionally left to the host; this layer
// only records the references.
struct PanoramaDocument
{
    std::unique_ptr<PanoramaNode> root;
    std::vector<std::string> stylesheet_includes; // <styles><include src="..."/>
    std::vector<std::string> script_includes;     // <scripts><include src="..."/>
    std::vector<std::string> inline_styles;        // CSS text from inline <style> blocks
    std::unordered_map<std::string, std::unique_ptr<PanoramaNode>> snippets; // name -> subtree

    [[nodiscard]] PanoramaNode* find_by_id(std::string_view target_id) const;
};

// Parses Panorama layout XML into a PanoramaDocument. Tolerant of comments, CDATA,
// processing instructions, entity references, and self-closing tags. Never throws;
// malformed input yields a best-effort tree.
[[nodiscard]] PanoramaDocument parse_panorama_xml(std::string_view xml);

using PanoramaLocalizeCallback = std::function<std::string(std::string_view)>;

// Panorama controls expose a small amount of host-created structure to CSS. In
// Chromium/CEF-backed Panorama, TextEntry owns a #PlaceholderText label and toggles
// HasInput as its value changes; Valve's shipped styles target those nodes/classes.
void ensure_panorama_text_entry_placeholders(PanoramaNode& root, const PanoramaLocalizeCallback& localize);
void sync_panorama_text_entry_input_class(PanoramaNode& node);

[[nodiscard]] bool panorama_node_is_leaf_content(const PanoramaNode& node);
[[nodiscard]] bool panorama_node_is_content_sized_control(const PanoramaNode& node);
[[nodiscard]] bool panorama_node_defaults_to_content_size(const PanoramaNode& node);
[[nodiscard]] bool panorama_node_is_focusable_control(const PanoramaNode& node);
[[nodiscard]] bool panorama_node_is_radio_button(const PanoramaNode& node);
[[nodiscard]] bool panorama_node_is_toggle_button(const PanoramaNode& node);
[[nodiscard]] bool panorama_node_collapses_to_selected_child(const PanoramaNode& node);
// Sets a control's selected state, keeping the `checked` class in sync (the JS
// `panel.checked` surface accepts either form; they must never diverge).
void panorama_set_node_selected(PanoramaNode& node, bool selected);

// Materializes Valve's internal ToggleButton/RadioButton children (`.TickBox` /
// `.RadioBox` + the control-text Label). Idempotent; run per frame from
// panorama_apply_control_presentation.
void ensure_panorama_selection_control_internals(PanoramaNode& node);

// Slider control (Valve `Slider`/`SlottedSlider`) -------------------------------
//
// Panorama's `<Slider>` is a native C++ control we render ourselves. It owns a
// `#SliderTrack` (the rail) holding a `#SliderTrackProgress` (the filled portion
// up to the thumb) and a `#SliderThumb` (the draggable knob); CS:GO styles those
// internals by id. The control's `value`/`min`/`max` attributes drive the thumb
// position, mirroring WebKit's RenderSliderContainer::layout
// (thumbOffset = proportionFromValue(value) * (trackContentWidth - thumbWidth)).
[[nodiscard]] bool panorama_node_is_slider(const PanoramaNode& node);
// True for the settings-row wrapper control (Valve `CSGOSettingsSlider`): a
// labelled Slider + numeric readout bound to a convar. Recognized so the host can
// find rows and the presentation pass can build their internals.
[[nodiscard]] bool panorama_node_is_settings_slider(const PanoramaNode& node);
// Materializes a `Slider`'s internal chrome (`#SliderTrack` > `#SliderTrackProgress`
// + `#SliderThumb`), idempotently. Run per frame from
// panorama_apply_control_presentation.
void ensure_panorama_slider_internals(PanoramaNode& node);
// Materializes a `CSGOSettingsSlider`'s row internals (a `Label#Title`, a
// `Slider#Slider.HorizontalSlider` carrying the row's min/max, and a
// `TextEntry#Value` readout), idempotently, matching settings_slider.xml. The
// host then binds the inner slider's `value` + the readout text to a convar.
void ensure_panorama_settings_slider_internals(PanoramaNode& node);
// True for the settings-row key-rebind control (Valve `CSGOSettingsKeyBinder`): a
// labelled key-binding button + a clear button, bound to an engine `bind` command.
// Recognized so the host can find rows and the presentation pass builds internals.
[[nodiscard]] bool panorama_node_is_settings_keybinder(const PanoramaNode& node);
// Materializes a `CSGOSettingsKeyBinder`'s internal chrome (a `#<id>__title` label,
// `#LabelFXContainer` > `#BindingLabelContainer` > the `#<id>__bind` value label,
// and the `#<id>__clear` clear button), idempotently, matching settings_keybinder.xml.
// The clickable leaves carry row-unique ids so the host can resolve a polled click
// back to the row (settings_keybinder.css is not included by the tab pages, so its few
// layout rules are baked as inline styles, mirroring ensure_panorama_settings_slider).
void ensure_panorama_settings_keybinder_internals(PanoramaNode& node);
// The slider's current position as a 0..1 fraction, derived from its `value`,
// `min`, and `max` attributes (clamped; degenerate range -> 0). Pure.
[[nodiscard]] float panorama_slider_fraction(const PanoramaNode& node);
// Sets the slider's `value` attribute from a 0..1 fraction (value = min +
// fraction*(max-min)), marking it dirty. Returns true when the stored `value`
// string actually changed (so a drag relayouts / the host writes the convar).
bool panorama_set_slider_fraction(PanoramaNode& node, float fraction);

// overflow:scroll support ------------------------------------------------------
//
// Materializes Valve's scrollbar chrome for nodes whose computed style scrolls
// on an axis: a `VerticalScrollBar`/`HorizontalScrollBar` child holding a
// `.ScrollThumb` panel (CS:GO styles those by tag/class — 8px white rounded
// thumb, right-aligned track). The chrome is tagged with the `__scrollbar`
// attribute so layout excludes it from the flow and positions it as an overlay
// spanning the viewport. Idempotent; run per frame from
// panorama_apply_control_presentation (after the cascade, like the selection
// internals above).
void ensure_panorama_scrollbar_internals(PanoramaNode& node);
// True for synthesized scrollbar chrome (the `__scrollbar` mark): excluded from
// flow layout, intrinsic measurement, fit-children extents and scrolling itself.
[[nodiscard]] bool panorama_node_is_scrollbar(const PanoramaNode& node);
// Whether the node's style asks for scrolling on the axis (overflow: scroll).
// Scrollability additionally requires layout overflow (max_scroll_* > 0).
[[nodiscard]] bool panorama_node_scrolls_x(const PanoramaNode& node);
[[nodiscard]] bool panorama_node_scrolls_y(const PanoramaNode& node);
// Clamped scroll-offset setter (WebCore RenderLayerScrollableArea::scrollToOffset
// with ScrollClamping::Clamped): constrains to [0, max_scroll_*] as computed by
// the last layout. Returns true when the stored offset changed (caller must
// trigger a relayout to apply it).
bool panorama_set_scroll_offset(PanoramaNode& node, float offset_x, float offset_y);
// Scrolls the nearest scrollable ancestor so `target`'s border box is visible —
// Valve's ScrollParentToMakePanelFit, implemented as WebCore scrollRectToVisible
// with ScrollAlignment::alignToEdgeIfNeeded on both axes (visible: no scroll;
// partial/hidden: align to the closest edge). Walks further up if an ancestor
// scroller cannot move (WebCore wheel/expose propagation). With `smooth` the
// scrollers glide to the computed offsets via the smooth-scroll spring below
// (Valve's bImmediate=false) instead of jumping. Returns true when any scroll
// offset (or smooth-scroll destination) changed.
bool panorama_scroll_ancestors_to_fit(PanoramaNode& target, bool smooth = false);

// Smooth scrolling (WebCore ScrollAnimationSmooth): starts or retargets the
// node's scroll animation toward (dest_x, dest_y), clamped to the scroll range.
// Retargeting preserves the in-flight velocity so the motion stays
// C1-continuous; a fresh start begins at rest. The spring is advanced by
// panorama_advance_scroll_animations (panorama_anim.hpp) each frame, which
// applies the moving offset through panorama_set_scroll_offset. Returns true
// when the destination actually moved.
bool panorama_smooth_scroll_to(PanoramaNode& node, float dest_x, float dest_y);
// Stops the node's scroll animation in place (direct manipulation — scrollbar
// thumb drags, ScrollToTop/Bottom — must not fight the spring).
void panorama_cancel_scroll_animation(PanoramaNode& node);
// The offset the node is heading to: the animation destination while one is
// active, else the current offset. Successive wheel ticks accumulate on this
// (WebCore retargets the active animation rather than the displayed offset).
[[nodiscard]] float panorama_scroll_destination_x(const PanoramaNode& node);
[[nodiscard]] float panorama_scroll_destination_y(const PanoramaNode& node);
// False for controls whose text renders via their internal label (toggle/radio
// buttons) — paint and intrinsic measurement must skip node.text for those.
[[nodiscard]] bool panorama_node_paints_own_text(const PanoramaNode& node);

// DropDown control semantics. Panorama's native DropDown is a C++ control we don't
// have (we render Panorama ourselves), so the host emulates open/close + selection
// through these. "Open" is tracked with CS:GO's own `DropDownMenuVisible` class so
// CSS can react and the state survives style recompute.
[[nodiscard]] bool panorama_dropdown_is_open(const PanoramaNode& dropdown);
void panorama_set_dropdown_open(PanoramaNode& dropdown, bool open);
// Marks `option` selected within `dropdown` (sets its `selected` flag + `checked`
// class + the dropdown's `selected` id attribute) and deselects its siblings, so a
// later GetSelected()/collapse agrees on the choice.
void panorama_select_dropdown_option(PanoramaNode& dropdown, PanoramaNode& option);
// The option a closed dropdown collapses to: its current selection (by `selected`
// id attribute, then `selected`/`checked`/`selected`-class child), else the first
// visible child. Null if there are no visible children. Pure (no mutation).
[[nodiscard]] PanoramaNode* panorama_dropdown_selected_child(const PanoramaNode& dropdown);
}
