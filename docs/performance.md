# Performance Regression Harness

PanoramaEngine ships an opt-in, headless performance-regression executable.
Its default mode is a deterministic correctness and work-count test, not a
wall-clock benchmark.

The generated fixture exercises:

- in-memory resource reads and repeat document loads;
- DOM parsing, stylesheet cascade, layout, text paint, and draw-list output;
- idle, style-mutated, and resized `PanoramaView` frames;
- geometry compilation, unchanged-command reuse, replay, and release;
- the geometry-submit path with optional timing stats disabled; and
- observer-counted document teardown.

Every identical iteration must produce the same canonical output fingerprint,
structural counts, cascade counters, resource counts, geometry work, and
teardown counts. Floating-point output is quantized before hashing so the
fingerprint represents rendered structure rather than object bytes or pointer
values.

## Configure and run the deterministic test

```powershell
cmake -S . -B build/performance `
  -DPANORAMA_BUILD_PERFORMANCE_TESTS=ON `
  -DPANORAMA_BUILD_EXAMPLES=OFF
cmake --build build/performance --config Release --target PanoramaPerformanceRegression --parallel
ctest --test-dir build/performance -C Release -R PanoramaPerformanceDeterministic --output-on-failure
```

`PANORAMA_BUILD_PERFORMANCE_TESTS` defaults to `OFF`, including for a top-level
build. When enabled, `PanoramaPerformanceDeterministic` joins ordinary CTest.
It performs two independent check-mode runs and contains no wall-time
threshold.

## Capture baseline and candidate measurements

Use a controlled, otherwise-idle runner and keep the compiler, build
configuration, power policy, and hardware constant:

```powershell
.\build\performance\Release\PanoramaPerformanceRegression.exe `
  --measure --warmup 5 --iterations 30 --format json --label baseline

.\build\performance\Release\PanoramaPerformanceRegression.exe `
  --measure --warmup 5 --iterations 30 --format csv --label candidate
```

The JSON and CSV records include the label, platform, architecture, compiler,
build configuration, pointer width, hardware-thread count, iteration metadata,
deterministic fingerprint, work counters, every timing sample, and
minimum/median/mean/maximum wall time. The executable never fails because of
wall time. Compare distributions and set policy in a dedicated benchmark
runner rather than encoding a noisy shared-CI threshold.

Run `PanoramaPerformanceRegression --help` for all CLI options. Check mode does
not read the clock around workloads. Geometry submission also exercises the
public null-stats path before validating reuse with stats enabled.

## Interpreting changes

A fingerprint or deterministic-counter change is a correctness-review signal,
not automatically a regression. Intentional rendering or algorithm changes can
legitimately alter it, but the baseline should be updated only after the new
output and work are reviewed.

Timing is supporting evidence only. It does not prove a GPU improvement,
application-level speedup, lower memory use, or lower power use: this harness is
headless and uses a counting render backend. GPU captures, representative
content, allocation/working-set traces, and platform-specific tests remain
separate acceptance levels.
