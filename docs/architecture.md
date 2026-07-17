# Architecture

## Pipeline

```
Panorama XML/CSS bytes
        |  panorama_xml.hpp / panorama_dom.hpp
        v
   PanoramaNode tree  (DOM: tags, attributes, classes, inline style, snippets)
        |  panorama_style.hpp (PanoramaStyleSheet)
        v
   Cascade            (selector matching, specificity, @define, @keyframes registry,
        |               inheritance -> PanoramaNode::computed)
        v
   Layout             panorama_layout.hpp: layout_panorama_tree() -- box model,
        |              flow-children, percentages, fit/fill-parent-flow, overflow
        v
   Animation advance   panorama_anim.hpp: transitions, @keyframes, smooth scroll --
        |               writes interpolated values back into computed/laid-out state
        v
   Paint               panorama_paint.hpp: build_panorama_draw_list() walks the
        |               laid-out tree -> PanoramaDrawList (renderer-agnostic quads)
        v
   Geometry submission  panorama_geometry_cache.hpp: PanoramaGeometryCache diffs the
        |               draw list against the previous frame, compiles only what
        |               changed, and can replay a fully unchanged frame with no
        |               diffing at all
        v
   PanoramaRenderBackend (application-provided GPU contract)
```

Input (`panorama_input.hpp`) and scripting (`panorama_runtime.hpp`) sit beside
this pipeline rather than inside it: input hit-tests the last laid-out tree and
mutates `PanoramaNode` state (hover/active/focus, click dispatch); the QuickJS
runtime mutates the DOM directly through the same `Panel`/`$` API a real
Panorama script would use. Either can dirty the tree, which is why every stage
after cascade is conditional on a per-stage dirty flag rather than always
running — see [integration.md](integration.md) for the actual per-frame
sequencing used by `PanoramaView` and custom coordinators.

## Module map

| Header | Owns |
| --- | --- |
| `panorama_dom.hpp` | `PanoramaNode`, the document tree, node lifetime observers |
| `panorama_style.hpp` | `PanoramaComputedStyle`, `PanoramaStyleSheet` (parse + cascade + `@define` + `@keyframes` registry) |
| `panorama_layout.hpp` | `layout_panorama_tree()`, `PanoramaTextMeasure` |
| `panorama_anim.hpp` | Transition/keyframe/scroll-spring advance, `PanoramaAnimationAdvanceResult` |
| `panorama_paint.hpp` | `PanoramaDrawList`/`PanoramaDrawCommand`, `build_panorama_draw_list()`, `PanoramaGlyphSource` |
| `panorama_geometry_cache.hpp` | `PanoramaGeometryCache` — incremental GPU geometry submission (see [integration.md](integration.md)) |
| `panorama_render_backend.hpp` | `PanoramaRenderBackend` (the GPU contract), `panorama_render_backend()`/`set_panorama_render_backend()` (current-backend global) |
| `panorama_font_atlas.hpp` | `PanoramaFontAtlas` — the built-in FreeType-backed `PanoramaTextMeasure`/`PanoramaGlyphSource` implementation |
| `panorama_input.hpp` | `PanoramaInputController` — pointer/wheel hit-testing, hover/active/focus, radio groups, dropdown/scrollbar internals |
| `panorama_runtime.hpp` | `PanoramaRuntime` — QuickJS interpreter, `Panel`/`$` bindings over `PanoramaNode`, event bus, native-action/layout-loader/focus hooks |
| `panorama_resource_provider.hpp` | `PanoramaResourceManager` + `Memory`/`Package`/`Directory` provider implementations |
| `panorama_document_session.hpp` | `PanoramaDocumentSession` — owns resources + localization + DOM + stylesheet together, handles `<Frame>`/`<styles>` expansion and layout-scoped cascade |
| `panorama_view.hpp` | `PanoramaView` — recommended standalone façade over document/runtime/input/frame sequencing and the current draw list |
| `panorama_package.hpp` | `.pbin` (Valve-style stored zip) package reader |
| `panorama_localization.hpp` | Dialog-variable token replacement / localization table |
| `panorama_text_break.hpp` | WebCore-style ASCII line-break opportunity finder used by wrapping |
| `panorama_text_edit.hpp` | Text-entry caret/selection editing model |
| `panorama_log.hpp` | The engine's sink-based logger with an application-configurable output callback |

## Ownership and lifetime

- **`PanoramaDocumentSession`** is the top-level owner for a loaded document:
  the resource manager, localization table, `PanoramaDocument` (root node +
  script includes + snippets), and `PanoramaStyleSheet` all live inside it and
  share its lifetime. Applications typically own one session per independently
  loaded UI surface (e.g. one for a main menu, one for a HUD overlay).
- **`PanoramaView`** is the recommended high-level surface owner. It
  keeps a document session, runtime, input controller, paint scratch, and draw
  list alive as one surface and performs the normal cascade/animation/layout/
  paint ordering. Its subsystem accessors preserve the lower-level extension
  points; it is a coordinator, not a second rendering or DOM implementation.
- **`PanoramaNode`** ownership is tree-structured (`std::unique_ptr` children
  under the session's root); `PanoramaNodeLifetimeObserver` lets other
  long-lived state (script contexts, input hover/focus targets) find out when
  a node they reference is destroyed instead of holding a dangling pointer.
- **`PanoramaRuntime`** owns the QuickJS interpreter and binds its `Panel`
  objects to `PanoramaNode*`. It does not own the DOM; construct/initialize it
  against a session's root and keep both alive together.
- **`PanoramaGeometryCache`** owns compiled GPU geometry handles tied to
  whichever `PanoramaRenderBackend` last compiled them. Call `release()`
  before destroying or swapping that backend so the cache can never hold a
  dangling handle.
- **`PanoramaFontAtlas`** owns FreeType glyph-atlas textures through the
  active render backend the same way; it is optional — an application may
  supply its own `PanoramaTextMeasure`/`PanoramaGlyphSource` instead (see
  [integration.md](integration.md)).

## Extension points

PanoramaEngine exposes four customization seams, each an interface or a
plain `std::function` field rather than a subclass hierarchy to walk:

1. **Rendering** — implement `PanoramaRenderBackend` (5 required methods:
   texture create/release, geometry compile/render/release; everything else
   has a safe no-op default). `adapters/` ships ready-made, opt-in Direct3D 12
   and Vulkan implementations of this interface
   (`panorama_d3d12_backend.hpp`, `panorama_vulkan_backend.hpp`) that are not
   compiled into the library — the core stays graphics-API-free — but can be
   `#include`d directly; see [../adapters/README.md](../adapters/README.md).
2. **Resources** — implement `PanoramaResourceProvider` (one required `read()`
   method) or use the three shipped providers (`Memory`/`Package`/`Directory`)
   and layer them by priority in a `PanoramaResourceManager`.
3. **Text** — supply a `PanoramaTextMeasure` function and a
   `PanoramaGlyphSource` (glyph + ascent functions) if you don't want the
   built-in FreeType `PanoramaFontAtlas`; both must agree on the same font so
   measured and painted text line up.
4. **Engine actions / sublayout loading** — `PanoramaRuntime::set_host_action_handler`,
   `set_layout_loaders`, and `set_focus_request_handler` are the bridge
   points for application services such as matchmaking, native controls, and
   console commands. A minimal integration can leave all three unset and run
   self-authored, non-game-backed Panorama UI.
