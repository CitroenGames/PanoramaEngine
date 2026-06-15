# PanoramaEngine

A self-contained C++20 library that loads, styles, lays out, scripts, and
paints Valve Panorama UI (the CS:GO menu/HUD UI framework) — with its own DOM,
CSS cascade, layout solver, animation system, input handling, and a QuickJS
runtime exposing the Panorama JavaScript surface (`$`, `Panel`, the event bus).

The library is renderer-, window-, and engine-agnostic. It depends only on the
C++ standard library and a vendored QuickJS (`Thirdparty/quickjs-0.15.0/`).
Everything host-specific — GPU upload, font rasterization, image decoding,
pointer sources, logging — enters through small injectable interfaces.

## Pipeline

```
resources (.pbin / directory / memory)        PanoramaResourceManager
        │
        ▼
PanoramaDocumentSession      XML parse → frame expansion → <styles>/<scripts>
        │                    collection → snippets → localization
        ▼
PanoramaStyleSheet::compute  full cascade: specificity + source order +
        │                    inline styles + @define vars + pseudo-classes
        ▼
panorama_apply_visibility_overrides / panorama_apply_control_presentation
        │
        ▼
layout_panorama_tree         Panorama box model: flow-children, fit-children,
        │                    fill-parent-flow(n), width/height-percentage, ...
        ▼
panorama_advance_anim / panorama_advance_keyframes   CSS transitions + @keyframes
        │
        ▼
build_panorama_draw_list     renderer-agnostic quads (PanoramaDrawList)
        │
        ▼
PanoramaRenderBackend        host: compile_geometry / render_geometry /
                             generate_texture / scissor / blend mode
```

Orthogonal to the paint path:

- **`PanoramaRuntime`** — QuickJS bound to the live `PanoramaNode` tree. Runs
  the document's `<scripts>`, exposes `$('#id')`, Panel methods/properties,
  `$.RegisterEventHandler` / `$.DispatchEvent` / `$.Schedule`, sublayout
  loading (`BLoadLayout`), and a host-action bridge. `consume_dirty()` tells
  the host when scripts mutated the DOM (→ recompute + relayout).
- **`PanoramaInputController`** — pointer interaction over the laid-out tree:
  hit-testing (including dropdown popups), `:hover`/`:active`/`:focus` flag
  propagation, `onactivate`/`onmouseover`/`onmouseout` bubbling, radio-group
  exclusivity, and DropDown open/select/dismiss emulation. The host only feeds
  it design-space pointer samples.

## Modules

| Header (`include/ui/panorama/`) | Responsibility |
| --- | --- |
| `panorama_package.hpp` | Reads `.pbin` packages (`PAN\x02` + stored zip) |
| `panorama_resource_provider.hpp` | Resource abstraction: package / directory / in-memory providers behind one prioritized manager |
| `panorama_xml.hpp` | SAX XML parser used by the DOM builder and converter |
| `panorama_dom.hpp` | `PanoramaNode` tree, document parse, control semantics (dropdowns, radio buttons, text entries) |
| `panorama_style.hpp` | Panorama CSS: parser, `@define`, `@keyframes`, cascade (`compute`) |
| `panorama_text_break.hpp` | Line breaking (WebCore `BreakLines` port) + the greedy word-wrap fitter |
| `panorama_layout.hpp` | Layout solver for Panorama's box model |
| `panorama_anim.hpp` | CSS transitions + `@keyframes` runtime |
| `panorama_paint.hpp` | Display-list builder (`PanoramaDrawList`), glyph-source hook for text |
| `panorama_input.hpp` | Hit-testing + `PanoramaInputController` + cascade post-passes |
| `panorama_render_backend.hpp` | The GPU contract a host implements to draw the display list |
| `panorama_runtime.hpp` | QuickJS Panorama JS surface over the live tree |
| `panorama_document_session.hpp` | One-stop document loader (resources + localization + DOM + stylesheet + snippets) |
| `panorama_localization.hpp` | `#token` localization |
| `panorama_log.hpp` | Standalone log sink (host installs its own) |
| `panorama_converter.hpp` | Legacy Panorama→RML text converter (transitional hosts only) |

