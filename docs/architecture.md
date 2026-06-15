# Architecture

PanoramaEngine is arranged as a host-neutral UI pipeline. It parses and runs
Panorama assets into an internal DOM, then emits a renderer-agnostic draw list.

## Pipeline

```text
resources (.pbin / directory / memory)
        |
        v
PanoramaDocumentSession
        |  XML parse, frame expansion, snippets, localization,
        |  stylesheet/script collection
        v
PanoramaStyleSheet::compute
        |  cascade, specificity, source order, inline styles,
        |  @define, layout scopes, pseudo-class state
        v
panorama_apply_visibility_overrides
panorama_apply_control_presentation
        |
        v
layout_panorama_tree
        |  Panorama box model, flow-children, fill-parent-flow,
        |  fit-children, percentage sizing, scroll geometry
        v
PanoramaRuntime / PanoramaInputController
        |  JS events, DOM mutation, pointer/wheel state,
        |  dirty tracking
        v
panorama_advance_anim / panorama_advance_keyframes
panorama_advance_scroll_animations
        |
        v
build_panorama_draw_list
        |  renderer-agnostic commands
        v
host renderer
```

The runtime and input controller are logically parallel to paint: they mutate
DOM state, pseudo-class flags, attributes, classes, text, and scroll offsets,
then the host decides when to recompute, relayout, and repaint.

## Module Map

| Header | Responsibility |
| --- | --- |
| `panorama_package.hpp` | Reads `.pbin` packages using Valve-style stored zip entries |
| `panorama_resource_provider.hpp` | Memory, package, directory, and prioritized resource lookup |
| `panorama_xml.hpp` | SAX XML parser |
| `panorama_dom.hpp` | `PanoramaNode`, parsed document tree, controls, scrolling, dropdowns, lifetime observers |
| `panorama_document_session.hpp` | One-stop document loading, frame expansion, snippets, stylesheet/script collection |
| `panorama_localization.hpp` | `#token` localization and tree localization |
| `panorama_style.hpp` | CSS parser, selectors, cascade, computed style, `@define`, `@keyframes` |
| `panorama_layout.hpp` | Intrinsic and resolved layout passes |
| `panorama_text_break.hpp` | WebCore-style line break and word-wrap helpers |
| `panorama_anim.hpp` | CSS transitions, keyframes, smooth scroll animation advancement |
| `panorama_input.hpp` | Hit testing, hover/active/focus, activation, wheel, dropdown, scrollbar, slider behavior |
| `panorama_runtime.hpp` | QuickJS Panorama `$` and `Panel` surface |
| `panorama_paint.hpp` | Display-list builder, glyph-source contract, paint scratch storage |
| `panorama_render_backend.hpp` | Optional backend interface for texture and geometry submission |
| `panorama_log.hpp` | Host-installable log sink |
| `panorama_converter.hpp` | Legacy Panorama-to-RML text converter for transitional hosts |

## Ownership

`PanoramaDocumentSession` owns the active `PanoramaDocument`, stylesheet,
localization table, resources, collected snippets, and script include metadata.
The root DOM node is `session.document().root`.

`PanoramaRuntime` does not own the DOM. It binds QuickJS Panel wrappers to live
`PanoramaNode` instances and observes node destruction so stale wrappers become
inert.

`PanoramaInputController` also does not own the DOM. It stores transient
pointers for hover, focus, drag, and scrollbar state, and observes node
destruction to clear them.

`PanoramaDrawList` owns command vectors for one paint pass. Use
`PanoramaPaintScratch` to keep reusable command storage alive across frames.

## Style And Layout Flow

The required order after loading or DOM/style mutation is:

1. `PanoramaStyleSheet::compute` or `compute_invalidated`
2. `panorama_apply_visibility_overrides`
3. `panorama_apply_control_presentation`
4. `panorama_capture_anim_targets` or `panorama_capture_anim_targets_recomputed`
5. `layout_panorama_tree`

The visibility pass reapplies `Panel.visible` overrides after the cascade. The
control presentation pass handles control-specific DOM presentation, such as a
closed dropdown collapsing to its selected option before layout.

Use `compute_invalidated` only when all affected nodes have been marked dirty.
Focus changes need a full `compute` because `:focus-within` can affect ancestor
matches.

## Script Contexts

Real Panorama scripts expect `$.GetContextPanel()` to be the root panel of the
layout file that included the script. `PanoramaDocumentSession::script_refs()`
preserves that association for initial loads, frames, and sublayouts. Hosts
should pass those refs to `PanoramaRuntime::initialize_with_script_contexts`.

When a runtime loader injects a sublayout later, it should execute
`PanoramaDocumentLoadResult::scripts_added` with
`PanoramaRuntime::run_source_in_context` so newly loaded scripts get the same
context semantics.

## Lifetime Rules

`PanoramaNode` destruction notifies every registered
`PanoramaNodeLifetimeObserver`, parents before children. This protects the
engine's own long-lived raw pointers:

- `PanoramaRuntime` neutralizes JS wrappers, Panel `Data()`, event handlers,
  context stacks, and scheduled callbacks.
- `PanoramaInputController` clears hover, focus, drag, and scrollbar pointers.
- `PanoramaDocumentSession` clears script include contexts that point at dead
  layout roots.

Hosts that hold `PanoramaNode*` beyond a local stack scope should observe node
destruction or use `PanoramaScopedNodeWatch`.

## Extension Points

Most host behavior enters through small interfaces:

- Add a new resource source by subclassing `PanoramaResourceProvider`.
- Improve text metrics by providing a `PanoramaTextMeasure`.
- Render text by filling `PanoramaGlyphSource` from the same font data.
- Connect native behavior through `PanoramaRuntime::set_host_action_handler`.
- Add native sublayout/snippet behavior through `set_layout_loaders`.
- Route logs through `set_panorama_log_sink`.
- Consume `PanoramaDrawList` directly, or adapt it to `PanoramaRenderBackend`.

Keep the dependency direction host to engine. Engine code should not include
host headers or assume a particular renderer, filesystem, window, or game API.
