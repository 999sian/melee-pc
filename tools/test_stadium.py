#!/usr/bin/env python3
"""Test Stadium completion paths through real scene callbacks under GDB.

Requires GDB with Python, X11/Xwayland, python-xlib and ImageMagick. The harness
automates character selection and completion boundary conditions using debugger writes.
It copies an existing memory card into a private test profile; never writes the
user's card. Logs and screenshots remain in the requested output directory.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess

MODES = {"home-run": 32, "target-test": 15, "10-man": 33, "100-man": 34,
         "3-minute": 35, "15-minute": 36, "endless": 37, "cruel": 38}
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("disc", type=Path)
parser.add_argument("--mode", choices=MODES, action="append")
parser.add_argument("--frames", type=int, default=1800)
parser.add_argument("--output", type=Path, default=Path("/tmp/melee-stadium"))
parser.add_argument("--card-dir", type=Path, default=Path.home() / ".local/share/melee-pc/USA")
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent
args.output = args.output.resolve()
args.output.mkdir(parents=True, exist_ok=True)
results = {}
for name in args.mode or MODES:
    directory = args.output / name
    directory.mkdir(exist_ok=True)
    profile = directory / "profile" / "melee-pc"
    profile.mkdir(parents=True, exist_ok=True)
    shutil.copytree(args.card_dir, profile / "USA", dirs_exist_ok=True)
    env = dict(os.environ, SDL_VIDEO_DRIVER="x11", MELEE_WINDOW_TITLE="melee-stadium-" + name,
               MELEE_VSYNC="0", MELEE_HEAP_CHECK="1", MELEE_TEST_MODE=str(MODES[name]),
               MELEE_TEST_FRAMES=str(args.frames), MELEE_TEST_OUTPUT=str(directory),
               XDG_DATA_HOME=str(directory / "profile"), XDG_CACHE_HOME=str(directory / "cache"))
    command = ["gdb", "-q", "-batch", "-ex", "set pagination off", "-ex", "set confirm off",
               "-ex", "set debuginfod enabled off", "-ex", "handle SIGUSR1 nostop noprint",
               "-x", str(root / "tools/stadium_gdb.py"), "--args", str(root / "build/melee"),
               str(args.disc.resolve())]
    print("TEST " + name, flush=True)
    with (directory / "gdb.log").open("w") as log:
        proc = subprocess.Popen(command, cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            proc.wait(timeout=max(120, args.frames / 20 + 60))
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGINT)
            try:
                proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.wait()
    text = (directory / "gdb.log").read_text()
    passed = (f"STADIUM_TEST PASS mode={MODES[name]} " in text and
              "exited normally" in text and "received signal" not in text and
              "Python Exception" not in text)
    results[name] = {"passed": passed, "gdb_exit": proc.returncode,
                     "markers": [line for line in text.splitlines() if line.startswith("STADIUM_TEST")],
                     "log": str(directory / "gdb.log")}
    (args.output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    print(("PASS " if passed else "FAIL ") + name, flush=True)
    if not passed:
        print("Inspect " + str(directory / "gdb.log"), flush=True)
        break
raise SystemExit(0 if all(row["passed"] for row in results.values()) else 1)
