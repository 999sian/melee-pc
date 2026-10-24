# Dusklight Android comparison

Checked GitHub on 20 October 2026, including the exact dependency revisions
pinned by Dusklight. The September local working checkout was not used as the
latest source; its origin refs were fetched without changing its files.

- Latest published release: [v2.0.3](https://github.com/TwilitRealm/dusklight/releases/tag/v2.0.3), published 30 September 2026.
- Current main: [`34a144a493b7f57adeb4034d382317c826eb5c8d`](https://github.com/TwilitRealm/dusklight/tree/34a144a493b7f57adeb4034d382317c826eb5c8d), dated 10 October 2026.
- Pinned Aurora: [`3227d76c60e1e782ca576610bce61c9e7744d8be`](https://github.com/encounter/aurora/tree/3227d76c60e1e782ca576610bce61c9e7744d8be).
- Pinned Borealis: [`4ac5e7052a8c49a122f8d57f626b5c75c5ca6968`](https://github.com/encounter/borealis/tree/4ac5e7052a8c49a122f8d57f626b5c75c5ca6968).

## Findings and relevance to Melee

| Area | Dusklight at these revisions | Melee decision |
| --- | --- | --- |
| Odin2/Adreno startup | v2.0.3 explicitly lists a fix for Odin2 and Galaxy S23. Aurora disables Dawn's `use_spirv_reconvergence_mode`. | Already present in `extern/aurora/lib/webgpu/gpu.cpp`. No extra patch or wholesale dependency update is needed for this fix. |
| Android activity | DuskActivity extends BorealisActivity, which extends SDLActivity. The shared shell owns surface, lifecycle, document and argument handling. | Our SDL activity and SAF picker cover existing requirements. Importing the whole shell is a migration, not a demonstrated FPS optimization. |
| Display rate | BorealisSurface defaults to 60 Hz, reapplies its request on surface changes and checks API 30, readiness and surface validity. It can accept other target rates. | AuroraSurface already votes 60 Hz. Keep Melee's simulation pacing at 60 Hz. A higher refresh vote alone is not an optimization. |
| Shader caching | Aurora stores pipeline configurations, orders known rows by first-use frame, uses a compilation worker and presents compilation progress through Dusklight's overlay. | Ship a version/size-checked Android starter inventory, compile locally in the launcher and preserve serialized driver calls; desktop packages retain their existing inventory. |
| Graphics toggles | Release enables `skip_validation` and `disable_robustness`; reconvergence is disabled. | Already present. Do not count these as new improvements. |
| Game modes | Android XML declares support for performance and battery game modes. | Same declarations already exist. This XML alone does not implement adaptive quality or an ADPF hint session. |
| Scheduling/thermals | No `PerformanceHintManager`, `APerformanceHint`, thermal-headroom integration or sustained-performance request was found in the inspected Android shell and native runtime paths. | Our proposed hint trial is additional work, not a Dusklight feature being copied. Melee already requests sustained-performance mode where supported. |
| Device presets | No Adreno/Mali model-to-quality preset table was found in the inspected shell/renderer/settings paths. | Do not infer safe graphics settings from a device model alone. Compare actual driver capabilities and measurements. |
| Build plumbing | Borealis Gradle reads native CMake output metadata to select ABI/API level, native library and matching SDL/Aurora Java sources. | Useful future build-maintenance pattern; does not establish a gameplay speedup. |

## Release APK inventory

The official v2.0.3 APK contains `assets/initial_pipeline_cache.db` (11,104,256
bytes), even though that database is absent from the Git tree. Its 2,695 rows
contain 2,678 GX configurations (version 13, 2,772 bytes each) and 17 Rml
configurations (version 3, 36 bytes each). These are portable pipeline inputs,
not precompiled GPU binaries. Aurora orders them by first-use frame and Dawn
creates binaries for the local device. The overlay reports queued/created
pipelines; this inspection does not establish that Dusklight blocks gameplay
until every pipeline finishes.

Melee now follows that inventory/local-compilation pattern. Android ships a
separate inventory exported from successful Android builds, checked against
the current configuration version and size before queuing. This does not enable the
experimental hybrid shader mode. The raw desktop seed remains excluded from APK assets. The larger current
configuration set was trial-compiled on the tablet with GX error scopes before
export; that does not validate it on the previously affected Adreno 750 driver.
See [startup validation](SHADER_STARTUP_RESULTS.md).

## Exact sources inspected

- [Adreno fix](https://github.com/encounter/aurora/commit/bded88e95d5a1a7c8b2d747843cb8cde17b39838).
- [DuskActivity](https://github.com/TwilitRealm/dusklight/blob/34a144a493b7f57adeb4034d382317c826eb5c8d/platforms/android/app/src/main/java/com/twilitrealm/dusk/DuskActivity.java).
- [BorealisActivity and surface](https://github.com/encounter/borealis/blob/4ac5e7052a8c49a122f8d57f626b5c75c5ca6968/platforms/android/java/dev/encounter/borealis/BorealisActivity.java).
- [Aurora pipeline cache](https://github.com/encounter/aurora/blob/3227d76c60e1e782ca576610bce61c9e7744d8be/lib/gfx/pipeline_cache.cpp).
- [Graphics device setup](https://github.com/encounter/aurora/blob/3227d76c60e1e782ca576610bce61c9e7744d8be/lib/webgpu/gpu.cpp).
- [Compilation overlay](https://github.com/TwilitRealm/dusklight/blob/34a144a493b7f57adeb4034d382317c826eb5c8d/src/dusk/ui/overlay.cpp).
- [Borealis Gradle integration](https://github.com/encounter/borealis/blob/4ac5e7052a8c49a122f8d57f626b5c75c5ca6968/platforms/android/gradle/borealis-application.gradle).

This is source comparison, not a Dusklight hardware benchmark or a guarantee
that its renderer/settings will work better for Melee.
