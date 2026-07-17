# Panorama Support Surface

What PanoramaEngine implements of Valve's Panorama UI system, and where it
deliberately stops. This is a companion to the README's
[What It Provides](../README.md#what-it-provides) bullet list — read that
first for the high-level summary; this file is the more detailed reference.

## Document / DOM

- XML documents with `<styles>` (recursive includes), `<scripts>`, `<Frame
  src>` (recursively resolved into one styled tree), and `<snippets>`
  (name -> subtree, instantiated later via `BLoadLayoutSnippet`/
  `BCreateChildren` or a native `instantiate_snippet` callback).
- Comments, CDATA, processing instructions, entities, self-closing tags.
- Node lifetime observers so long-lived state elsewhere (script contexts,
  input hover/focus, scoped watches over a set of nodes across a handler that
  may delete some of them) doesn't dangle.

## CSS

- Full selector cascade: descendant combinators, specificity + source order,
  inline-style override, class/id/attribute/pseudo-class selectors
  (`:hover`/`:active`/`:selected`/`:enabled`/`:disabled`/`:focus`), `:not()`
  and sibling combinators, layout-file cascade scoping.
- `@define` theme variables (recursive substitution) and CSS custom
  properties.
- `@keyframes` (per-stop resolved style + channel bitmask; `from`/`to`/`N%`
  selectors; per-segment `animation-timing-function`) and CSS transitions,
  both driving real interpolation (not just parsed-and-ignored) for opacity,
  position (`x`/`y`/`z` longhands), color/background-color, wash-color,
  brightness, transform, border, box-shadow, blur, and clip channels.
- Panorama sizing primitives: `fit-children`, `fill-parent-flow(ratio)`,
  `width-percentage`/`height-percentage`, coordinate `position` (px and `%`,
  additive), `flow-children` (none/right/down), align, min/max, margin/
  padding/border (border-box).
- Text: `text-align`, `text-transform` (uppercase/lowercase/none),
  `letter-spacing`, `line-height`, `text-overflow` (`ellipsis`/`clip`/
  `shrink` — auto-fit font size — /`noclip`), `text-shadow`, inline
  `<b>`/`<i>` styled runs, WebCore-style ASCII line-break/word-wrap.
- Paint: `border-radius` (px and `%`, per-corner, tessellated arcs),
  `box-shadow` (outset soft falloff + inset, color-first or
  fill/inset-prefixed syntax), linear/radial gradients (including oblique and
  rounded variants), `background-size`/`background-position`
  (stretch/contain/cover/fixed), `-mix-blend-mode` (normal/additive/screen/
  multiply/opaque, canonical premultiplied blend states), `z-index`
  (direct-sibling stacking, not full CSS stacking contexts), `overflow`
  (squish/scroll/clip/noclip, 2-axis scissor clipping with real clip-stack
  intersection).
- `blur: gaussian/fastgaussian/fastanimgaussian(...)` parses fully (std
  deviations + pass count) and a reusable separable-gaussian kernel builder
  is provided; the GPU backdrop-blur *pass* itself (capture-behind + blur +
  composite) is a backend-specific rendering feature, not something the
  engine can implement generically — see `PanoramaRenderBackend::blur_region`.

## Layout

`layout_panorama_tree()` is a two-pass solver (bottom-up intrinsic sizing,
top-down resolve) supporting the box model above plus scrollable overflow,
dropdown popup geometry, and the common control internals (radio groups,
sliders, scrollbars) input needs to hit-test against.

## Scripting

`PanoramaRuntime` embeds QuickJS and exposes a Panorama-shaped surface: `$` as
a callable selector function, a `Panel` class bound to live `PanoramaNode`
pointers, an event bus (`RegisterEventHandler`/`DispatchEvent`/
`RegisterForUnhandledEvent`), `$.Schedule`, dialog-variable token
replacement, and `TriggerClass`/`SetHasClass`/style and visibility mutation
through the same `Panel` API real Panorama scripts use. Most CS:GO-specific
game API namespaces (`GameInterfaceAPI`, `LobbyAPI`, `GameTypesAPI`,
inventory/friends/matchmaking/persona models) are graceful no-op or
falsy-by-default stubs — real scripts run to completion without throwing, but
without a live game backend behind those specific calls. Self-authored,
non-game-backed Panorama-style UI is fully interactive with no stubs
involved.

## Input

`PanoramaInputController` hit-tests the laid-out tree (front-to-back,
ancestor-bubbled `onactivate`/hover) and drives hover/active/focus state,
radio-group exclusivity, dropdown open/select/dismiss, slider drag, and
scrollbar thumb interaction. Keyboard focus navigation (Tab), text-entry
caret/selection editing (`panorama_text_edit.hpp`), and IME composed-text
insertion are supported; the engine does not poll a platform input API
itself — applications translate SDL, Win32, or other platform events into
controller calls.

## Rendering

See [integration.md](integration.md#renderer-responsibilities-recap) for the
`PanoramaRenderBackend` contract and [architecture.md](architecture.md) for
where paint and geometry submission sit in the pipeline.
`PanoramaGeometryCache` (`panorama_geometry_cache.hpp`) is the reference
incremental-submission implementation; using it is optional, but
writing a correct alternative from scratch is nontrivial (see that header's
own documentation for why).

## RML Conversion (Alternate Path)

`panorama_converter.hpp` (`convert_panorama_document()`, `convert_panorama_css()`)
is a separate, best-effort source-to-source converter from Panorama XML/CSS to
RmlUi's RML/RCSS markup for rendering Panorama-authored UI
through RmlUi's own layout and renderer instead of this engine's native
pipeline (everything else on this page). It does not sit in the pipeline
described above and nothing else in the engine depends on it.

The conversion is lossy by design: because RmlUi's box model and CSS dialect
differ from Panorama's, declarations the converter cannot faithfully map are
dropped rather than emitted as markup RmlUi would reject. This includes
Panorama-only sizing primitives (`fit-children`, `fill-parent-flow`,
`width-percentage`/`height-percentage`), gradients, `blur`, transforms,
unresolved `@define`/theme-variable values, and `@keyframes`/`@media` blocks.
`<scripts>` includes are collected in the result but never translated —
Panorama JavaScript has no RmlUi equivalent, so the application decides
separately whether to run them (e.g. against this engine's own `PanoramaRuntime`
alongside the RmlUi-rendered visuals). Expect a reasonable RmlUi rendering of
the document's structure and styling, not pixel parity with the native
pipeline.

## Known limits

- `.pbin` package reading expects Valve-style stored (uncompressed) zip
  entries with no zip data descriptors — real CS:GO packages satisfy this;
  an arbitrarily-produced zip might not.
- Text wrapping uses the built-in WebCore-style ASCII break-opportunity
  finder. There is no ICU, CJK line breaking, hyphenation, or
  break-anywhere fallback.
- `box-shadow` falloff is linear, not Gaussian, and ignores `border-radius`
  (always a rectangular shadow shape).
- `z-index` reorders direct siblings only — there are no full CSS stacking
  contexts.
- `overflow` clipping intersects on both axes only; a node clipping on a
  single axis is treated as not clipping (rare in practice — most real
  Panorama uses set both).
- Backdrop blur (`blur: gaussian(...)`) parses and the kernel math is
  implemented and unit-tested, but the actual GPU capture-blur-composite pass
  is backend-specific and is not implemented for every backend out of the
  box — check what your `PanoramaRenderBackend` implementation actually does
  in `blur_region`.
- DOM, runtime, input controller, and node lifetime observer state are
  single-threaded. Cascade computation itself can be forked across worker
  threads through `PanoramaStyleSheet::compute_root_style()` and
  `compute_forked_subtree()`, but PanoramaEngine intentionally has no thread
  pool dependency.
- The CMake build registers a headless scripted `PanoramaView` lifecycle smoke
  test.
