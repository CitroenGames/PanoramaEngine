# Building PanoramaEngine

## With sighmake (this repository)

```powershell
sighmake panorama_engine.buildscript
sighmake --build . --config Debug --parallel 8
```

`panorama_engine.buildscript` vendors both of its native dependencies under its
own `Thirdparty/` (`quickjs-0.15.0/`, `freetype/`) and includes their
buildscripts itself, so building this one file is enough to produce:

- `PanoramaEngine` (the library)
- `QuickJS` and `freetype` (vendored dependencies)
- `PanoramaExampleHelloLayout`, `PanoramaExampleSoftwareRaster`,
  `PanoramaExampleScriptedUi`, `PanoramaExampleWindowRaster` (the
  `examples/` programs, pulled in transitively because
  `panorama_engine.buildscript` is itself included from
  `examples/examples.buildscript`)

All build output (`outdir`/`intdir` in every buildscript under this
directory, including the vendored ones) is relative to that buildscript
file's own location and stays under `PanoramaEngine/bin/` and
`PanoramaEngine/build/` — never escaping this folder. That is deliberate:
it is what lets the whole `PanoramaEngine/` directory be copied or
git-submoduled into a different host project at any nesting depth without
editing a single path. A host that also uses sighmake just adds
`PanoramaEngine` to its own `target_link_libraries(...)`; sighmake resolves
the dependency's own declared `outdir`, so it does not matter where under the
host tree `PanoramaEngine/` actually lives.

## With a different build system

PanoramaEngine has no build-system-specific requirements beyond what
`panorama_engine.buildscript` already documents structurally:

- **Standard**: C++20, exceptions on, RTTI on.
- **Sources**: every `.cpp` under `src/` (non-recursive glob — the directory
  is flat).
- **Public include path**: `include/` (so hosts `#include
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

A CMake host would typically vendor this whole directory (e.g. as a git
submodule) and do the equivalent of:

```cmake
add_subdirectory(Thirdparty/quickjs-0.15.0)   # or your own QuickJS target
add_subdirectory(Thirdparty/freetype)         # or your own FreeType target
add_library(PanoramaEngine STATIC ${panorama_engine_sources})
target_include_directories(PanoramaEngine PUBLIC include)
target_link_libraries(PanoramaEngine PUBLIC QuickJS freetype)
```

## Examples

The first three programs under `examples/` are plain console apps with no
window, GPU, or game filesystem dependency — each is a single `main.cpp`
linking only `PanoramaEngine` and its vendored dependencies. They are the
fastest way to confirm a fresh checkout (or a port to a new build system)
actually compiles and runs before wiring up a real host renderer. The fourth,
`PanoramaExampleWindowRaster`, is the odd one out: it opens a real Win32 or
X11 window (`win32_main.cpp` / `posix_main.cpp`, platform-conditional in
`examples.buildscript`, so only one compiles per platform) to demonstrate
what a minimal host-owned windowing layer around the same CPU rasterizer
looks like — see [../examples/README.md](../examples/README.md#04_window_raster).

| Example | What it exercises |
| --- | --- |
| `PanoramaExampleHelloLayout` | In-memory resource provider, document load, cascade, layout, box-tree dump |
| `PanoramaExampleSoftwareRaster` | Building a `PanoramaDrawList` and replaying it through a tiny CPU rasterizer to a `.bmp` |
| `PanoramaExampleScriptedUi` | QuickJS runtime + `PanoramaInputController` synthetic clicks mutating the DOM |
| `PanoramaExampleWindowRaster` | Same CPU rasterizer as above, live in a Win32/X11 window, loading its layout from a real XML file on disk |

## Tests

Unit tests for PanoramaEngine currently live in the host repository's own
test runner (`OpenStrikeTests.exe --panorama`, driven by
`projects/openstrike/tests/test_main.cpp`), not inside this directory — see
[Current Limits](../README.md#current-limits) in the README.