## Host integration points

All optional except resources:

| Hook | Purpose | Without it |
| --- | --- | --- |
| `PanoramaResourceProvider` | layout/CSS/script/image bytes | nothing loads |
| `PanoramaTextMeasure` | accurate text metrics for layout | 0.5em/1.2em approximation |
| `PanoramaGlyphSource` | rasterized glyphs for paint | text skipped, boxes still paint |
| `PanoramaRenderBackend` | GPU geometry/texture/scissor/blend | consume `PanoramaDrawList` yourself |
| `set_panorama_log_sink` | route engine logs | stderr |
| `PanoramaRuntime::set_host_action_handler` | JS → engine actions (`cmd <command>`, `play <map> <mode>`) | actions ignored |
| `PanoramaRuntime::set_layout_loaders` | `BLoadLayout` / snippets from JS | no-ops |

## Examples

`examples/` contains three standalone console apps (built as
`PanoramaExampleHelloLayout` / `...SoftwareRaster` / `...ScriptedUi`):

1. **`01_hello_layout`** — in-memory XML + CSS → session → cascade → layout →
   printed box tree, plus a `:hover` recompute.
2. **`02_software_raster`** — the same pipeline, then `build_panorama_draw_list`
   replayed through a tiny CPU triangle rasterizer into `hello_panorama.bmp`.
   Shows the display list is fully renderer-agnostic.
3. **`03_scripted_ui`** — boots `PanoramaRuntime` on the tree, then drives the
   per-frame host loop: `PanoramaInputController::update_pointer` (synthetic
   clicks) → JS `onactivate` runs → `consume_dirty` → recompute → relayout.

## Embedding skeleton

```cpp
using namespace openstrike;

PanoramaDocumentSession session;
session.resources().add_provider(std::make_unique<PanoramaDirectoryResourceProvider>(root));
session.load("panorama/layout/mainmenu.xml");

PanoramaNode& tree = *session.document().root;
PanoramaRuntime runtime;
runtime.initialize(tree, session.resources(), session.document().script_includes);

PanoramaInputController input;
for (;;) // per frame
{
    bool dirty = input.update_pointer(tree, mouse_x, mouse_y, lmb_down, &runtime);
    runtime.update(dt);
    if (dirty || runtime.consume_dirty())
    {
        session.style_sheet().compute(tree);
        panorama_apply_visibility_overrides(tree);
        panorama_apply_control_presentation(tree);
    }
    panorama_advance_anim(tree, dt);
    panorama_advance_keyframes(tree, session.style_sheet().keyframes(), dt);
    panorama_advance_scroll_animations(tree, dt); // smooth-scroll springs
    layout_panorama_tree(tree, view_w, view_h, text_measure);
    PanoramaDrawList list = build_panorama_draw_list(tree, glyphs);
    // submit `list` through your PanoramaRenderBackend
}
```

## Node lifetime

`PanoramaNode` destruction notifies `PanoramaNodeLifetimeObserver`s (a
process-global registry — `panorama_add/remove_node_lifetime_observer`), for
every node individually, parents before children. The library's own long-lived
holders of raw node pointers are observers already:

- **`PanoramaRuntime`** neutralizes the node's JS wrapper (surviving script
  references go inert: every Panel method becomes a safe no-op and
  `panel.IsValid()` returns `false`), frees the node's `Data()` object and
  per-panel event handlers, unregisters `$.RegisterEventHandler` registrations
  whose context panel died, and re-points scheduled-callback / context-stack
  references at the document.
- **`PanoramaInputController`** drops matching hover/focus/scrollbar-drag
  pointers (so `reset()` is only needed for wholesale tree swaps).
- **`PanoramaDocumentSession`** nulls dangling script-include contexts.

Hosts caching `PanoramaNode*` across frames should register an observer too,
or use `PanoramaScopedNodeWatch` for pointers held across a script call.

