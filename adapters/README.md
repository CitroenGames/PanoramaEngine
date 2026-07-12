# GPU adapters

Ready-made implementations of the engine's `PanoramaRenderBackend` GPU contract
(`include/ui/panorama/panorama_render_backend.hpp`) for common graphics APIs.

These are **optional and opt-in**. The PanoramaEngine library itself has no
graphics-API dependency — it produces a renderer-agnostic `PanoramaDrawList` and
never talks to a GPU (see [../docs/architecture.md](../docs/architecture.md)).
These adapters therefore live **outside** the `PanoramaEngine` library target and
are **not compiled into it**. A host that wants one `#include`s the single header
and links the graphics SDK itself; a host that does not simply never includes it
and pays nothing (no extra link dependencies, no SDK requirement).

| Header | API | Host links |
| --- | --- | --- |
| `panorama_d3d12_backend.hpp` | Direct3D 12 (Windows) | `d3d12.lib`, `d3dcompiler.lib` (declared via `#pragma comment`) |
| `panorama_vulkan_backend.hpp` | Vulkan 1.0+ | the Vulkan loader (`vulkan-1`) |

Both are single, header-only, `inline` implementations in the `panorama_adapters`
namespace. They are deliberately "basic generic": each owns exactly what it needs
to turn a `PanoramaDrawList` into GPU draw calls — shaders, one pipeline/PSO per
blend mode, a texture path, per-command vertex/index buffers, an SRV heap
(D3D12) / descriptor pools (Vulkan), scissor, and blend — and nothing about
windowing, swapchain, or the frame loop, which the host already owns.

## The contract each adapter fills

`PanoramaRenderBackend` is a compile-once / render-per-frame interface:

- **Textures** — `generate_texture` / `update_texture` / `release_texture`
  upload straight-alpha RGBA8 (the color space `PanoramaDrawList` uses, including
  the `PanoramaFontAtlas` glyph atlas).
- **Geometry** — `compile_geometry` builds an immutable vertex+index buffer from a
  draw command (positions scaled by `ui_scale` into framebuffer pixels);
  `render_geometry` binds it plus a texture and draws; `release_geometry` frees it.
  The engine's `PanoramaGeometryCache` calls these to reuse unchanged geometry
  across frames — see [../docs/integration.md](../docs/integration.md).
- **State** — `set_scissor` (Panorama `overflow` clipping) and `set_blend_mode`
  (`-mix-blend-mode`: Normal / Additive / Screen / Multiply / Opaque).

`texture == 0` on a draw command means an untextured solid fill; both adapters
bind an internal 1×1 white texture for it, as the contract specifies.

## Host responsibilities

The host owns the device, queue, swapchain, and render target; it **injects**
them once and hands the adapter the command list/buffer it is recording each
frame:

1. Construct the backend with an init struct (device + present queue + target
   format).
2. `panorama::set_panorama_render_backend(&backend);` then load fonts/textures —
   `PanoramaFontAtlas::load()` uploads the glyph atlas through the backend.
3. Per frame, after binding your render target and (D3D12) inside your open
   command list / (Vulkan) inside your render pass:

   ```cpp
   backend.new_frame(cmd, fb_width, fb_height);
   geometry_cache.submit(draw_list, backend, ui_scale);   // or replay(backend)
   ```

`new_frame()` sets the current command list/buffer and the orthographic
projection (framebuffer pixels → clip space, with the correct per-API NDC Y
direction). The D3D12 backend also uses it to reclaim GPU resources retired in an
earlier frame once its internal fence proves that frame completed, so it must be
called once per frame.

See the header comment at the top of each file for a full usage sketch and the
exact init-struct fields.

## Limits (both adapters)

- **No backdrop blur.** `blur_region()` (Panorama `blur: gaussian`) is left as the
  base-class no-op; panels that request blur render without it. Subclass and
  override `blur_region()` to add it.
- Colors are treated as **straight (non-premultiplied) alpha** with fixed-function
  source-over blending, matching `PanoramaDrawList`.
- **Single-threaded**, like the rest of the engine: all calls must come from the
  thread that owns the injected queue.
- Texture uploads are **synchronous** (a queue submit + wait). They are intended
  for load-time atlas/image uploads, not per-frame streaming.
- These are starting points, not tuned production renderers: uploads and buffers
  favor simplicity (UPLOAD-heap buffers on D3D12, staging-per-upload on Vulkan)
  over throughput. They are correct and complete for driving Panorama UI; profile
  and specialize if a host needs more.

## Regenerating the Vulkan shaders

The Vulkan adapter embeds precompiled SPIR-V from `shaders/`. See
[shaders/README.md](shaders/README.md) to regenerate it after editing the GLSL.
The D3D12 adapter compiles its HLSL at runtime with `D3DCompile`, so it has no
build step.
