# Examples

Four standalone example programs. The first three are console-only (a single
`main.cpp` with no window, GPU, or game-filesystem dependency), the fastest
way to confirm a fresh checkout (or a port to a new build system) actually
compiles and runs. The fourth puts the same CPU rasterizer behind a real
native window. All of them link only `PanoramaEngine` and its vendored
`Thirdparty/` dependencies. See [../docs/building.md](../docs/building.md)
for build-system-agnostic requirements and
[../docs/integration.md](../docs/integration.md) for production integration
details beyond what these examples show.

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

The recommended standalone path: `PanoramaView` owns the document, QuickJS
runtime, input controller, cascade/layout sequencing, animation pump, and draw
list. Synthetic pointer presses/releases target the laid-out `#Toggle` button.
The document's own `app.js` updates `#Status` and flips a class on each click;
one `view.update()` call performs the required dirty recompute and relayout. The
first click also loads a scripted child layout, proving the view's built-in
runtime sublayout and context-panel bridge.

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
(`panorama_log.hpp`) and from `$.Msg` inside `app.js`. Applications can redirect
both by installing a log sink instead of using the default stderr sink.

## 04_window_raster

The same load/cascade/layout/paint pipeline as 02, but blitted into a real
native window instead of a `.bmp`: `raster_view.hpp` holds the shared
document-loading, layout, and CPU-rasterization code (including a minimal
`PanoramaRenderBackend` that stores textures as plain CPU buffers, so glyph
atlas quads from `PanoramaFontAtlas` rasterize too, not just untextured
panels); `win32_main.cpp` blits the resulting framebuffer with GDI's
`StretchDIBits`, `posix_main.cpp` does the same over Xlib with `XPutImage`.
Only one of the two compiles into the binary per platform (Windows gets
`win32_main.cpp`, Linux/macOS get `posix_main.cpp`), matching the platform
windowing boundary described in
[../docs/integration.md](../docs/integration.md#application-defined-services).

Unlike 01-03, this example loads its layout from a real file on disk via
`PanoramaDirectoryResourceProvider` instead of an in-memory string — pass a
path as the first argument, or run with none to load the bundled
`sample/raster.xml`:

```
$ ./PanoramaExampleWindowRaster [layout.xml]
```

Resizing the window re-runs layout and re-rasterizes at the new size.
Press Escape or close the window to exit.

Text needs an actual font file on disk, which the engine itself
intentionally does not vendor (see
[../docs/architecture.md](../docs/architecture.md#extension-points)'s "Text"
extension point) — but this example bundles
[Lato](https://fonts.google.com/specimen/Lato) (SIL Open Font License, see
`sample/resource/ui/fonts/OFL.txt`) under `sample/resource/ui/fonts/`
(mirrors CS:GO's own content layout, the same path `PanoramaFontAtlas`
already looks for on a real Panorama install), so it renders real glyphs out
of the box. Point it at a `layout.xml` elsewhere with no font under that
same relative path and it logs a warning and skips text instead, same as
example 02.

### POSIX build note

`posix_main.cpp` targets Xlib specifically (not a native Cocoa backend), so
it links against `X11` on Linux out of the box. On macOS it needs
[XQuartz](https://www.xquartz.org/) installed — `examples.buildscript`
already points the macOS config at XQuartz's standard install location
(`/opt/X11`).
