#!/usr/bin/env python3
"""Deterministic integration smoke tests for melee-pc.

    python3 tools/smoke_test.py --no-disc   # CLI only: no disc image, no GPU
    python3 tools/smoke_test.py             # everything (needs a disc and a GPU)
    python3 tools/smoke_test.py --case vs-match
    python3 tools/smoke_test.py --list

Every case is a subprocess run with a fixed argv and environment, judged on the
process exit status plus regular expressions over its combined output. Nothing
here drives the game with synthetic input or compares pixels: on this machine
`devctl.py key` drops keypresses, and `import -window` under Xwayland can hand
back a stale composited frame, so both are unusable as a pass/fail signal. The
scenes are reached with MELEE_BOOT_SCENE instead and every run is bounded by
MELEE_EXIT_AFTER_FRAMES, which makes the frame count a fixed, comparable number
rather than a wall-clock race. See docs/testing.md.

stdlib only, and no test framework: this has to run from a bare CI image.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
MELEE = ROOT / "build" / "melee"

# A crash is a crash whatever the case was testing.
# "Device lost ... Device was destroyed" is aurora's own teardown message and
# appears on every clean exit, so only an *unexplained* device loss is a fault.
FORBID_ALWAYS = [
    r"PANIC",
    r"\[FATAL\]",
    r"received signal",
    r'assertion "[^"]*" failed',
    r"Device lost(?!.*Device was destroyed)",
]

# Boot-scene evidence. GameModeKind (src/melee/gm/forward.h) and GameStateKind:
# a mode id alone only proves the mode loaded, the state id is what separates
# "Classic's intro is on screen" (state 0) from "the fight is running"
# (state 1, scene 2 = GS_VS).
SCENES = {
    #                 mode, state, scene, frames verified to reach that state
    "title": (0, 0, 0, 900),
    "vs": (14, 1, 2, 1500),
    "classic": (3, 1, 2, 1800),
    "training": (28, 2, 4, 1500),
}


def scene_case(name, scene, extra_env=None, extra_require=(), timeout=300):
    mode, state, kind, frames = SCENES[scene]
    env = {"MELEE_BOOT_SCENE": scene, "MELEE_SEED": "1"}
    env.update(extra_env or {})
    return {
        "name": name,
        "args": ["--no-card", "{disc}"],
        "env": env,
        "needs": "disc+gpu",
        "frames": frames,
        "code": 0,
        "timeout": timeout,
        "require": [
            r"boot scene: game mode %d\b" % mode,
            r"boot scene: state %d scene %d\b" % (state, kind),
            r"graphics backend: ",
        ]
        + list(extra_require),
    }


CASES = [
    # ---- no disc, no GPU: argument handling, which is where a packaged build
    # most often greets a user, and the only part that can run on a bare runner.
    {
        "name": "cli-version",
        "args": ["--version"],
        "needs": "",
        "code": 0,
        "require": [r"melee-pc \S+"],
    },
    {
        "name": "cli-help",
        "args": ["--help"],
        "needs": "",
        "code": 2,
        "require": [r"usage: ", r"--version \| --help"],
    },
    {
        "name": "cli-unknown-option",
        "args": ["--not-an-option"],
        "needs": "",
        "code": 2,
        "require": [r"unknown option --not-an-option", r"usage: "],
    },
    {
        "name": "cli-two-discs",
        # Two paths that exist: each disc argument is opened as it is parsed,
        # so a nonexistent first path would fail before the duplicate check
        # ever ran and the case would pass for the wrong reason.
        "args": ["{melee}", "{melee}"],
        "needs": "",
        "code": 2,
        "require": [r"more than one disc given \("],
    },
    {
        "name": "cli-dvd-no-path",
        "args": ["--dvd"],
        "needs": "",
        "code": 2,
        "require": [r"--dvd needs a disc path", r"usage: "],
    },
    {
        "name": "cli-missing-disc",
        "args": ["/nonexistent/melee.iso"],
        "needs": "",
        "code": 2,
        "require": [r"cannot open disc /nonexistent/melee\.iso"],
    },
    # ---- GPU, no disc
    {
        "name": "launcher-startup",
        "args": [],
        "env": {},
        "needs": "gpu",
        "code": 0,
        "stop_after": 12,
        "timeout": 120,
        # The launcher is the no-disc entry point; reaching its document means
        # RmlUi, the fonts and the swapchain all came up.
        "require": [r"graphics backend: ", r"Loaded font face .*font\.ttf"],
    },
    {
        "name": "launcher-close-repeat",
        "args": [],
        "env": {},
        "needs": "gpu",
        "code": 0,
        "stop_after": 10,
        "repeat": 3,
        "timeout": 180,
        # Open and shut down three times in one pref/cache directory. A
        # shutdown race (an unjoined ARQ worker, a device torn down under
        # aurora) shows up as an abort on a later pass, not the first.
        "require": [r"graphics backend: "],
    },
    {
        "name": "backend-unknown-falls-back",
        "args": [],
        "env": {"MELEE_BACKEND": "definitely-not-a-backend"},
        "needs": "gpu",
        "code": 0,
        "stop_after": 12,
        "timeout": 120,
        # An unknown value must name the valid ones and continue on auto.
        # This needs a GPU despite carrying no disc: backend_from_env() runs
        # while building the AuroraConfig, which is past argument handling.
        "require": [
            r"unknown backend 'definitely-not-a-backend'",
            r"valid values are .*\bvulkan\b",
            r"falling back to auto",
            r"graphics backend: \S+ \(auto\)",
        ],
    },
    # ---- disc + GPU
    scene_case("title-boot", "title"),
    scene_case("vs-match", "vs"),
    scene_case("classic-mode", "classic", timeout=360),
    scene_case("training-mode", "training"),
    {
        # No CLI entry point verifies a disc, and adding a flag only the test
        # uses would be the tail wagging the dog: the SHA-1 path is covered by
        # the launcher_data unit test (tools/test_launcher_data.cpp, ctest
        # -L melee). What a real boot proves instead is that the image parsed,
        # the FST walked, and the region-specific files are the ones on it.
        "name": "disc-inspection",
        "args": ["--no-card", "{disc}"],
        "env": {"MELEE_BOOT_SCENE": "title", "MELEE_SEED": "1"},
        "needs": "disc+gpu",
        "frames": 900,
        "code": 0,
        "timeout": 300,
        "require": [
            r"\[FileCache\] Prewarm finished: \d+ archives",
            r"GmTtAll\.usd",     # USA title archive (.usd is the NTSC-U suffix)
            r"# Distribution 1",  # read out of the disc's language setting
            r"# Language 1",
        ],
    },
    {
        "name": "memcard-create-and-load",
        "args": ["{disc}"],
        "env": {"MELEE_BOOT_SCENE": "title", "MELEE_SEED": "1"},
        "needs": "disc+gpu",
        "frames": 900,
        "code": 0,
        "repeat": 2,
        "timeout": 600,
        # Two passes in one pref directory: the first creates the card image,
        # the second loads it. Asserting the loaded path also proves the run
        # stayed inside the throwaway pref dir instead of touching the real
        # save under $HOME.
        "require": [r"Loaded GC Card Image: {prefdir}/melee-pc/USA/Card A"],
        "require_first": [r"CARD API Initialized"],
        "forbid_first": [r"Loaded GC Card Image"],
    },
    {
        "name": "memcard-no-card",
        "args": ["--no-card", "{disc}"],
        "env": {"MELEE_BOOT_SCENE": "title", "MELEE_SEED": "1"},
        "needs": "disc+gpu",
        "frames": 900,
        "code": 0,
        "timeout": 300,
        # CARDInit still creates a slot A image (it runs before
        # aurora_card_set_present), so the observable difference is that the
        # game never opens a save file on it.
        "require": [r"CARD API Initialized"],
        "forbid": [r"Failed to close file at idx"],
    },
    {
        "name": "audio-init",
        "args": ["--no-card", "{disc}"],
        "env": {
            "MELEE_BOOT_SCENE": "title",
            "MELEE_SEED": "1",
            "MELEE_AUDIO_STATS": "1",
        },
        "needs": "disc+gpu",
        "frames": 900,
        "code": 0,
        "timeout": 300,
        # The voice census is printed from the AX mixer callback against the
        # output clock, so a line at all proves the device opened and is
        # pulling frames; running>0 proves voices are actually being mixed
        # rather than the pool filling with stuck ones.
        "require": [
            r"voices t=[\d.]+s used=[1-9]\d* running=[1-9]",
        ],
    },
    {
        # The window-close path is what runs atexit(pc_shutdown_once); skipping
        # it lets Dawn's static destructors race the live device. Driving a real
        # WM_DELETE_WINDOW needs X11 and python-xlib, which this already does.
        "name": "window-close",
        "cmd": ["python3", "tools/test_window_close.py", "{disc}", "--seconds", "8"],
        "needs": "disc+gpu",
        "code": 0,
        "timeout": 180,
        "require": [r"PASS: normal window close exits with status 0"],
    },
]


def combined(prefdir, out_path):
    """Process output plus the MELEE_LOG_FILE sink.

    Both are read: the log file is flushed per record specifically so a run
    that aborts still has its last lines, while the pipe also catches the raw
    printfs (--version, usage) that never reach the log callback.
    """
    text = out_path.read_text(errors="replace")
    log = prefdir / "log.txt"
    if log.exists():
        text += "\n" + log.read_text(errors="replace")
    return text


def one_pass(case, disc, prefdir, out_path, frames):
    env = dict(os.environ)
    # SDL_GetPrefPath is $XDG_DATA_HOME/melee-pc on Linux, so redirecting
    # XDG_DATA_HOME is enough to keep the run off the real memory card.
    # HOME is deliberately left alone: overriding it loses XAUTHORITY and the
    # x11 video driver then refuses to initialise.
    env["XDG_DATA_HOME"] = str(prefdir)
    env["XDG_CACHE_HOME"] = str(prefdir)
    env["MELEE_LOG_FILE"] = str(prefdir / "log.txt")
    # A repo checkout may carry an extracted iso/ tree that the file cache
    # silently prefers over the disc image. Point it at nothing so every read
    # comes from the image and the run behaves the same here and in CI.
    env["MELEE_FILES_DIR"] = str(prefdir / "no-loose-files")
    env.update(case.get("env", {}))
    if frames is not None:
        env["MELEE_EXIT_AFTER_FRAMES"] = str(frames)

    def subst(s):
        return s.format(disc=disc or "", prefdir=prefdir, melee=MELEE)

    if "cmd" in case:
        argv = [subst(a) for a in case["cmd"]]
        cwd = str(ROOT)
    else:
        argv = [str(MELEE)] + [subst(a) for a in case["args"]]
        cwd = str(ROOT)

    with out_path.open("w") as out:
        proc = subprocess.Popen(argv, cwd=cwd, env=env, stdout=out, stderr=subprocess.STDOUT)
        stop_after = case.get("stop_after")
        try:
            if stop_after is not None:
                # No MELEE_EXIT_AFTER_FRAMES for a launcher run: it never enters
                # the frame loop. SDL turns SIGTERM into SDL_EVENT_QUIT, which
                # aurora reports as AURORA_EXIT -- the same event a window close
                # produces, so this exercises the real shutdown path.
                deadline = time.monotonic() + stop_after
                while time.monotonic() < deadline and proc.poll() is None:
                    time.sleep(0.2)
                if proc.poll() is None:
                    proc.send_signal(signal.SIGTERM)
            return proc.wait(timeout=case.get("timeout", 120)), None
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
            return None, "timed out after %ss" % case.get("timeout", 120)


def check(case, text, code, frames, prefdir, first):
    problems = []
    want = case.get("code")
    if code != want:
        problems.append("exit %r, expected %r" % (code, want))
    if frames is not None and not re.search(
        r"MELEE_EXIT_AFTER_FRAMES: reached frame %d," % frames, text
    ):
        problems.append("no clean exit at frame %d" % frames)
    required = list(case.get("require_first" if first else "require", []))
    for pattern in required:
        if "{prefdir}" in pattern:
            pattern = pattern.format(prefdir=re.escape(str(prefdir)))
        if not re.search(pattern, text):
            problems.append("missing /%s/" % pattern)
    forbidden = FORBID_ALWAYS + list(case.get("forbid", []))
    if first:
        forbidden += list(case.get("forbid_first", []))
    for pattern in forbidden:
        hit = re.search(pattern, text)
        if hit:
            problems.append("forbidden /%s/ matched %r" % (pattern, hit.group(0)[:60]))
    return problems


def run_case(case, disc):
    prefdir = Path(tempfile.mkdtemp(prefix="melee-smoke-"))
    frames = case.get("frames")
    passes = case.get("repeat", 1)
    started = time.monotonic()
    problems, backend = [], "-"
    for i in range(passes):
        out_path = prefdir / ("out%d.txt" % i)
        code, failure = one_pass(case, disc, prefdir, out_path, frames)
        text = combined(prefdir, out_path)
        hit = re.search(r"graphics backend: (\S+)", text)
        if hit:
            backend = hit.group(1)
        if failure:
            problems.append(failure)
            break
        found = check(case, text, code, frames, prefdir, first=(i == 0 and passes > 1))
        if found:
            problems += ["pass %d: %s" % (i + 1, p) for p in found]
            break
    if problems:
        # Keep the evidence for a failure; a pass leaves nothing behind
        # (a pipeline cache is ~75 MB per case).
        problems.append("logs: %s" % prefdir)
    else:
        shutil.rmtree(prefdir, ignore_errors=True)
    return problems, backend, time.monotonic() - started


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--no-disc", action="store_true",
                    help="only cases that need neither a disc image nor a GPU")
    ap.add_argument("--case", action="append", default=[], help="run only this case (repeatable)")
    ap.add_argument("--list", action="store_true", help="list case names and exit")
    args = ap.parse_args()

    if args.list:
        for case in CASES:
            print("%-28s %s" % (case["name"], case.get("needs") or "nothing"))
        return 0

    disc = os.environ.get("MELEE_DISC") or str(ROOT.parent / "melee.ciso")

    cases = CASES
    if args.case:
        cases = [c for c in CASES if c["name"] in args.case]
        unknown = set(args.case) - {c["name"] for c in CASES}
        if unknown:
            print("unknown case(s): %s" % ", ".join(sorted(unknown)), file=sys.stderr)
            return 2
    if args.no_disc:
        cases = [c for c in cases if not c.get("needs")]

    if not MELEE.exists():
        print("%s not built; run: ninja -C build" % MELEE, file=sys.stderr)
        return 2
    if any("disc" in c.get("needs", "") for c in cases) and not os.path.exists(disc):
        print("disc image not found: %s (set MELEE_DISC)" % disc, file=sys.stderr)
        return 2

    print("melee: %s" % MELEE)
    if any("disc" in c.get("needs", "") for c in cases):
        print("disc:  %s" % disc)
    print()
    failed = 0
    for case in cases:
        problems, backend, secs = run_case(case, disc)
        status = "FAIL" if problems else "PASS"
        failed += bool(problems)
        frames = case.get("frames")
        print("%-4s %-28s %6.1fs  frames=%-6s backend=%s"
              % (status, case["name"], secs, frames if frames else "-", backend))
        for problem in problems:
            print("       %s" % problem)
    print()
    print("%d case(s), %d failed" % (len(cases), failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
