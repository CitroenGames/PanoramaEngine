# PanoramaEngine

PanoramaEngine is a self-contained C++20 library for loading, running, laying
out, and painting Valve Panorama UI assets outside Source. It owns the full UI
pipeline: Panorama XML parsing, stylesheet cascade, layout, animation, input,
QuickJS scripting, localization, and renderer-agnostic paint output.

The library is platform-neutral. It has no windowing, graphics API, or game
engine dependency. Applications connect their platform events, native actions,
and renderer through narrow public interfaces. QuickJS is vendored under
`Thirdparty/quickjs-0.15.0/`, and the optional FreeType-backed font atlas uses
the vendored `Thirdparty/freetype/` library.

## What It Provides

- Panorama XML document loading with `<styles>`, `<scripts>`, `<Frame>`, and
  `<snippets>` support.
- Panorama CSS parsing and cascade with `@define`, `@keyframes`, inline styles,
  selector specificity, layout scoping, and Panorama sizing primitives such as
  `fit-children`, `fill-parent-flow`, `width-percentage`, and
  `height-percentage`.
- A layout solver for Panorama's box model, `flow-children`, alignment,
  percentages, scrollable overflow, dropdown popup geometry, and common control
  internals.
- A QuickJS-backed Panorama runtime exposing `$`, `Panel`, event handlers,
  scheduling, sublayout loading hooks, and a native-action callback.
- Pointer and wheel input over the laid-out tree, including hover, active,
  focus, activation bubbling, radio groups, dropdowns, sliders, and scrollbars.
- CSS transitions, `@keyframes`, smooth scroll animations, and transition-end
  delivery hooks.
- A FreeType-backed text atlas that supplies matching layout measurements and
  paint glyph data through the renderer-agnostic backend.
- A renderer-independent `PanoramaDrawList` containing batches of colored or
  textured triangles, scissors, blend modes, and backdrop blur commands.
- `PanoramaView`, a high-level standalone surface that owns document/runtime/
  input state and correctly sequences cascade, animation, layout, and draw-list
  rebuilding for applications that do not need a custom frame coordinator.
- `PanoramaGeometryCache`, an incremental GPU-geometry submitter: replays an
  unchanged frame with zero diffing, and otherwise reuses/recompiles only the
  draw commands that actually changed since the previous frame.
