# Building PanoramaEngine

## With CMake

PanoramaEngine has a conventional root CMake project and a namespaced build-tree
alias, `PanoramaEngine::PanoramaEngine`. QuickJS and FreeType are vendored and
configured automatically; the default build has no package-manager downloads.

```powershell
cmake -S . -B build/cmake
cmake --build build/cmake --config Debug --parallel
```

Useful options:

- `PANORAMA_BUILD_EXAMPLES=OFF` builds only the library and dependencies
  (this is already the default when added through `add_subdirectory`).
- `PANORAMA_BUILD_WINDOW_EXAMPLE=OFF` skips the Win32/X11 window example while
  retaining the three headless examples.
- `PANORAMA_BUILD_TESTS=OFF` skips CTest registration (this already defaults to
  off when PanoramaEngine is added through `add_subdirectory`).
- `PANORAMA_MSVC_STATIC_RUNTIME=OFF` uses CMake's default MSVC runtime. This
  defaults to `ON` for a top-level standalone build (matching sighmake) and
  `OFF` as a subdirectory, so PanoramaEngine follows the surrounding CMake
  runtime configuration.

To link the library from another CMake target:

```cmake
add_subdirectory(path/to/PanoramaEngine)
target_link_libraries(MyApplication PRIVATE PanoramaEngine::PanoramaEngine)
```

## With sighmake

```powershell
sighmake panorama_engine.buildscript
sighmake --build . --config Debug --parallel 8
```

`panorama_engine.buildscript` vendors both of its native dependencies under its
own `Thirdparty/` (`quickjs-0.15.0/`, `freetype/`) and includes their
buildscripts itself, so building this one file produces the library and its
dependencies:

- `PanoramaEngine` (the library)
- `QuickJS` and `freetype` (vendored dependencies)

Generate the examples graph instead when the standalone programs are wanted:

```powershell
sighmake examples/examples.buildscript
sighmake --build . --config Debug --parallel 8
```

All build output (`outdir`/`intdir` in every buildscript under this
directory, including the vendored ones) is relative to that buildscript
file's own location and stays under `PanoramaEngine/bin/` and
`PanoramaEngine/build/` — never escaping this folder. The build graph therefore
remains relocatable at any directory depth. A sighmake application links the
`PanoramaEngine` target with `target_link_libraries(...)`; sighmake resolves the
library's declared `outdir` without location-specific path changes.

## With a different build system

PanoramaEngine has no build-system-specific requirements beyond what
`panorama_engine.buildscript` already documents structurally:

- **Standard**: C++20, exceptions on, RTTI on.
- **Sources**: every `.cpp` under `src/` (non-recursive glob — the directory
  is flat).
- **Public include path**: `include/` (applications `#include
  "ui/panorama/panorama_....hpp"`).
- **Private headers**: a couple of `.hpp` files live alongside the sources in
  `src/` (not part of the public surface); no extra include path is needed
  for them since they're only included by sibling `.cpp` files in the same
  directory.
- **Link dependencies**: QuickJS (`Thirdparty/quickjs-0.15.0/`) and FreeType
  (`Thirdparty/freetype/`), both vendored in-tree. Build them as their own
  static libraries (each already has a buildscript documenting its own
  sources/defines if you want to mirror them exactly) and link both into
  whatever consumes `PanoramaEngine`.
- **Platform defines**: on Windows, define `NOMINMAX` and
  `WIN32_LEAN_AND_MEAN` for the same reason most Windows C++ code does
  (`<windows.h>` is not included by PanoramaEngine itself, but MSVC's
  standard headers still see the ambient defines).

Other build systems can mirror these same source and dependency rules.

## Examples

The first three programs under `examples/` are plain console apps with no
window, GPU, or game filesystem dependency — each is a single `main.cpp`
linking only `PanoramaEngine` and its vendored dependencies. They are the
fastest way to confirm a fresh checkout (or a port to a new build system)
actually compiles and runs before wiring up a production renderer. The fourth,
`PanoramaExampleWindowRaster`, is the odd one out: it opens a real Win32 or
X11 window (`win32_main.cpp` / `posix_main.cpp`, platform-conditional in
`examples.buildscript`, so only one compiles per platform) to demonstrate
what a minimal platform windowing layer around the same CPU rasterizer
looks like — see [../examples/README.md](../examples/README.md#04_window_raster).

| Example | What it exercises |
| --- | --- |
| `PanoramaExampleHelloLayout` | In-memory resource provider, document load, cascade, layout, box-tree dump |
| `PanoramaExampleSoftwareRaster` | Building a `PanoramaDrawList` and replaying it through a tiny CPU rasterizer to a `.bmp` |
| `PanoramaExampleScriptedUi` | `PanoramaView` high-level lifecycle + synthetic clicks mutating the DOM through QuickJS |
| `PanoramaExampleWindowRaster` | Same CPU rasterizer as above, live in a Win32/X11 window, loading its layout from a real XML file on disk |

## Tests

The CMake build registers `PanoramaStandaloneScriptedUi`, a headless lifecycle
smoke test covering load/runtime initialization, runtime sublayouts and child
script contexts, idle-frame stability, resize invalidation, synthetic input,
script DOM mutation, cascade, relayout, and draw-list rebuilding:

```powershell
ctest --test-dir build/cmake -C Debug --output-on-failure
```
