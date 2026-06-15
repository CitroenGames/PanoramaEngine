# PanoramaEngine

PanoramaEngine is a self-contained C++20 library for loading, running, laying
out, and painting Valve Panorama UI assets outside Source. It owns the full UI
pipeline: Panorama XML parsing, stylesheet cascade, layout, animation, input,
QuickJS scripting, localization, and renderer-agnostic paint output.

The library is host-neutral. It has no windowing, graphics API, RmlUi, or game
engine dependency; hosts provide resources, text metrics, glyphs, logging,
native actions, and the final renderer. QuickJS is vendored under
`Thirdparty/quickjs-0.15.0/`.

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
- A renderer-independent `PanoramaDrawList` containing batches of colored or
  textured triangles, scissors, blend modes, and backdrop blur commands.

## Repository Layout

| Path | Purpose |
| --- | --- |
| `include/ui/panorama/` | Public headers and host-facing API |
| `src/` | Engine implementation |
| `examples/` | Three small standalone console examples |
| `Thirdparty/quickjs-0.15.0/` | Vendored QuickJS dependency |
| `docs/` | Build, integration, architecture, and support notes |
| `panorama_engine.buildscript` | Library and example build graph |

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
layout_panorama_tree(root, view_width, view_height, text_measure);

PanoramaRuntime runtime;
runtime.initialize(root, session.resources(), session.document().script_includes);

PanoramaInputController input;

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

    if (dirty || transitions.layout_changed || keyframes.layout_changed || scrolls.layout_changed)
    {
        layout_panorama_tree(root, view_width, view_height, text_measure);
    }

    PanoramaDrawList draw_list = build_panorama_draw_list(root, glyph_source);
    // Submit draw_list to the host renderer.
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
- Layout text measurement and paint glyph data should come from the same font
  source, otherwise labels will measure and render differently.
- The engine should not include host headers. Dependency direction is host to
  PanoramaEngine.
