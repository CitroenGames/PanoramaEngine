# Examples

Three standalone console programs, each a single `main.cpp` with no window,
GPU, or game-filesystem dependency. They link only `PanoramaEngine` and its
vendored `Thirdparty/` dependencies, and are the fastest way to confirm a
fresh checkout (or a port to a new build system) actually compiles and runs.
See [../docs/building.md](../docs/building.md) for build-system-agnostic
requirements and [../docs/integration.md](../docs/integration.md) for what a
production host does beyond what these examples show.

## Building and running

```powershell
sighmake examples/examples.buildscript
sighmake --build . --config Debug --parallel 8
```

`examples.buildscript` includes `../panorama_engine.buildscript`, so this one
command also builds `PanoramaEngine` and its vendored `QuickJS`/`freetype`
dependencies. Binaries land in `bin/x64/Debug/` (or `bin/posix/Debug/` on
macOS/Linux); run them from that directory (`PanoramaExampleSoftwareRaster`
writes its output image relative to the working directory).

## 01_hello_layout

In-memory resource provider, document load, cascade, and
`layout_panorama_tree`, then a printed box-tree dump. No paint, no runtime —
this is the smallest possible embedding.

```
$ ./PanoramaExampleHelloLayout
Laid out 7 rule(s) at 1280x720:

<root>  box=(0, 0  1280x720)
  <Panel> #Screen  box=(0, 0  1280x720)
    <Panel> #NavBar  box=(0, 0  1280x96)
      <Label> #Title  box=(16, 16  160x38)
      <Button> #PlayButton  box=(200, 16  160x64)
        <Label>  box=(200, 16  36x22)
      <Button> #SettingsButton  box=(368, 16  160x64)
        <Label>  box=(368, 16  72x22)
    <Panel> #Content  box=(0, 96  1280x624)
      <Label>  box=(0, 96  290x24)

#PlayButton background while :hover -> rgba(58, 64, 74, 255)
```

The last line flags `#PlayButton` as hovered, recomputes the cascade, and
prints the resolved `:hover` background — proof the cascade and pseudo-class
matching run without any input controller involved.

## 02_software_raster

Same load/cascade/layout pipeline as example 01, then `build_panorama_draw_list`
and a ~60-line CPU triangle rasterizer that alpha-blends the resulting
`PanoramaDrawList` into an RGBA buffer and writes `hello_panorama.bmp`. No
`PanoramaGlyphSource` is supplied, so text is skipped — panels still paint
backgrounds, borders, `border-radius`, and `box-shadow`, which is enough to
show that the draw list is genuinely renderer-agnostic (a real backend
implements `PanoramaRenderBackend` instead of hand-rolling a rasterizer).

```
$ ./PanoramaExampleSoftwareRaster
draw list: 8 command(s), 759 vertices, 2220 indices
wrote hello_panorama.bmp (960x540)
```

Open `hello_panorama.bmp`: a dark nav bar with a highlighted first button, a
sidebar of rounded tiles (one accented), and a card with a soft drop shadow.

## 03_scripted_ui

The full headless per-frame loop a host runs: `PanoramaRuntime` (QuickJS with
`$`/`Panel` bound to the live tree) plus `PanoramaInputController` driving
synthetic pointer presses/releases at the `#Toggle` button's laid-out
coordinates. The document's own `app.js` updates `#Status` and flips a class
on each click; `runtime.consume_dirty()` and a recompute/relayout follow each
input step, exactly as [docs/integration.md](../docs/integration.md#4-per-frame-loop)
describes.

```
$ ./PanoramaExampleScriptedUi
[panorama] [panorama] app.js loaded
[panorama] [panorama] runtime started: 1 script(s), 1 event handler(s)
before any input:
  #Status text="Ready" armed=no
[panorama] Panorama click: #Toggle <Button> onactivate
[panorama] [panorama] app.js: AppToggled fired, count = 1
after click 1:
  #Status text="Clicked 1 time(s)" armed=yes
[panorama] Panorama click: #Toggle <Button> onactivate
[panorama] [panorama] app.js: AppToggled fired, count = 2
after click 2:
  #Status text="Clicked 2 time(s)" armed=no
```

`[panorama] ...` lines come from the engine's own logger
(`panorama_log.hpp`) and from `$.Msg` inside `app.js` — a host redirects both
by installing its own log sink instead of the default stderr one.
