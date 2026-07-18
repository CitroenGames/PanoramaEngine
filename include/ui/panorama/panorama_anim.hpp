#pragma once

#include "ui/panorama/panorama_dom.hpp"

// CSS-transition engine for the Panorama tree. The cascade produces a target
// style each recompute; this layer interpolates the displayed value toward that
// target over `transition-duration` using the `transition-timing-function`, for
// opacity, position, color, and background-color.
//
// Usage per frame:
//   if (styles recomputed) panorama_capture_anim_targets(root);
//   panorama_advance_anim(root, dt);   // writes interpolated values into computed
// Call panorama_capture_anim_targets once on the base (pre-script) style so the
// initial displayed value is the "from" of the first transition.
namespace panorama
{
// A CSS transition that finished during an advance: the displayed value reached
// the cascade target (WebCore DeclarativeAnimation::invalidateDOMEvents fires
// `transitionend` only on the Active->After phase crossing). A transition that
// is replaced or snapped by a re-capture before reaching its end never reports
// here — that is WebCore's `transitioncancel`, which Panorama scripts don't use.
// `property` is the engine's canonical transition-property key ("opacity",
// "position", "transform", ...), the same vocabulary `transition-property`
// matching uses, and what CS:GO's PropertyTransitionEnd handlers compare against.
struct PanoramaTransitionEnd
{
    PanoramaNode* node = nullptr;
    const char* property = nullptr;
};

struct PanoramaAnimationAdvanceResult
{
    bool visual_changed = false;
    bool layout_changed = false;
    // Slice 3 (PanoramaDrawConstants campaign): set instead of (never together
    // with) visual_changed for a channel whose ONLY effect is the per-command
    // PanoramaDrawConstants a layer context carries -- CSS `transform` (and
    // pre-transform-scale2d, which feeds the same context matrix), `opacity`,
    // and a blur value change where the blur command's EXISTENCE in the draw
    // list does not change (both the pre- and post-advance value are visible:
    // see panorama_anim.cpp's blur_visible helper -- a 0-crossing adds/removes
    // the backdrop-blur command, a visual_changed-class structural change, not
    // a constants patch). A frame whose ONLY dirty class is this one never
    // needs to re-run paint/hash/compile: the caller can recompute affected
    // layer contexts and patch the cached geometry's constants in place
    // instead (see PanoramaNativeView's recomposite-only fast path).
    bool recomposite_changed = false;
    bool active = false;
    // Transitions that completed this advance, in tree order. The host forwards
    // these as Panorama 'PropertyTransitionEnd' (panelName, propertyName) events
    // AFTER the advance (WebCore enqueues, then dispatches — handlers observe
    // final values). Only filled by panorama_advance_anim (transitions);
    // @keyframes/scroll advances leave it empty.
    std::vector<PanoramaTransitionEnd> transition_ends;
};

// Collects the nodes whose recomposite-class value (see
// PanoramaAnimationAdvanceResult::recomposite_changed above) changed during an
// advance, so a caller retaining layer-context/geometry-cache state across
// frames (PanoramaNativeView's fast path) knows exactly which contexts need
// recomputing without re-walking the whole tree. `nodes` is APPENDED to, never
// cleared, by panorama_advance_anim/panorama_advance_keyframes -- the caller
// owns it and clears it once per frame (see PanoramaNativeView::render), so
// its capacity survives across frames with zero steady-state allocation, and
// passing the SAME tracker to both calls in one frame accumulates one
// combined list. `generation` gates the per-node dedup stamp
// (PanoramaNode::recomposite_dirty_stamp): the caller bumps its own counter
// once per frame BEFORE either advance call and stores it here, starting from
// 1 -- generation 0 would collide with a freshly-constructed node's default
// stamp and wrongly look "already recorded".
struct PanoramaRecompositeDirtyTracker
{
    std::vector<PanoramaNode*>* nodes = nullptr;
    std::uint32_t generation = 0;
};

// Snapshots each node's current computed values as transition targets, starting a
// transition (from the currently displayed value) for any animatable property
// whose target changed and that has a matching `transition-*` rule. On the first
// call it initialises the displayed values to the current computed values.
void panorama_capture_anim_targets(PanoramaNode& root);

// What the selective capture below saw, driving the host's animation-advance
// gate: a style frame that starts no transition (every fresh node snapped) and
// touches no animation-named node needs no advance — anything already running
// keeps the gate set through the advance result, and a keyframe animation can
// only start through a style change (its node is style_fresh that frame).
struct PanoramaAnimCaptureResult
{
    bool any_transition_animating = false; // a visited node has a transition in flight
    bool any_keyframe_candidate = false;   // a visited node carries (or must revert) an animation-name
};

// Like panorama_capture_anim_targets, but only for nodes the cascade actually
// recomputed since the last capture (PanoramaNode::style_fresh). Required after
// PanoramaStyleSheet::compute_invalidated: untouched nodes hold the per-frame
// INTERPOLATED value in `computed` (the advance pass writes it back), which a
// full re-capture would mistake for a new cascade target and cancel the
// in-flight transition at its current value.
PanoramaAnimCaptureResult panorama_capture_anim_targets_recomputed(PanoramaNode& root);

// Host code can populate intrinsic dimensions after transition capture (for
// example, resolved Image natural size). Keep the displayed transition state in
// lockstep so later animation frames do not restore stale cascade values.
void panorama_sync_anim_dimensions(PanoramaNode& node);

// Advances all running transitions by `dt` seconds and writes the interpolated
// values back into each node's computed style (opacity, position, colors) so the
// layout and paint passes use the animated state. The result identifies whether
// callers need to repaint and whether layout-affecting position data changed.
// `dirty_tracker`, when non-null, records every node whose recomposite-class
// value changed this advance (see PanoramaRecompositeDirtyTracker); omit it
// (the default) when the caller has no use for per-node granularity.
PanoramaAnimationAdvanceResult panorama_advance_anim(
    PanoramaNode& root, float dt, PanoramaRecompositeDirtyTracker* dirty_tracker = nullptr);

// Advances CSS @keyframes animations by `dt` seconds for every node whose computed
// `animation-name` names an entry in `keyframes`, writing the interpolated channels
// (opacity / brightness / colors / wash / transform / position) into each node's
// computed style. Honors animation-duration/-delay/-iteration-count/-direction/
// -fill-mode and per-segment timing functions. Must run AFTER the cascade and
// panorama_advance_anim so animations win over the static value for their channels.
// `dirty_tracker`: see panorama_advance_anim above.
PanoramaAnimationAdvanceResult panorama_advance_keyframes(PanoramaNode& root,
    const std::unordered_map<std::string, PanoramaKeyframes>& keyframes, float dt,
    PanoramaRecompositeDirtyTracker* dirty_tracker = nullptr);

// Sheet-aware overload: identical behavior, but each node's keyframe runtime
// caches its registry resolution keyed on the sheet's never-reused instance id
// + content generation (bumped by add_source/clear — the only registry
// mutators), so steady-state frames skip the per-node name lookup.
PanoramaAnimationAdvanceResult panorama_advance_keyframes(PanoramaNode& root, const PanoramaStyleSheet& sheet, float dt,
    PanoramaRecompositeDirtyTracker* dirty_tracker = nullptr);

// Advances every active smooth-scroll spring (PanoramaNode::scroll_anim, started
// by panorama_smooth_scroll_to / wheel input / smooth ScrollParentToMakePanelFit)
// by `dt` seconds. WebCore ScrollAnimationSmooth: a critically-damped spring per
// axis, integrated with semi-implicit Euler at a fixed 4ms physics step, snapping
// to the destination on overshoot or once position AND velocity are within
// tolerance. Offsets apply through panorama_set_scroll_offset, so a changed
// result (`layout_changed`) requires a relayout to move the content.
PanoramaAnimationAdvanceResult panorama_advance_scroll_animations(PanoramaNode& root, float dt);
}
