# Audio investigation — 12 September 2026

Three parallel investigations covered music streaming, synth/SFX bookkeeping,
and SDL output. The reported symptom was missing music or music cutting out.

## Confirmed cause and fix

`src/pc/audio.c`, `next_sample`, rejected `loop_addr >= end_addr` and treated
the endpoint as exclusive. HPS music uses three ring buffers spaced 64 KiB
apart. `HSD_Synth_8038ADD0` in `src/sysdolphin/baselib/synth.c` deliberately
points the loop address at the next buffer, then updates the endpoint after
observing the new playback position at the next synth callback. Forward
transitions therefore legitimately exceed the old endpoint.

The mixer now decodes the inclusive endpoint and transitions on address
equality. It restores ADPCM loop history and lets playback advance through
the next buffer while its new endpoint is pending. ARAM bounds checks remain.

## Verification

- Added `tools/test_audio_stream.c` and `tools/test_audio_stream.py`, compiling
  the actual mixer with the configured build flags and no audio device.
- Reproduced failures before the fix for inclusive endpoints and forward ring
  transitions. Tests pass after the fix for forward/backward transitions,
  final PCM8/PCM16/ADPCM sample values, and ADPCM loop history restoration.
- `python3 tools/test_audio_stream.py`: PASS.
- `cmake --build build -j 4`: PASS; rebuilt `build/melee`.
- Live boot comparison with the same disc/save and audio diagnostics:
  original executable produced approximately 20.6 seconds of captured mix,
  with every complete second from 4 through 19 exactly silent. The rebuilt
  executable produced approximately 40 seconds with no completely silent
  one-second interval after startup. Both stream voices remained running at
  40.5 seconds.
- Captures/logs: `/tmp/melee-audio-before.{f32,log}` and
  `/tmp/melee-audio-after.{f32,log}`. Raw format is float32 stereo, 32 kHz.
  Runs were time-limited; forced termination of the second run produced a
  teardown abort after the measured playback interval.

This verifies the reproduced boot music cutoff, not every stage, long match,
or physical speaker output. Diagnostic capture writes run on the audio thread.

## Separate findings requiring targeted reproduction

These were not changed or established as causes of the reported music cutoff.

- `HSD_SynthSFXStopRange` / `HSD_SynthSFXStopNode` in `synth.c`: bank-stop paths
  can free an AX voice and clear its synth metadata without an enclosing
  interrupt mutex. Slot reuse between those operations could orphan a new
  voice. Reproduce with concurrent bank unloading and queued SEM playback.
- `HSD_SynthPStreamHeaderCallback` in `synth.c`: secondary stereo HPS voice
  acquisition lacks the synth-node backlink initialization used by stereo
  SFX, although `dropcallback` relies on it. Exercise secondary voice stealing
  under voice pressure to establish impact.
- Channel-volume fade state in `synth.c` and callback registration in
  `audio.c` have unsynchronized access paths. Investigate with deterministic
  interleaving or race instrumentation.
- The SDL callback waits on the global interrupt mutex and performs optional
  synchronous capture writes. Timing stalls remain possible, but were not
  measured as the cause. SDL rate, format, and requested byte handling are
  consistent: float32 stereo, 32 kHz, 160-sample frames.
- The existing linear interpolator has a one-sample delay and may omit the
  final interpolated sample on voice termination. This is separate from the
  buffer-transition repair and has not been changed.

The workspace already contained extensive audio and unrelated changes. This
investigation preserved them and added only the endpoint fix, regression
tests, and this report.
