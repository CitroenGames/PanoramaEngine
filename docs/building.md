# Building

PanoramaEngine is built with `sighmake`. The root buildscript includes the
library, the vendored QuickJS buildscript, and all examples.

## Requirements

- A C++20 compiler toolchain.
- `sighmake` available on `PATH`.
- No system QuickJS install is needed; the dependency is vendored in
  `Thirdparty/quickjs-0.15.0/`.

The checked-in buildscript has `Debug` and `Release` configurations for `x64`
and `macOS`.

## Commands

From the repository root:

```powershell
sighmake panorama_engine.buildscript
sighmake --build . --config Debug --parallel 8
```

For an optimized build:

```powershell
sighmake --build . --config Release --parallel 8
```

The exact output directories are defined by the `outdir` entries in
`panorama_engine.buildscript` and `examples/examples.buildscript`.

## Targets

| Target | Type | Source |
| --- | --- | --- |
| `PanoramaEngine` | static library | `src/*.cpp`, public headers under `include/ui/panorama/` |
| `QuickJS` | static library | `Thirdparty/quickjs-0.15.0/quickjs.buildscript` |
| `PanoramaExampleHelloLayout` | console exe | `examples/01_hello_layout/main.cpp` |
| `PanoramaExampleSoftwareRaster` | console exe | `examples/02_software_raster/main.cpp` |
| `PanoramaExampleScriptedUi` | console exe | `examples/03_scripted_ui/main.cpp` |

## Running Examples

The examples are small console applications:

- `PanoramaExampleHelloLayout` prints a laid-out box tree and a hover-state
  style check.
- `PanoramaExampleSoftwareRaster` writes `hello_panorama.bmp` in the current
  working directory.
- `PanoramaExampleScriptedUi` runs a synthetic click sequence through the
  QuickJS runtime and prints DOM state changes.

Run them from a writable directory if you want the raster example to emit its
BMP successfully.

## Validation

This standalone repository does not currently include its own unit-test runner.
The broader host validation lives in OpenStrike as:

```powershell
OpenStrikeTests.exe --panorama
```

Use the examples as local smoke tests after documentation-only changes, and use
the host test runner when changing parser, layout, runtime, input, paint, or
animation behavior.

## Adding Files

- Public headers belong under `include/ui/panorama/`.
- Implementation files belong under `src/`.
- New `.cpp` files must be added to `panorama_engine.buildscript`.
- New example `.cpp` files must be wired into `examples/examples.buildscript`.
- Keep host-specific code out of this repository; hosts depend on
  PanoramaEngine, not the other way around.