- A best-effort Panorama-to-RmlUi (RML/RCSS) source converter
  (`panorama_converter.hpp`) for rendering Panorama-authored
  UI through RmlUi instead of this engine's native pipeline — a separate,
  lossy alternate path; see
  [docs/panorama-support.md](docs/panorama-support.md#rml-conversion-alternate-path).

## Project Layout

| Path | Purpose |
| --- | --- |
| `include/ui/panorama/` | Public library headers |
| `src/` | Engine implementation |
| `adapters/` | Optional, opt-in `PanoramaRenderBackend` implementations for Direct3D 12 and Vulkan; include one directly and link its graphics SDK when needed |
| `examples/` | Four small standalone examples, including a real window |
| `Thirdparty/quickjs-0.15.0/` | Vendored QuickJS dependency |
| `Thirdparty/freetype/` | Vendored FreeType dependency used by `PanoramaFontAtlas` |
| `docs/` | Build, integration, architecture, and support notes |
| `panorama_engine.buildscript` | Self-contained library build graph; all output stays under `PanoramaEngine/bin` and `PanoramaEngine/build` regardless of nesting depth |

## Quick Start

Generate and build with standard CMake (the vendored QuickJS and FreeType
dependencies are configured automatically):

```powershell
cmake -S . -B build/cmake
cmake --build build/cmake --config Debug --parallel
```

Examples and tests default off when PanoramaEngine is added through
`add_subdirectory()`. The existing `sighmake` path remains supported; see
[docs/building.md](docs/building.md). A default standalone build produces the
library and these programs:

| Example target | Demonstrates |
| --- | --- |
| `PanoramaExampleHelloLayout` | In-memory resources, document load, cascade, layout, and box-tree inspection |
| `PanoramaExampleSoftwareRaster` | Building a `PanoramaDrawList` and replaying it through a tiny CPU rasterizer |
| `PanoramaExampleScriptedUi` | QuickJS runtime, input dispatch, DOM mutation, dirty handling, and relayout |
| `PanoramaExampleWindowRaster` | A dirty-tracked `PanoramaView`, optimized CPU rasterizer, and paced Win32/X11 host loading XML from disk |

More build details are in [docs/building.md](docs/building.md).

## Recommended Integration

`PanoramaView` is the default integration path for an application. It preserves
all low-level extension points, but owns the easy-to-get-wrong frame ordering
and runtime sublayout/focus bridges:

```cpp
#include "ui/panorama/panorama_view.hpp"

using namespace panorama;

PanoramaView view;
view.set_viewport(1280.0F, 720.0F);
view.resources().add_provider(
    std::make_unique<PanoramaDirectoryResourceProvider>(resource_root));

if (!view.load("panorama/layout/mainmenu.xml"))
{
    return;
}

for (;;)
{
    view.update_pointer(mouse_x, mouse_y, mouse_down);
    view.update_wheel(mouse_x, mouse_y, wheel_ticks_y);
    const PanoramaViewUpdateResult changed = view.update(dt_seconds);

    // Feed the renderer-independent list to a custom renderer. It is rebuilt
    // only when style, layout, animation, or explicit visual invalidation
    // actually changed the surface.
    if (changed.visual_changed || redraw_requested)
    {
        render(view.draw_list());
    }
}
```

Configure `view.runtime()` before `load()` to install native actions, bootstrap
scripts, or a runtime client. After setting the active render backend, bind a
loaded `PanoramaFontAtlas` with `set_font_atlas()` to get matching measurement,
glyph discovery, atlas upload, and painting in the correct order; custom text
implementations can instead use `set_text_measure()` plus `set_glyph_source()`.
Native DOM edits can call `mark_style_dirty()` on the changed node;
`PanoramaView::update()` detects the propagated dirty bits.

Standalone applications can make font selection deterministic instead of
depending on content-tree discovery:

```cpp
PanoramaFontAtlasLoadOptions fonts;
fonts.resource_root = resource_root;
fonts.faces = {
    {"fonts/Inter-Regular.ttf", 400},
    {"fonts/Inter-Bold.ttf", 700},
};
if (font_atlas.load(fonts))
    view.set_font_atlas(&font_atlas);
```

When `faces` is empty, `search_directories` and conventional `fonts/`,
`ui/fonts/`, and `resource/ui/fonts/` directories near `resource_root` are
searched. No application-specific current-working-directory path is probed.

## Low-Level Integration

Applications that need custom scheduling or partial-cascade control can compose
the same subsystems directly:

```cpp
#include "ui/panorama/panorama_anim.hpp"
#include "ui/panorama/panorama_document_session.hpp"
#include "ui/panorama/panorama_font_atlas.hpp"
#include "ui/panorama/panorama_geometry_cache.hpp"
#include "ui/panorama/panorama_input.hpp"
#include "ui/panorama/panorama_layout.hpp"
#include "ui/panorama/panorama_paint.hpp"
#include "ui/panorama/panorama_resource_provider.hpp"
#include "ui/panorama/panorama_runtime.hpp"

using namespace panorama;

PanoramaDocumentSession session;
session.resources().add_provider(
    std::make_unique<PanoramaDirectoryResourceProvider>(resource_root));

if (!session.load("panorama/layout/mainmenu.xml"))
{
    return;
}

PanoramaNode& root = *session.document().root;
session.style_sheet().compute(root);
panorama_apply_visibility_overrides(root);
panorama_apply_control_presentation(root);
panorama_capture_anim_targets(root);

PanoramaRuntime runtime;
runtime.initialize(root, session.resources(), session.document().script_includes);

PanoramaFontAtlas font_atlas;
font_atlas.load(resource_root);
layout_panorama_tree(root, view_width, view_height, font_atlas.text_measure());

PanoramaInputController input;
PanoramaGeometryCache geometry_cache;
bool visual_dirty = true;

for (;;)
{
    bool dirty = input.update_pointer(root, mouse_x, mouse_y, mouse_down, &runtime);
    dirty |= input.update_wheel(root, mouse_x, mouse_y, wheel_ticks_y, &runtime);

    runtime.update(dt_seconds);
    dirty |= runtime.consume_dirty();

    if (dirty)
    {
        session.style_sheet().compute(root);
        panorama_apply_visibility_overrides(root);
        panorama_apply_control_presentation(root);
        panorama_capture_anim_targets(root);
        visual_dirty = true;
    }

    PanoramaAnimationAdvanceResult transitions = panorama_advance_anim(root, dt_seconds);
    for (const PanoramaTransitionEnd& ended : transitions.transition_ends)
    {
        if (ended.node != nullptr && ended.property != nullptr)
        {
            runtime.dispatch_property_transition_end(*ended.node, ended.property);
        }
    }
    PanoramaAnimationAdvanceResult keyframes =
        panorama_advance_keyframes(root, session.style_sheet().keyframes(), dt_seconds);
    PanoramaAnimationAdvanceResult scrolls = panorama_advance_scroll_animations(root, dt_seconds);
    visual_dirty = visual_dirty || transitions.visual_changed || keyframes.visual_changed;

    if (dirty || transitions.layout_changed || keyframes.layout_changed || scrolls.layout_changed)
    {
        layout_panorama_tree(root, view_width, view_height, font_atlas.text_measure());
        visual_dirty = true;
    }

    if (!visual_dirty && geometry_cache.replay(render_backend))
    {
        continue; // nothing changed: cheapest possible frame, no diffing at all
    }

    font_atlas.ensure_tree_text(root);
    font_atlas.upload_if_dirty();
    PanoramaDrawList draw_list = build_panorama_draw_list(root, font_atlas.glyph_source());
    geometry_cache.submit(draw_list, render_backend, ui_scale);
    visual_dirty = false;
}
```

For production applications, prefer the script-context preserving runtime setup
shown in [docs/integration.md](docs/integration.md).

## Documentation

- [docs/building.md](docs/building.md) - build requirements, targets, and local
  validation.
- [docs/integration.md](docs/integration.md) - resource loading, runtime setup,
  input, animation, paint, and renderer responsibilities.
- [docs/architecture.md](docs/architecture.md) - pipeline, module map, ownership,
  lifetime, and extension points.
- [docs/panorama-support.md](docs/panorama-support.md) - supported Panorama
  document, CSS, scripting, input, rendering, and known limits.

## Current Limits

- The runtime implements the Panorama UI surface, but most CS:GO game API
  namespaces are graceful stubs. `GameInterfaceAPI`, `LobbyAPI`, and
  `GameTypesAPI` route selected native actions; inventory, friends, matchmaking,
  persona, and similar game-backed APIs are not real data models.
- `.pbin` package reading expects Valve-style stored zip entries: no compression
  and no zip data descriptors.
- Text wrapping follows the built-in WebCore-style line breaker for ASCII break
  opportunities. There is no ICU, CJK line breaking, hyphenation, or
  break-anywhere fallback.
- DOM, runtime, input controller, and node lifetime observer state are
  single-threaded.
- The CMake project registers the scripted `PanoramaView` example as a
  standalone CTest lifecycle smoke test.

## Integration Notes

- The public namespace is `panorama`.
- `PanoramaDrawList` colors are straight, non-premultiplied RGBA. Premultiply in
  the renderer only if the backend blend state requires it.
- Use `PanoramaFontAtlas` when you want the built-in FreeType text path. Custom
  integrations may instead provide their own `PanoramaTextMeasure` and
  `PanoramaGlyphSource`, but both should come from the same font source.
- Application-specific headers and dependencies belong outside PanoramaEngine.
- Expensive diagnostics are configured through `PanoramaDiagnostics` and
  `set_panorama_diagnostics()`. The generic `PANORAMA_TREE_GUARD`,
  `PANORAMA_DISABLE_STYLE_INDEX`, and `PANORAMA_DISABLE_STYLE_SHARING`
  environment switches provide a command-line-friendly alternative.
- If you render through Direct3D 12 or Vulkan, `adapters/` ships ready-made
  `PanoramaRenderBackend` implementations (`panorama_d3d12_backend.hpp`,
  `panorama_vulkan_backend.hpp`) you can `#include` as a starting point instead
  of writing the backend from scratch — see [adapters/README.md](adapters/README.md).
  They are optional and opt-in: the library itself stays graphics-API-free.
