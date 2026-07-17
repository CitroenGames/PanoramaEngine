# Integration Guide

This expands the README's [recommended integration](../README.md#recommended-integration)
and [low-level integration](../README.md#low-level-integration) into a complete
application setup.

## Recommended: `PanoramaView`

New integrations should normally use `PanoramaView`. It owns a
`PanoramaDocumentSession`, `PanoramaRuntime`, `PanoramaInputController`, paint
scratch storage, and the current `PanoramaDrawList`, and it applies the frame
ordering described below internally.

```cpp
PanoramaView view;
view.set_viewport(width, height);
view.resources().add_provider(
    std::make_unique<PanoramaDirectoryResourceProvider>(resource_root));

if (!view.load("panorama/layout/app.xml"))
    return false;

// Every frame:
view.update_pointer(mouse_x, mouse_y, mouse_down);
view.update_wheel(mouse_x, mouse_y, wheel_y);
PanoramaViewUpdateResult changed = view.update(dt_seconds);
if (changed.visual_changed || redraw_requested)
    renderer.render(view.draw_list());
```

The view automatically gives each script its layout-file context panel, wires
runtime `BLoadLayout`/snippet requests back into the document session, routes
script focus through its input controller, forwards transition-end events, and
honors native `PanoramaNode::mark_style_dirty()` propagation. Configure
`view.runtime()` before `load()` for bootstrap scripts or native actions.
After setting the active render backend, bind a loaded `PanoramaFontAtlas` with
`view.set_font_atlas(&atlas)` to have the view perform glyph discovery/upload
before paint, or supply matching custom text measurement and glyph callbacks with
`set_text_measure()`/`set_glyph_source()`.

The remainder of this guide documents the lower-level components for custom
scheduling, partial-cascade threading, or direct geometry-cache submission.

## 1. Resources

Wrap whatever your engine uses for asset storage in a `PanoramaResourceProvider`
and register it on a `PanoramaResourceManager` (owned by a
`PanoramaDocumentSession`, or standalone). Three providers ship out of the box:

- `PanoramaMemoryResourceProvider` — in-memory `path -> bytes` map. Useful for
  tests, for UI authored as C++ string literals instead of
  loading from disk, or as a virtual-path override layer.
- `PanoramaDirectoryResourceProvider` — reads from a real directory.
- `PanoramaPackageResourceProvider` — reads from a parsed `.pbin` package.

Providers are checked in ascending `priority` order — the lowest value goes
first and `read()` returns on the first provider that has the path, so a
application registers an override layer at a *lower* priority than what it
should take precedence over (e.g. an in-memory override at priority `-1` in
front of the real package at the default priority `0`). Ties break by
insertion order (`add_provider(provider, priority, identifier)`). Only `read()`
is required; `resolve_file()` (default: unsupported) lets a provider expose a
real filesystem path when the application needs one (e.g. handing a path to a
system font loader).

## 2. Loading a document

`PanoramaDocumentSession::load()` (or `load_into()` for a sublayout) parses
the XML, recursively resolves `<styles>` includes and `<Frame src>` children
into one styled tree, and computes the initial cascade. It also tracks
per-layout-file cascade scoping (a sublayout's own `<style>`/`<styles>` only
style the subtree it created — real Panorama scoping, needed so a HUD
module's stylesheet cannot restyle a sibling module) and collects `<snippet>`
definitions for later instantiation.

## 3. Scripting (optional)

If the document has `<scripts>`, boot a `PanoramaRuntime` against the loaded
root (`initialize` / `initialize_with_script_contexts`, the latter needed when
sublayouts each need their own `$.GetContextPanel()`). Before calling
`initialize`, wire the hooks the application needs:

- `set_host_action_handler` — routes JS-triggered engine actions (a real
  Panorama script calling into `GameInterfaceAPI`/`LobbyAPI`-style stubs) back
  into native code as `(action, arg)` pairs.
- `set_layout_loaders` — backs `BLoadLayout`/`BLoadLayoutSnippet` so a script
  can load a sublayout or instantiate a snippet at runtime.
- `set_focus_request_handler` — lets `Panel.SetFocus()` move keyboard focus
  through your `PanoramaInputController` (see below), since the runtime does
  not own input.

A document with no scripts needs none of this — the DOM is still fully usable
via direct `PanoramaNode` mutation (`mark_tree_dirty()` equivalent: just
recompute the cascade after mutating).

## 4. Per-frame loop

The shape below is the same one in the README, annotated with why each step
exists and what marks the tree dirty for the next one. A custom coordinator
tracks its own dirty flags (`style dirty`, `layout dirty`, `visual dirty`)
rather than unconditionally recomputing every stage every frame. This lets the
application choose how to batch native DOM mutations and input updates.

1. **Input.** `PanoramaInputController::update_pointer`/`update_wheel`
   hit-test the *previous* frame's laid-out tree (one-frame latency, not
   perceptible) and update hover/active/focus state, firing
   `onmouseover`/`onmouseout`/`onactivate` and radio-group/dropdown/scrollbar
   internals. A return of `true` means something changed — style-dirty the
   tree.
2. **Script pump.** `PanoramaRuntime::update(dt)` runs scheduled callbacks and
   the JS microtask queue, then `consume_dirty()` reports whether a script
   mutated the DOM since the last call — style-dirty the tree if so. Scripts
   can mutate anything (classes, attributes, structure) without telling you
   which nodes changed, so a script-dirty frame should recompute the whole
   cascade, not just an invalidated subset.
3. **Cascade.** `PanoramaStyleSheet::compute()` (full) — a partial
   `compute_invalidated()` exists for integrations that track exactly which
   nodes changed outside the cascade. It is safe only when every touched or
   created node calls `PanoramaNode::mark_style_dirty()`; otherwise the partial
   recompute can miss a change.
4. **Animation advance.** `panorama_advance_anim` (transitions),
   `panorama_advance_keyframes` (needs `sheet.keyframes()`), and
   `panorama_advance_scroll_animations` each return a
   `PanoramaAnimationAdvanceResult` with `visual_changed`/`layout_changed`/
   `active` flags. Dispatch `transition_ends` through
   `PanoramaRuntime::dispatch_property_transition_end` — real Panorama menus
   key panel teardown off this event (a faded-out tab sets `visible = false`
   only once its fade transition completes). `active` staying true (an
   infinite `@keyframes` animation, common for ambient menu glow/pulse
   effects) means visual-dirty every frame indefinitely; this is expected,
   not a bug, and it is exactly what makes `PanoramaGeometryCache::replay()`
   (step 7) legitimately unreachable while such an animation is running.
5. **Layout.** `layout_panorama_tree(root, width, height, text_measure)` only
   needs to re-run when something layout-dirtied the tree (a style recompute,
   a layout-affecting transition/keyframe/scroll, or a viewport resize).
6. **Text + paint.** If using `PanoramaFontAtlas`, call
   `ensure_tree_text(root)` then `upload_if_dirty()` before painting so glyph
   metrics used by layout match what paint rasterizes. Then
   `build_panorama_draw_list(out, root, glyphs, scratch)` — pass a
   `PanoramaPaintScratch` you keep across frames so its command buffers are
   reused rather than reallocated every animated frame.
7. **Geometry submission.** This is the step most worth getting right, and
   the one [`PanoramaGeometryCache`](../include/ui/panorama/panorama_geometry_cache.hpp)
   exists to do for you:

   ```cpp
   if (!visual_dirty && geometry_cache.replay(backend)) {
       return; // nothing changed: cheapest possible frame, no diffing at all
   }
   PanoramaDrawList list = build_panorama_draw_list(root, glyphs, &scratch);
   geometry_cache.submit(list, backend, ui_scale);
   visual_dirty = false;
   ```

   `submit()` diffs each command against the previous frame by content
   signature + texture + blend mode + scissor rect (position-by-position, so
   draw-list command order should be stable frame to frame for the same
   subtree — it already is, since paint always walks the tree in the same
   order): unchanged commands are reused without recompiling, changed ones
   are recompiled, and geometry that fell out of the list is released.
   `valid()` is only true once every command compiled or reused successfully;
   a partial failure releases everything it built that frame rather than
   leave a cache silently missing draw commands, so the next frame retries
   from a clean slate.

   `ui_scale` must be the same value passed to `layout_panorama_tree()` that
   frame — geometry is compiled in framebuffer pixels (design pixels *
   `ui_scale`), and the cache's content signature folds in `ui_scale` for
   exactly this reason (so the same command re-scaled doesn't false-positive
   as unchanged).

   Call `geometry_cache.release()` before destroying or swapping the active
   `PanoramaRenderBackend` (also safe to call from a destructor — it checks
   `panorama_render_backend()` internally and simply forgets the cache
   without calling into a backend that may already be gone).

## Renderer responsibilities recap

Implement `PanoramaRenderBackend` (`panorama_render_backend.hpp`). Only
texture create/release and geometry compile/render/release are pure virtual;
scissor, blend mode, texture in-place update, and backdrop blur all have safe
no-op defaults, so a minimal backend (or the software rasterizer in
`examples/02_software_raster/`) only needs five methods to get pixels on
screen. `PanoramaDrawList` colors are straight (non-premultiplied) RGBA —
premultiply in the backend only if your blend state requires it.

If you target Direct3D 12 or Vulkan, `adapters/` provides drop-in,
header-only implementations of this interface
(`panorama_d3d12_backend.hpp`, `panorama_vulkan_backend.hpp`) that already
handle textures, per-blend-mode pipelines, scissor, and the geometry
compile/render/release path. They are opt-in and SDK-linked (not part of the
library, which stays graphics-API-free); the application injects its
device/queue and hands the adapter the command list it records each frame. See
[../adapters/README.md](../adapters/README.md) for the per-frame usage and
limits (notably: backdrop blur is left unimplemented).

## Application-defined services

PanoramaEngine deliberately does not include: a windowing/input backend (SDL,
Win32, etc. — the application polls input and calls
`PanoramaInputController`), a thread pool (cascade forking across worker
threads can use the split points exposed by
`PanoramaStyleSheet::compute_root_style()` and `compute_forked_subtree()`), a
virtual filesystem/VPK reader, or any specific texture decode format beyond
what a `PanoramaResourceProvider` and
`PanoramaRenderBackend::generate_texture` choose to support. These are
intentionally external because they differ most between applications and
platforms.
