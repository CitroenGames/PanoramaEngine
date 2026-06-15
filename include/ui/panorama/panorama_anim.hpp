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
namespace openstrike
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
    bool active = false;
    // Transitions that completed this advance, in tree order. The host forwards
    // these as Panorama 'PropertyTransitionEnd' (panelName, propertyName) events
    // AFTER the advance (WebCore enqueues, then dispatches — handlers observe
    // final values). Only filled by panorama_advance_anim (transitions);
    // @keyframes/scroll advances leave it empty.
    std::vector<PanoramaTransitionEnd> transition_ends;
};

// Snapshots each node's current computed values as transition targets, starting a
// transition (from the currently displayed value) for any animatable property
// whose target changed and that has a matching `transition-*` rule. On the first
// call it initialises the displayed values to the current computed values.
void panorama_capture_anim_targets(PanoramaNode& root);

// Like panorama_capture_anim_targets, but only for nodes the cascade actually
// recomputed since the last capture (PanoramaNode::style_fresh). Required after
// PanoramaStyleSheet::compute_invalidated: untouched nodes hold the per-frame
// INTERPOLATED value in `computed` (the advance pass writes it back), which a
// full re-capture would mistake for a new cascade target and cancel the
// in-flight transition at its current value.
void panorama_capture_anim_targets_recomputed(PanoramaNode& root);

// Host code can populate intrinsic dimensions after transition capture (for
// example, resolved Image natural size). Keep the displayed transition state in
// lockstep so later animation frames do not restore stale cascade values.
void panorama_sync_anim_dimensions(PanoramaNode& node);

// Advances all running transitions by `dt` seconds and writes the interpolated
// values back into each node's computed style (opacity, position, colors) so the
// layout and paint passes use the animated state. The result identifies whether
// callers need to repaint and whether layout-affecting position data changed.
PanoramaAnimationAdvanceResult panorama_advance_anim(PanoramaNode& root, float dt);

// Advances CSS @keyframes animations by `dt` seconds for every node whose computed
// `animation-name` names an entry in `keyframes`, writing the interpolated channels
// (opacity / brightness / colors / wash / transform / position) into each node's
// computed style. Honors animation-duration/-delay/-iteration-count/-direction/
// -fill-mode and per-segment timing functions. Must run AFTER the cascade and
// panorama_advance_anim so animations win over the static value for their channels.
PanoramaAnimationAdvanceResult panorama_advance_keyframes(
    PanoramaNode& root, const std::unordered_map<std::string, PanoramaKeyframes>& keyframes, float dt);

// Advances every active smooth-scroll spring (PanoramaNode::scroll_anim, started
// by panorama_smooth_scroll_to / wheel input / smooth ScrollParentToMakePanelFit)
// by `dt` seconds. WebCore ScrollAnimationSmooth: a critically-damped spring per
// axis, integrated with semi-implicit Euler at a fixed 4ms physics step, snapping
// to the destination on overshoot or once position AND velocity are within
// tolerance. Offsets apply through panorama_set_scroll_offset, so a changed
// result (`layout_changed`) requires a relayout to move the content.
PanoramaAnimationAdvanceResult panorama_advance_scroll_animations(PanoramaNode& root, float dt);
}
