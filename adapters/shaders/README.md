# Vulkan adapter shaders

The Vulkan adapter (`../panorama_vulkan_backend.hpp`) embeds precompiled SPIR-V so
it needs no offline shader build step when PanoramaEngine is consumed. This
directory holds the GLSL sources and the generated SPIR-V that the header
`#include`s.

| File | Purpose |
| --- | --- |
| `panorama_ui.vert` / `panorama_ui.frag` | GLSL sources (the actual UI shader) |
| `panorama_ui.vert.spv.inl` / `panorama_ui.frag.spv.inl` | generated SPIR-V as bare comma-separated `uint32_t` words, embedded by the header |

The pipeline is intentionally tiny: the vertex shader transforms framebuffer-pixel
positions by the push-constant orthographic projection and passes UV + straight
(non-premultiplied) RGBA vertex color through; the fragment shader returns
`color * texture(uv)`. Blending is fixed-function (set per `PanoramaBlendMode`).

## Regenerate after editing the GLSL

Requires `glslc` from the Vulkan SDK. From this directory:

```sh
glslc --target-env=vulkan1.0 -O -mfmt=num panorama_ui.vert -o panorama_ui.vert.spv.inl
glslc --target-env=vulkan1.0 -O -mfmt=num panorama_ui.frag -o panorama_ui.frag.spv.inl
```

`-mfmt=num` emits bare comma-separated hex words (no surrounding braces), which is
what the header's `static const uint32_t code[] = { #include ... };` expects. Keep
the vertex input locations (0=pos, 1=uv, 2=color), the `set=0, binding=0` combined
image sampler, and the 64-byte vertex push constant in sync with the pipeline
setup in `panorama_vulkan_backend.hpp` if you change the shader interface.
