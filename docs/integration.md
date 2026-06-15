# Integration Guide

This guide describes the host responsibilities around PanoramaEngine. The
library owns Panorama parsing, styling, layout, scripting, input semantics, and
paint-list generation; the host supplies the platform and game-specific pieces.

## Host Responsibilities

| Responsibility | API |
| --- | --- |
| Layout, CSS, JS, image bytes | `PanoramaResourceProvider` implementations |
| Accurate text metrics | `PanoramaTextMeasure` passed to `layout_panorama_tree` |
| Glyph atlas and glyph placement | `PanoramaGlyphSource` passed to `build_panorama_draw_list` |
| Rendering triangles, textures, scissors, blend modes, blur | Consume `PanoramaDrawList` or implement `PanoramaRenderBackend` |
| Native logging | `set_panorama_log_sink` |
| JS-to-engine actions | `PanoramaRuntime::set_host_action_handler` |
| Runtime sublayout/snippet loading | `PanoramaRuntime::set_layout_loaders` |

All pointer input is passed in design-space coordinates. If the host renders
the UI scaled to a framebuffer, convert OS mouse coordinates back to the same
design space used for `layout_panorama_tree`.

## Resource Setup

`PanoramaDocumentSession` owns a `PanoramaResourceManager`. Add one or more
providers before loading a document:

```cpp
PanoramaDocumentSession session;
session.resources().add_provider(
    std::make_unique<PanoramaDirectoryResourceProvider>(resource_root),
    0,
    "loose-files");
```

Available providers:

| Provider | Use |
| --- | --- |
| `PanoramaMemoryResourceProvider` | Tests, generated documents, examples |
| `PanoramaDirectoryResourceProvider` | Loose files under a resource root |
| `PanoramaPackageResourceProvider` | `.pbin` package contents |

Providers are priority ordered. Use identifiers when you need to remove or
replace a provider at runtime.

## Document Load

```cpp
PanoramaDocumentSessionOptions options;
options.resource_root = resource_root;
options.localize_text = true;

if (!session.load("panorama/layout/mainmenu.xml", options))
{
    return false;
}

PanoramaNode& root = *session.document().root;
session.style_sheet().compute(root);
panorama_apply_visibility_overrides(root);
panorama_apply_control_presentation(root);
panorama_capture_anim_targets(root);
layout_panorama_tree(root, viewport_width, viewport_height, text_measure);
```

`load` parses XML, expands `<Frame>` references, gathers stylesheet sources,
collects script includes, loads snippets, applies localization when enabled, and
builds the stylesheet cascade.

## Runtime Setup

For simple documents that only have root-level scripts, this is enough:

```cpp
PanoramaRuntime runtime;
runtime.initialize(root, session.resources(), session.document().script_includes);
```

For real Panorama assets, prefer the context-preserving path. It keeps each
script's `$.GetContextPanel()` bound to the layout root that included it,
including frame and sublayout scripts:

```cpp
std::vector<PanoramaRuntimeScriptInclude> scripts;
scripts.reserve(session.script_refs().size());
for (const PanoramaScriptInclude& script : session.script_refs())
{
    scripts.push_back({script.path, script.context});
}

PanoramaRuntime runtime;
runtime.initialize_with_script_contexts(root, session.resources(), scripts, resource_root);
```

Set host hooks before `initialize` if scripts may use them during startup:

```cpp
runtime.set_host_action_handler(
    [&](const std::string& action, const std::string& arg)
    {
        if (action == "cmd")
        {
            run_console_command(arg);
        }
        else if (action == "play")
        {
            start_match_from_panorama(arg);
        }
    });
```

## Sublayout And Snippet Loading

`BLoadLayout`, `LoadLayout`, `BLoadLayoutSnippet`, and `BCreateChildren` are
host-wired through `set_layout_loaders`. A loader should mutate the DOM through
the session and execute any newly collected scripts:

```cpp
runtime.set_layout_loaders(
    [&](PanoramaNode& target, const std::string& source)
    {
        PanoramaDocumentLoadResult result = session.load_sublayout(target, source);
        for (const PanoramaScriptInclude& script : result.scripts_added)
        {
            std::optional<std::string> text = session.resources().read_text(script.path);
            if (!text)
            {
                continue;
            }

            PanoramaNode& context = script.context != nullptr ? *script.context : root;
            runtime.run_source_in_context(*text, "panorama://" + script.path, context);
        }
    },
    [&](PanoramaNode& target, const std::string& name)
    {
        session.instantiate_snippet(target, name);
    },
    [&](const std::string& name)
    {
        return session.has_snippet(name);
    });
```

After a loader changes the DOM, the normal dirty path should recompute styles
and relayout.

## Per-Frame Loop

The host typically runs these steps each frame:

1. Feed pointer and wheel state to `PanoramaInputController`.
2. Pump `PanoramaRuntime::update`.
3. If input or scripts changed the DOM or pseudo-class state, recompute styles
   and capture transition targets.
4. Advance transitions, keyframes, and smooth scroll animations.
5. Relayout if the viewport, style, transition, or scroll state changed layout.
6. Build a `PanoramaDrawList`.
7. Submit draw commands to the renderer.

```cpp
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
for (const PanoramaTransitionEnd& end : transitions.transition_ends)
{
    if (end.node != nullptr && end.property != nullptr)
    {
        runtime.dispatch_property_transition_end(*end.node, end.property);
    }
}

PanoramaAnimationAdvanceResult keyframes =
    panorama_advance_keyframes(root, session.style_sheet().keyframes(), dt_seconds);
PanoramaAnimationAdvanceResult scrolls = panorama_advance_scroll_animations(root, dt_seconds);

if (dirty || transitions.layout_changed || keyframes.layout_changed || scrolls.layout_changed)
{
    layout_panorama_tree(root, viewport_width, viewport_height, text_measure);
}

PanoramaDrawList draw_list;
build_panorama_draw_list(draw_list, root, glyph_source, &paint_scratch);
```

If you use `PanoramaStyleSheet::compute_invalidated` for performance, pair it
with `panorama_capture_anim_targets_recomputed`. Use full `compute` for focus
changes because `:focus-within` can change ancestor styles.

## Rendering The Draw List

`build_panorama_draw_list` emits painter-ordered `PanoramaDrawCommand` batches.
Each command contains vertices, indices, texture id, blend mode, optional
scissor rectangle, or a backdrop blur request.

Renderer notes:

- `texture == 0` means an untextured solid batch; bind a 1x1 white texture or
  use a solid-color path.
- Colors are straight RGBA. Premultiply only if your blend state requires it.
- `PanoramaGlyphSource::atlas_texture` is passed through on text commands.
- `PanoramaGlyphSource` and `PanoramaTextMeasure` should use the same font data.
- Backdrop blur commands carry no geometry. Blur the already-rendered
  framebuffer region inside the command rectangle.
- Scissor coordinates are in design pixels; scale to framebuffer pixels when
  rendering at a different resolution.

## Node Lifetime

`PanoramaNode` destruction notifies global `PanoramaNodeLifetimeObserver`s.
`PanoramaRuntime`, `PanoramaInputController`, and `PanoramaDocumentSession`
already observe node destruction and clear their own raw pointers.

If a host caches `PanoramaNode*` across frames or across script calls, either:

- Register a `PanoramaNodeLifetimeObserver`, or
- Use `PanoramaScopedNodeWatch` for a temporary pointer.

JS wrappers become inert when their backing node is destroyed; Panel methods
become safe no-ops and `panel.IsValid()` returns `false`.
