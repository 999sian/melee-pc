# Android hybrid shader tablet experiment

20 October 2026. **Keep the prototype opt-in. It did not improve cold-cache performance on this tablet.**

## Device and method

Samsung SM-T505, Android 16 / LineageOS 23.2, Adreno 610 with Qualcomm's 2021
Vulkan driver, about 2.7 GB RAM. The disc came from the SD card. Tests used the
compatibility package, 1x resolution, no MSAA, VSync off, performance hints and
GPU timestamps off. The user's original app, disc and saves were retained.

Three sequential captures ran the same seeded CPU VS boot (Mario / Link,
Final Destination) for 1,200 simulation frames. The two cold runs each started
with empty Dawn and pipeline caches; the repeat hybrid run reused its own cache.
Original caches were moved aside and restored afterward. Perfetto retained all
1,199 simulation intervals in each final capture, with no trace error counters.
Initial failed diagnostic attempts are excluded.

The diagnostic boot bypasses the interactive launcher. These figures therefore
include first-use compilation during VS startup; they are not a measurement of
launcher warm-up effectiveness. The 55-second traces also include loading and
shutdown. Simulation intervals are **not displayed FPS**; no presentation or GPU
execution time comparison was collected.

## Results

| Metric | Normal, cold | Hybrid, cold | Hybrid, repeat |
| --- | ---: | ---: | ---: |
| Pipeline creations | 78 | 76 | 76 |
| Total pipeline creation time, log | 9.974 s | 12.648 s | 1.032 s |
| Maximum single pipeline, log | 431.5 ms | 542.3 ms | 25.8 ms |
| Dawn cache misses, final log | 364 | 364 | 0 |
| Mean simulation interval | 25.57 ms | 27.86 ms | 17.84 ms |
| p99 simulation interval | 211.06 ms | 253.02 ms | 25.06 ms |
| Simulation intervals over 33 ms | 70 | 72 | 4 |

The cold hybrid run recorded 107,452 generic, 187,826 specialized and 6,990 skipped
GX draw commands. Generic shaders handled about 36.4% of successful GX draws.
Unsupported selections remained common (178,127 versus 58,189 covered selections);
selection counts are not draw counts. Skipped commands include the existing
pipeline budget behavior and mean this prototype does not eliminate missing draws.

The repeat hybrid launch recorded 109,962 generic, 189,380 specialized and 4,109
skipped draws. Cached pipeline creation was much faster, but this experiment has
no paired repeat normal run, so it establishes no warm-cache performance advantage.
The repeat run's longest simulation interval (1.18 s) occurred during startup;
shader creation alone does not explain every slow interval.

Screenshots showed both fighters, stage, HUD and controls in both modes. The bright
magenta stage detail also appeared with the normal renderer. Different simulation
progress at screenshot times prevents a pixel-for-pixel parity claim. No WebGPU
shader validation errors occurred in the final captures. This is one scene, not
coverage of every material, effect, fighter or stage.

## Implementation and decision

`MELEE_ANDROID_UBERSHADERS=1` enables runtime TEV argument/operation/konst/alpha
controls for a bounded 1–4 stage family. Vertex processing, texture sampling,
lighting, fog, swaps and fixed pipeline state reuse existing code and remain
family variants. Unsupported effects retain specialized rendering. Supported
specialized requests are deferred to existing loading boundaries; Android driver
calls remain serialized. Device-built generic variants warm first on later boots.

Only two pipeline creations were removed in this scene, while average creation
time increased from 127.9 to 166.4 ms. The retained static family variants limit
permutation reduction. Enabling this globally would worsen the measured cold
experience. Keep the normal renderer as default; investigate which static family
variants dominate before expanding the interpreter. Measure sustained GPU cost
and broader visual parity before promoting the experiment.

Linux and Android native builds passed, as did all 63 Melee tests and 261 selected
renderer tests. The family/payload harness enables C++ assertions even in release
builds. Real device validation found and fixed a reserved WGSL identifier. An
initial asleep-screen launch crashed creating a Vulkan surface before shader
execution; later launches used an awake display. That separate lifecycle race
was not fixed by this shader experiment.

## Deliverables and device state

- `dist/Melee-Android-compat-test.apk`: latest build, hybrid off, installed after testing.
- `dist/Melee-Android-hybrid-experimental.apk`: same native build, hybrid on, ordinary launcher; no benchmark boot or automatic exit.
- Both APKs passed signature verification and 16 KB ZIP alignment checks. This does not establish runtime compatibility on a 16 KB page device.
- Original shader caches restored; experimental caches retained in separate private folders. The original `dev.melee.game` installation was not modified.
- Local raw logs, traces, screenshots and analysis: `local/android-ubershader-20261020/` (ignored by Git).

APK SHA-256:

```
normal: 766aee297313d43d2f798569bbd14dd63fad4bfac8082031db19c8410093a842
hybrid: 1a3c0a4b2bfb1f543e8c085540f23c9d23ed3f9bac0b3822b43725b1b1de1a49
```
