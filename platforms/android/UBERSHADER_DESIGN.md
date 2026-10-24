# Hybrid ubershader feasibility

Research and prototype date: 20 October 2026. An opt-in implementation now exists; see [tablet results](UBERSHADER_TABLET_RESULTS.md).

## Recommendation

Keep the small experimental family of generic GX shaders as a fallback for pending specialized
pipelines. Keep specialized shaders as the normal path. Do not enable exclusive
ubershaders by default on Android before measuring sustained GPU cost.

An ubershader interprets material/GX state at draw time instead of baking that state
into a new shader for every combination. It reduces permutations and can replace
missing draws during compilation; it does not make the specialized compiler faster
or eliminate pipeline creation for fixed-function state.

## Repository evidence

- `extern/aurora/lib/gx/pipeline.cpp::render` returns without drawing when
  `gfx::get_pipeline` reports an unready pipeline. This is the fallback insertion
  point, but the draw currently only carries the specialized pipeline reference.
- `gx/gx.hpp::ShaderConfig` includes vertex formats, texture generators, TEV stages,
  fog, alpha compare, indirect stages and Z-texture state. These become runtime
  inputs, or explicit family keys, for the fallback.
- `gx/shader.cpp::build_shader_source` emits specialized WGSL. Dolphin's GLSL/HLSL
  shader generators cannot be dropped into this WebGPU renderer.
- `gx/shader_info.cpp::build_shader_info` changes uniform packing and texture usage
  with material state. Reusing a specialized draw's bind groups for a generic
  shader would read the wrong data. A stable fallback layout and matching packing
  are required.
- `gfx/pipeline_cache.cpp` serializes Android compilation and accepts only
  device-built cache rows. A fallback does not establish that background driver
  compilation is safe. Keep both protections during the prototype.
- Depth/blend/cull state, attachment formats, sample count and destination-alpha
  passes still require compatible pipeline variants. Precompile the fallback
  variants actually needed before entering gameplay.

## Smallest useful experiment

1. Record missing-draw counts by material/feature combination in a cold-cache run.
   Choose a bounded common family from that evidence; measure coverage rather than
   assuming one texture/TEV stage covers the game.
2. Give that family a stable uniform/texture layout, dummy bindings for unused
   texture slots and runtime TEV/alpha/fog inputs. Support its vertex transforms
   too, or count the remaining specialized vertex permutations explicitly.
3. Precompile its pipeline variants during loading. Preserve specialized draw
   data, and pack fallback data only where the prototype supports the draw.
4. At render time, use the specialized pipeline if ready, otherwise the prepared
   fallback. Unsupported features retain the existing path and are counted. A
   partially implemented family is not advertised as universal coverage.
5. Initially compile specialized pipelines at loading/menu boundaries on Android;
   allow live compilation only under the existing safeguards. Separately test
   whether this driver's asynchronous pipeline API or worker compilation is safe.
   A generic fallback alone cannot prevent a long inline compiler call from
   stalling the frame.
6. Expand only after image comparisons and device measurements show the tradeoff
   is worthwhile. Include indirect texturing, lighting, alpha/depth semantics,
   destination alpha and lines/points in the coverage audit.

WGSL constraints: avoid implicit-derivative texture sampling in non-uniform
control flow. Select among fixed texture bindings with validated uniform control,
explicit gradients or appropriate explicit LOD while preserving GX filtering.
Do not silence uniformity diagnostics to ship an unverified fallback.

## Acceptance measurements

Compare existing shaders and hybrid mode with separate cold and warm caches on
identical stage/fighters/effects at 1x. Record compile wall time, skipped/fallback
and specialized draws, presented-frame p95/p99, GPU pass time, startup time and
sustained temperature/performance. Use side-by-side captures for correctness.

The SM-T505's previous instrumented GPU run averaged 10.254 ms per pass at 1x,
with p95 13.165 ms and p99 15.680 ms. A 60 Hz frame allows 16.667 ms: this is limited
headroom, not a prediction of ubershader performance. Those measurements do not
include an ubershader and cannot prove it will sustain 60 FPS.

## Sources inspected

- Dolphin's actual hybrid selection prefers a ready specialized pipeline and
  falls back to an uber pipeline:
  https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoCommon/VertexManagerBase.cpp
- Dolphin queues/precompiles fallback pipelines:
  https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoCommon/ShaderCache.cpp
- Generic pixel shader and texture-selection implementation:
  https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoCommon/UberShaderPixel.cpp
- Mode definitions:
  https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoCommon/VideoConfig.h

Sources above were read from GitHub raw master on the research date. Dolphin's
2017 blog URL returned HTTP 403 here; claims above rely on inspected source.

## Implemented experiment

`MELEE_ANDROID_UBERSHADERS=1` enables a bounded family for 1–4 TEV stages with
ADD/SUB operations, runtime argument/konst selection, bias, scale, clamp, register
writes and alpha comparison. It reuses existing vertex, texture and fog code.
Texture routing, vertex format, lighting, fog, swap routing and fixed pipeline
state remain family variants. Indirect effects, Z texture, post texture matrices
and emboss/bump texgen retain the specialized path.

Selection happens before shader-info, bindings and uniforms are built. Supported
specialized requests stay at background priority and compile at existing loading
boundaries on Android. A ready specialized pipeline wins; otherwise the selected
generic family gets its matching uniform layout. Previously device-built families
warm first on the next launch. New families can still compile during gameplay.

The opt-in stays disabled by default. The tablet experiment reduced pipeline count
only slightly and increased cold compilation time; it does not justify enabling
this for ordinary users. See the measurement report for coverage and limitations.
