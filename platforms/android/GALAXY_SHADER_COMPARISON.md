# Galaxy Android shader research

Inspected 20 October 2026. Source review only; neither port was built or benchmarked.

## Source snapshots

- GalaxyQuest main: `179739d6d1538ba64f7544e601ccc95748de846a` (19 October).
- OpenPort-Labs/Galaxy-Android main: `25db547c38f92179bf9a0cb94101a98c3b43e2d6` (19 October).

GalaxyQuest is a native OpenGL ES/OpenXR port for Meta Quest. Galaxy-Android is a
related phone/tablet port with a native Vulkan path and an OpenGL ES fallback.
These backends differ from Melee's Aurora/WebGPU/Dawn stack.

## What the code actually does

| Project | Compilation/cache design | What happens on a miss |
| --- | --- | --- |
| GalaxyQuest | Specialized shader generation, linked OpenGL program binaries, source-hash validation, a shared EGL compilation context | Queues urgent shaders before cache rebuilds; draws without a program are skipped. If the shared compiler context cannot be created, compilation runs inline. |
| Galaxy-Android Vulkan | Persistent shader/pipeline inventory; GLSL-to-SPIR-V cache plus device-compatible Vulkan pipeline cache; separate startup compilation and exact pipeline warm-up | A worker compiles missing SPIR-V; the render thread installs pipelines. It can use a simplified placeholder shader while the real material is pending. |
| Our Melee Android | Cached pipeline configurations plus Dawn cache; device-built-only filtering; scene associations and serialized inline compilation | Missing specialized pipelines can skip draws; scene warm-up prioritizes known configurations but has a 250 ms entry budget. |

No complete GX ubershader was found in the inspected paths. In Galaxy-Android,
`placeholderPipeline` creates a zero-initialized ShaderUid and preserves vertex
flags and the draw's pipeline state. It does not interpret the original material's
TEV state, so it must not be treated as a visually equivalent universal fallback.
It can itself compile synchronously if its stub is not cached.

Galaxy-Android's runtime worker translates GLSL to SPIR-V; the render thread still
calls `createPipeline`. Its drain loop stops after four pipelines or five elapsed
milliseconds, but the budget is checked after a creation call: one expensive driver
call can overrun it. This is not evidence that parallel driver compilation would
be safe in our Android backend.

Its inventory combines a canonical catalog, a persisted learned inventory and an
optional cooked asset manifest. Without that manifest, directory scanning counts
unparsed archives and reports gaps; it does not extract all material states from
them. Its startup readiness check logs a warning rather than stopping gameplay if
required pipelines are missing. Neither approach proves complete shader coverage.

## Best adaptations for Melee

1. **Check actual pipeline readiness.** Track scene-required configurations that
   successfully produced compatible pipelines, failures, unknown draws and elapsed
   build time. A queue draining or a shader module existing does not alone prove
   a usable draw pipeline exists.
2. **Warm exact scene pipelines before the match.** Extend our existing stage /
   fighter / costume history rather than introducing a second inventory. Offer
   clear progress while building the selected scene's known pipeline variants.
   This trades a longer load for fewer in-match misses; first-time effects remain
   unknown and must be counted.
3. **Report gaps and learn them.** Record uncached material/pipeline combinations
   during a repeatable scene/effect tour. Keep shader/config version and backend
   compatibility checks; continue using on-device builds on Android.
4. **Keep the two costs visible.** Profile shader/module translation separately
   from final pipeline creation where Dawn permits. Do not bolt Galaxy's shaderc
   and direct VkPipelineCache calls onto WebGPU: reuse Dawn's cache facilities.
5. **Continue the hybrid fallback experiment separately.** A correct fallback
   needs our stable bindings and runtime GX/TEV evaluation described in
   [UBERSHADER_DESIGN.md](UBERSHADER_DESIGN.md). A flat placeholder could replace
   absent geometry with incorrect colors, transparency or depth, and is not an
   acceptable substitute for that work.

Recommended next implementation: scene-specific readiness/progress and an explicit
wait for the selected scene's known pipelines, retaining the existing Android driver
safeguards. Measure cold/warm loading time, late/skipped draws and presented-frame
p95/p99 against the current 250 ms warm-up. No runtime improvements are claimed here.

## Primary source links

- [GalaxyQuest compiler, binary cache and missing-draw behavior](https://github.com/bigmak94/GalaxyQuest/blob/179739d6d1538ba64f7544e601ccc95748de846a/platform/src/gx/gl_renderer.cpp#L468)
- [Galaxy-Android startup phases](https://github.com/OpenPort-Labs/Galaxy-Android/blob/25db547c38f92179bf9a0cb94101a98c3b43e2d6/platform/mobile/render/VulkanLifecycle.cpp#L90)
- [Inventory population, gaps, warm-up and readiness](https://github.com/OpenPort-Labs/Galaxy-Android/blob/25db547c38f92179bf9a0cb94101a98c3b43e2d6/platform/src/gx/vk_shader_inventory.cpp)
- [Runtime worker and pipeline creation budget](https://github.com/OpenPort-Labs/Galaxy-Android/blob/25db547c38f92179bf9a0cb94101a98c3b43e2d6/platform/src/gx/vk/vk_shader_worker.cpp)
- [Missing shader handling and placeholder implementation](https://github.com/OpenPort-Labs/Galaxy-Android/blob/25db547c38f92179bf9a0cb94101a98c3b43e2d6/platform/src/gx/vk/vk_pipeline_manager.cpp#L86)
- [SPIR-V cache and shaderc compilation](https://github.com/OpenPort-Labs/Galaxy-Android/blob/25db547c38f92179bf9a0cb94101a98c3b43e2d6/platform/src/gx/vk_shadergen.cpp)
- [Driver pipeline cache compatibility](https://github.com/OpenPort-Labs/Galaxy-Android/blob/25db547c38f92179bf9a0cb94101a98c3b43e2d6/platform/src/gx/vk/vk_pipeline_cache_manager.cpp)
