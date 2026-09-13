#!/usr/bin/env python3
"""Compile the mixer regression with this build's real audio.c flags."""
import json
import pathlib
import shlex
import subprocess
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parent.parent
entries = json.loads((root / "build/compile_commands.json").read_text())
entry = next(e for e in entries if e["file"].endswith("/pc/audio.c"))
args = shlex.split(entry["command"])
# Keep compile definitions/includes, but enable assertions and discard unused
# device functions so this unit test needs neither SDL linkage nor a device.
for option in ("-o", "-c"):
    index = args.index(option)
    del args[index:index + 2]
args = [a for a in args if a != "-DNDEBUG"]
if "--sanitize" in sys.argv:
    args += ["-fsanitize=undefined", "-fno-sanitize-recover=undefined"]
with tempfile.TemporaryDirectory(prefix="melee-audio-test-") as directory:
    exe = pathlib.Path(directory) / "audio_test"
    subprocess.run(args + ["-UNDEBUG", "-ffunction-sections", "-fdata-sections",
                          "-fdiagnostics-color=never", "-no-pie",
                          str(root / "tools/test_audio_stream.c"),
                          "-Wl,--gc-sections", "-lm", "-o", str(exe)],
                   cwd=entry["directory"], check=True)
    subprocess.run([str(exe)], check=True)