JS wrappers are cached one-per-node, so the same panel compares `===` in
script and `panel.Data()` is stable across lookups.

## Clip regions

Panorama's `clip:` property (render-time only — no effect on layout or hit
testing) is supported in both shipped forms:

- `clip: rect( top, right, bottom, left )` — edges of the **visible** rect in
  % of the border box; folded into the scissor.
- `clip: radial( cx cy, start, sweep )` — hides the wedge swept **clockwise**
  from `start` (0deg = 12 o'clock) over `sweep` degrees about the centre
  (% of the box). `(0deg, 0deg)` is fully visible, `(0deg, 360deg)` fully
  hidden. The visible complement is decomposed into convex wedges and every
  triangle the subtree emitted is geometrically clipped against them
  (positions/uvs/colours interpolate), so it works for backgrounds, images,
  gradients and text alike, and respects the node's transform (including
  mirroring — `SpinnerRotate`'s `scaleX(-1)`).

Both forms animate (transitions and `@keyframes`) — this is what drives
CS:GO's loading spinner, countdown wipes, and the radial radio menu slices.

## Known limits

- **Script API surface**: `$`, Panel methods, the event bus, scheduling, and
  sublayout loading are real. Most CS:GO *game* API namespaces
  (`FriendsListAPI`, `InventoryAPI`, `CompetitiveMatchAPI`, ...) are inert
  stubs from the bootstrap prelude (`makeStub`): they accept any call and
  return `0`/`''`/nested stubs. `GameInterfaceAPI`/`LobbyAPI`/`GameTypesAPI`
  route real host actions; the rest exist so shipped scripts run unmodified.
- **Pseudo-classes**: `:hover`, `:active`, `:selected`, `:enabled`,
  `:disabled`, `:focus`, `:focus-within`, `:root`. Anything else
  (`:nth-child`, structural selectors, ...) does not match.
- **Packages**: `.pbin` zip entries must be STORE'd (no compression, no data
  descriptors) — matching Valve's shipped packages.
- **Text wrapping**: labels word-wrap to their resolved content width by
  default (real Panorama behaviour; `white-space: nowrap` opts out, and
  `text-overflow: ellipsis`/`shrink` labels keep their single-line truncation
  semantics). Break opportunities follow WebCore's `BreakLines` model —
  breakable spaces plus the printable-ASCII pair table (after `-`/`?`, before
  opening punctuation) — with `\n` as a forced break. No ICU: non-ASCII text
  (CJK) has no break opportunities, and there is no mid-word break-anywhere
  or hyphenation.
- **Scrolling**: wheel ticks and `ScrollParentToMakePanelFit(bImmediate=false)`
  glide via a WebCore `ScrollAnimationSmooth` port (critically-damped spring,
  no overshoot; retargets preserve velocity) — hosts must pump
  `panorama_advance_scroll_animations` each frame and relayout when it reports
  movement. Scrollbar thumb drags and `ScrollToTop`/`ScrollToBottom` stay
  immediate.
- **Threading**: the DOM, runtime, and observer registry are single-threaded
  by design.
- **Animatable properties**: transitions and `@keyframes` cover opacity,
  position, colours, wash/brightness, transform, width/height, border,
  `box-shadow` (WebCore `ShadowData` blending — an absent endpoint blends from
  a default transparent shadow), `blur`, `clip`, and
  `pre-transform-scale2d`. `border-radius` does not animate (unused by the
  CS:GO sheets).
- **border-radius**: per-corner `a b c d` radii are supported (WebCore
  `FloatRoundedRect::Radii` constraint scaling included); the percentage form
  (`50%` circles) applies uniformly only, and `h / v` elliptical corners
  collapse to the horizontal radii.

## Notes

- Colours in the draw list are **straight** (non-premultiplied) RGBA; the host
  premultiplies if its blend states require it.
- The engine must not include host headers (e.g. a game's `core/log.hpp`) —
  the dependency direction is strictly host → engine.
- Unit tests live in the host repo's test runner: `OpenStrikeTests.exe --panorama`.
