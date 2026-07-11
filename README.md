# PanoramaEngine

PanoramaEngine is a self-contained C++20 library for loading, running, laying
out, and painting Valve Panorama UI assets outside Source. It owns the full UI
pipeline: Panorama XML parsing, stylesheet cascade, layout, animation, input,
QuickJS scripting, localization, and renderer-agnostic paint output.

The library is host-neutral. It has no windowing, graphics API, or game
engine dependency; hosts provide resources, logging, native actions, and the
final renderer. QuickJS is vendored under `Thirdparty/quickjs-0.15.0/`, and the
optional FreeType-backed font atlas is backed by the vendored
`Thirdparty/freetype/` library.

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
  scheduling, sublayout loading hooks, and a host-action bridge.
- Pointer and wheel input over the laid-out tree, including hover, active,
  focus, activation bubbling, radio groups, dropdowns, sliders, and scrollbars.
- CSS transitions, `@keyframes`, smooth scroll animations, and transition-end
  delivery hooks.
- A FreeType-backed text atlas that supplies matching layout measurements and
  paint glyph data through the renderer-agnostic backend.
- A renderer-independent `PanoramaDrawList` containing batches of colored or
  textured triangles, scissors, blend modes, and backdrop blur commands.
- `PanoramaGeometryCache`, an incremental GPU-geometry submitter: replays an
  unchanged frame with zero diffing, and otherwise reuses/recompiles only the
  draw commands that actually changed since the previous frame.
- A best-effort Panorama-to-RmlUi (RML/RCSS) source converter
  (`panorama_converter.hpp`) for hosts that want to render Panorama-authored
  UI through RmlUi instead of this engine's native pipeline — a separate,
  lossy alternate path; see
  [docs/panorama-support.md](docs/panorama-support.md#rml-conversion-alternate-path).

## Repository Layout

| Path | Purpose |
| --- | --- |
| `include/ui/panorama/` | Public headers and host-facing API |
| `src/` | Engine implementation |
| `examples/` | Three small standalone console examples |
| `Thirdparty/quickjs-0.15.0/` | Vendored QuickJS dependency |
| `Thirdparty/freetype/` | Vendored FreeType dependency used by `PanoramaFontAtlas` |
| `docs/` | Build, integration, architecture, and support notes |
| `panorama_engine.buildscript` | Library and example build graph (self-contained: all build output lands under `PanoramaEngine/bin` and `PanoramaEngine/build`, regardless of where this directory sits in a host tree) |

## Quick Start

Generate and build the project with `sighmake`:

```powershell
sighmake panorama_engine.buildscript
sighmake --build . --config Debug --parallel 8
```

The root buildscript includes the examples, so a normal build produces the
library and these standalone programs:

| Example target | Demonstrates |
| --- | --- |
| `PanoramaExampleHelloLayout` | In-memory resources, document load, cascade, layout, and box-tree inspection |
| `PanoramaExampleSoftwareRaster` | Building a `PanoramaDrawList` and replaying it through a tiny CPU rasterizer |
| `PanoramaExampleScriptedUi` | QuickJS runtime, input dispatch, DOM mutation, dirty handling, and relayout |

More build details are in [docs/building.md](docs/building.md).

## Minimal Host Loop

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

using namespace openstrike;

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

For production hosts, prefer the script-context preserving runtime setup shown
in [docs/integration.md](docs/integration.md).

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
  `GameTypesAPI` route selected host actions; inventory, friends, matchmaking,
  persona, and similar game-backed APIs are not real data models.
- `.pbin` package reading expects Valve-style stored zip entries: no compression
  and no zip data descriptors.
- Text wrapping follows the in-repo WebCore-style line breaker for ASCII break
  opportunities. There is no ICU, CJK line breaking, hyphenation, or
  break-anywhere fallback.
- DOM, runtime, input controller, and node lifetime observer state are
  single-threaded.
- Unit tests currently live in the host repository test runner
  (`OpenStrikeTests.exe --panorama`), not in this standalone repository.

## Notes For Hosts

- The public namespace is `openstrike`, matching the original host integration.
- `PanoramaDrawList` colors are straight, non-premultiplied RGBA. Premultiply in
  the renderer only if the backend blend state requires it.
- Use `PanoramaFontAtlas` when you want the built-in FreeType text path. Custom
  hosts may still provide their own `PanoramaTextMeasure` and
  `PanoramaGlyphSource`, but both should come from the same font source.
- The engine should not include host headers. Dependency direction is host to
  PanoramaEngine.
