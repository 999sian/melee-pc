#!/usr/bin/env python3
"""Black-floor (projected fighter shadow) sweep: one case = one stage/aspect/mode/window.

A floor that lost its shadow multiply renders as solid RGB(0,0,0) polygons, so the
screen metric is the fraction of the framebuffer that is exactly black. Dark stage
art (Final Destination's sky, Brinstar Depths) is near-black but not exactly black,
which is why the threshold is on exact zero. The metric only triages: FLAG means a
person or agent must look at the screenshot, CLEAN means there is nothing black
enough on screen for the artifact to be present.

  CLEAN   nothing exactly-black beyond the noise floor
  FLAG    exactly-black pixels above the trigger; look at the shot
  ERROR   the run did not reach the match

--only legacy sets AURORA_LEGACY_TEX_LOD=1, restoring the unconditional
textureSampleBias that produced black floors before the fix: the positive control
for comparing a suspicious case against a known-bad render.

Concurrency is capped globally with lock files, so several agents may run this
at once without stacking GPU contexts. Usage:

  python3 tools/shadow_sweep.py <disc> --stage 14 --aspect 0 --mode 2 \
      --width 1024 --height 768 [--slots 2] [--out DIR] [--only fixed|legacy]
"""
import argparse
import fcntl
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import time

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
ORIGINAL_ASPECT = 4 / 3
# A floor polygon that lost its shadow multiply is multiplied to exactly zero.
# Measured: Temple 4:3 shows 0.11-0.28 exactly-black; a clean Yoshi's Story shot
# 0.0002; Final Destination's night sky is near-black but only 0.05 exactly black,
# so every shot above the trigger still gets looked at rather than auto-failed.
BLACK_TRIGGER = 0.02


def slot(slots, timeout=1800):
    """Take one of `slots` global run slots; blocks until one is free."""
    Path("/tmp/melee-shadow-slots").mkdir(exist_ok=True)
    deadline = time.time() + timeout
    handles = [open(f"/tmp/melee-shadow-slots/{i}", "w") for i in range(slots)]
    while time.time() < deadline:
        for h in handles:
            try:
                fcntl.flock(h, fcntl.LOCK_EX | fcntl.LOCK_NB)
                return h
            except BlockingIOError:
                continue
        time.sleep(2)
    raise SystemExit("no run slot free")


def content_rect(aspect_mode, width, height):
    """Pixel rect of the framebuffer inside the window; the rest is letterbox."""
    want = {0: ORIGINAL_ASPECT, 1: 16 / 9}.get(aspect_mode, max(ORIGINAL_ASPECT, width / height))
    w, h = (int(height * want), height) if width / height > want else (width, int(width / want))
    return ((width - w) // 2, (height - h) // 2, w, h)


def black_metrics(png, rect):
    """Exactly-black share (the artifact) and near-black share (context)."""
    x, y, w, h = rect
    a = np.asarray(Image.open(png).convert("RGB"))[y:y + h, x:x + w]
    if a.size == 0:
        return {"black0": 1.0, "dark": 1.0}
    m = a.max(axis=2)
    return {"black0": round(float((m == 0).mean()), 4), "dark": round(float((m < 18).mean()), 4)}


def run(disc, case, out, legacy):
    out.mkdir(parents=True, exist_ok=True)
    profile = out / "profile" / "melee-pc"
    profile.mkdir(parents=True, exist_ok=True)
    shutil.copytree(Path.home() / ".local/share/melee-pc/USA", profile / "USA", dirs_exist_ok=True)
    (profile / "launcher.cfg").write_text(f"widescreen {case['aspect']}\n")
    env = dict(
        os.environ, SDL_VIDEO_DRIVER="x11",
        MELEE_WINDOW_TITLE=f"melee-shadow-{out.parent.name}-{out.name}",
        MELEE_VSYNC="0", MELEE_TEST_WIDE_INFO="1", MELEE_TEST_MODE=str(case["mode"]),
        MELEE_TEST_STAGE=str(case["stage"]), MELEE_TEST_FRAMES=str(case["frames"]),
        MELEE_TEST_OUTPUT=str(out), MELEE_TEST_WIDTH=str(case["width"]),
        MELEE_TEST_HEIGHT=str(case["height"]),
        XDG_DATA_HOME=str(out / "profile"), XDG_CACHE_HOME=str(out / "cache"))
    env.pop("MELEE_TEST_SINGLE_SHOT", None)
    if legacy:
        env["AURORA_LEGACY_TEX_LOD"] = "1"
    else:
        env.pop("AURORA_LEGACY_TEX_LOD", None)
    cmd = ["gdb", "-q", "-batch", "-ex", "set pagination off", "-ex", "set confirm off",
           "-ex", "set debuginfod enabled off", "-ex", "handle SIGUSR1 nostop noprint",
           "-x", str(ROOT / "tools/special_smash_gdb.py"), "--args", str(ROOT / "build/melee"),
           str(Path(disc).resolve())]
    with (out / "gdb.log").open("w") as log:
        proc = subprocess.Popen(cmd, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT,
                                start_new_session=True)
        try:
            proc.wait(timeout=240)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGINT)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.wait()
    log = (out / "gdb.log").read_text()
    ok = f"SPECIAL_TEST PASS mode={case['mode']} " in log and "received signal" not in log
    rect = content_rect(case["aspect"], case["width"], case["height"])
    shots = sorted(p for p in out.glob("*.png") if p.stem in ("match", "final"))
    return {"ok": ok and bool(shots), "log": str(out / "gdb.log"),
            "shots": {p.stem: dict(path=str(p), **black_metrics(p, rect)) for p in shots}}


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("disc", type=Path)
    p.add_argument("--stage", type=int, required=True)
    p.add_argument("--aspect", type=int, choices=(0, 1, 2), default=0)
    p.add_argument("--mode", type=int, default=2)
    p.add_argument("--width", type=int, default=1024)
    p.add_argument("--height", type=int, default=768)
    p.add_argument("--frames", type=int, default=540)
    p.add_argument("--slots", type=int, default=2)
    p.add_argument("--only", choices=("fixed", "legacy", "both"), default="fixed")
    p.add_argument("--out", type=Path, default=Path("/tmp/melee-shadow"))
    a = p.parse_args()
    case = {k: getattr(a, k) for k in ("stage", "aspect", "mode", "width", "height", "frames")}
    tag = f"s{a.stage}-a{a.aspect}-m{a.mode}-{a.width}x{a.height}"
    base = a.out.resolve() / tag
    shutil.rmtree(base, ignore_errors=True)

    result = {"case": case, "tag": tag}
    with slot(a.slots):
        for variant in (("fixed", "legacy") if a.only == "both" else (a.only,)):
            result[variant] = run(a.disc, case, base / variant, variant == "legacy")
            time.sleep(2)  # let the GPU context go before the next launch

    def worst(v):
        return max((s["black0"] for s in result[v]["shots"].values()), default=1.0)

    variants = [v for v in ("fixed", "legacy") if v in result]
    if not all(result[v]["ok"] for v in variants):
        result["verdict"] = "ERROR"
    elif max(worst(v) for v in variants) >= BLACK_TRIGGER:
        result["verdict"] = "FLAG"
    else:
        result["verdict"] = "CLEAN"
    result["black0"] = {v: worst(v) for v in variants}
    print(json.dumps(result))
    (base / "result.json").write_text(json.dumps(result, indent=1))
    raise SystemExit(0 if result["verdict"] == "CLEAN" else 1)


if __name__ == "__main__":
    main()
