#!/usr/bin/env python3
"""Milestone M0 (docs/netcode-plan.md section 5 and 12): one recorded run must
replay bit-identical on every platform we ship.

    tools/net_determinism.py [--frames 2400] [--seed 7] [--exe build/melee]
                             [--win-exe build-win/melee.exe] [--disc ../melee.ciso]
                             [--work /tmp/net_determinism] [--port 42400]
                             [--only linux,linux-flip,windows] [--keep]

One reference recording is made on this Linux build with a fixed MELEE_SEED, a
fixed frame count and input driven through MELEE_KEY_FIFO (MELEE_DEBUG_VS=cpu,
so Link is keyboard-driven and Mario is a level-4 CPU: a real match, not an
idle one). That single file is then fed back with MELEE_NET_REPLAY to every
platform we can reach here, and src/pc/net_snapshot.c reports the first frame
whose checksum differs ("net: REPLAY DIVERGED at frame N").

Rows:
  linux       the reference build replaying its own recording; must be identical.
  linux-flip  the same recording with ONE byte flipped at a known frame; must
              report divergence at exactly that frame. Without this row a green
              table proves nothing - a replay harness that silently compares
              nothing looks identical on every platform.
  windows     build-win/melee.exe (tools/package_windows.sh) under Proton
              (tools/run_proton.sh).
  android /   SKIPPED here, no device and no macOS host; the invocation each
  macos       would use is printed so the row can be filled on real hardware.

Exit 0 when every platform that actually ran was identical (and the flip row
was caught), 1 otherwise. A platform that never reached the replay is reported
BLOCKED, never "identical" and never "diverged".
"""
import argparse
import os
import re
import shutil
import signal
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import net_test  # fifo_write(): one key line into a MELEE_KEY_FIFO

# src/pc/net_snapshot.c's on-disk format: "MRC1", u32 seed (host order), then
# one FrameRecord per frame = PADStatus pads[4] then u32 ck. PADStatus is 16
# bytes on TARGET_PC, not 12: extern/aurora/include/dolphin/pad.h:112-126 adds
# `u32 extButton` under TARGET_PC. Verified against real recordings (every
# recorded size is 8 + 68*n) and by compiling the real headers with the
# project's own flags from build/compile_commands.json.
REC_MAGIC = b"MRC1"
REC_HDR = 8
REC_STRIDE = 68
PAD_STRIDE = 16
CK_OFF = 64
FLIP_OFF = 2  # PADStatus.stickX of pad 0, inside the hashed pad block

# The stage debug VS loads (gmvsmode.c:169 asks for St_Kind_Last): the match
# reading its archive is the "the match actually started" signal. It MUST be
# the HIT line, not STORED: file_cache.cpp:445 prewarms GrNLa.dat into the
# cache at boot, so matching the bare name matches before the title screen and
# the whole run then records an idle title (checked: prewarm logs it at line
# 101 of the run log, the match at line 962).
MATCH_LOADED = re.compile(r"(?:LOOSE )?HIT: GrNLa\.dat")

# Held one line at a time by keyboard.c's fifo thread; X=A, Z=B, C=X, V=Y,
# arrows are the analog stick (keyboard.c:33-45). Never Return: that pauses the
# match and a paused match records nothing worth comparing.
MATCH_INPUT = ["Right 400", "X 120", "Left 400", "Z 150", "Up 200", "C 120",
               "Right+Up 250", "X 150", "Down 200", "V 120", "Left+Up 250",
               "X 150", "Right 300", "Tab 120"]


def win_path(p):
    """Linux absolute path as Wine sees it (Z: is the host root)."""
    return "Z:" + os.path.abspath(p).replace("/", "\\")


# ---- the recording ----------------------------------------------------------

def rec_read(path):
    with open(path, "rb") as f:
        return f.read()


def rec_frames(data):
    return (len(data) - REC_HDR) // REC_STRIDE


def rec_seed(data):
    return struct.unpack_from("<I", data, 4)[0]


def rec_ck(data, frame):
    return struct.unpack_from("<I", data, REC_HDR + frame * REC_STRIDE + CK_OFF)[0]


def rec_pad(data, frame, slot=0):
    """(button, stickX, stickY) of one slot, for the "is anyone playing" gate."""
    off = REC_HDR + frame * REC_STRIDE + slot * PAD_STRIDE
    button = struct.unpack_from("<H", data, off)[0]
    x, y = struct.unpack_from("<bb", data, off + 2)
    return button, x, y


def rec_digest(path):
    import hashlib
    return hashlib.sha256(rec_read(path)).hexdigest()[:16]


class Run:
    """One game instance, its log and its key FIFO."""

    def __init__(self, name, argv, work, env, log_path=None):
        self.name = name
        self.log_path = log_path or os.path.join(work, f"{name}.log")
        self.out = open(os.path.join(work, f"{name}.out"), "wb")
        e = dict(os.environ)
        e.update(env)
        e.pop("MELEE_LOG_FILE", None)
        e["MELEE_LOG_FILE"] = self.log_path
        self.argv = argv
        self.proc = subprocess.Popen(argv, env=e, stdout=self.out,
                                     stderr=subprocess.STDOUT, start_new_session=True)

    def text(self):
        out = b""
        for p in (self.log_path, self.out.name):
            try:
                with open(p, "rb") as f:
                    out += f.read()
            except FileNotFoundError:
                pass
        return out.decode("utf-8", "replace")

    def alive(self):
        return self.proc.poll() is None

    def kill(self):
        try:
            os.killpg(os.getpgid(self.proc.pid), signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            pass
        self.proc.wait()
        self.out.close()


def record(args):
    """The reference run. Returns (path, log text)."""
    work = args.work
    fifo = os.path.join(work, "record.keys")
    os.mkfifo(fifo)
    cache = os.path.join(work, "record.cache")
    os.makedirs(cache)
    path = os.path.join(work, "reference.rec")
    run = Run("record", [args.exe, "--no-card", args.disc], work, {
        "SDL_VIDEO_DRIVER": "x11", "MELEE_VSYNC": "0",
        "MELEE_SEED": str(args.seed), "MELEE_DEBUG_VS": "cpu",
        "MELEE_NET_PORT": str(args.port), "MELEE_CACHE_DIR": cache,
        "MELEE_KEY_FIFO": fifo, "MELEE_NET_RECORD": path,
        "MELEE_WINDOW_TITLE": "net-determinism-record",
    })
    print(f"determinism: recording {args.frames} frames, seed {args.seed}, pid {run.proc.pid}",
          flush=True)

    def frames():
        try:
            return max(0, (os.path.getsize(path) - REC_HDR) // REC_STRIDE)
        except OSError:
            return 0

    try:
        # Start in the opening movie, Start at the title -> debug VS. How many
        # frames that takes depends on boot timing, so press until the stage
        # archive shows up in the log rather than counting presses.
        # A cold MELEE_CACHE_DIR stores 894 archives before the title, which
        # on a loaded machine has been measured at ~150 s; every leg gets its
        # own cold cache so the legs stay comparable, so budget for it.
        deadline = time.time() + 300
        while not MATCH_LOADED.search(run.text()):
            if not run.alive():
                raise RuntimeError("the recording instance exited before the match started")
            if time.time() > deadline:
                raise RuntimeError("never reached the match (no 'HIT: GrNLa.dat' in the log)")
            net_test.fifo_write(fifo, "Return 150")
            time.sleep(2.0)
        start = frames()
        print(f"determinism: match entered around frame {start}", flush=True)
        # Then play until the recording is long enough. keyboard.c's fifo
        # thread keeps the FIFO open while it holds a line, so writing as fast
        # as the loop spins just fills the pipe (EAGAIN) and queues thousands
        # of stale lines; pace by the hold each line asks for.
        i = 0
        budget = time.time() + args.frames / 8.0 + 120
        while frames() < args.frames:
            if not run.alive():
                raise RuntimeError("the recording instance exited early")
            if time.time() > budget:
                raise RuntimeError(f"only reached frame {frames()} of {args.frames}")
            line = MATCH_INPUT[i % len(MATCH_INPUT)]
            net_test.fifo_write(fifo, line)
            time.sleep(int(line.split()[1]) / 1000.0 + 0.15)
            i += 1
    finally:
        text = run.text()
        run.kill()
    # A SIGKILL can land mid-fwrite; cut to whole records at the target length.
    with open(path, "r+b") as f:
        f.truncate(REC_HDR + args.frames * REC_STRIDE)
    data = rec_read(path)
    if data[:4] != REC_MAGIC:
        raise RuntimeError(f"recording magic is {data[:4]!r}, not {REC_MAGIC!r}")
    if rec_seed(data) != args.seed:
        raise RuntimeError(f"recording seed is {rec_seed(data)}, expected {args.seed}")
    if rec_frames(data) != args.frames:
        raise RuntimeError(f"recording is {rec_frames(data)} frames, expected {args.frames}")
    check_lively(data, start)
    return path, text


def check_lively(data, match_start):
    """A recording of an idle title screen would replay identically and prove
    nothing: src/pc/net_snapshot.c:117-139 only folds fighter state into the
    checksum while in_fight(). Insist on a match that moves and on inputs that
    were actually driven."""
    n = rec_frames(data)
    tail = range(max(match_start, n - 600), n)
    changed = sum(1 for f in tail if f > 0 and rec_ck(data, f) != rec_ck(data, f - 1))
    pressed = sum(1 for f in tail if rec_pad(data, f) != (0, 0, 0))
    print(f"determinism: last {len(tail)} frames: {changed} checksum changes, "
          f"{pressed} frames with input", flush=True)
    if changed < len(tail) // 2:
        raise RuntimeError(f"recording looks static ({changed}/{len(tail)} checksum changes): "
                           "the match never ran or was paused")
    if pressed < 20:
        raise RuntimeError(f"recording has almost no input ({pressed} frames): "
                           "MELEE_KEY_FIFO driving did not reach the match")


def corrupt(src, dst, frame):
    """One byte flipped inside the hashed pad block of `frame`."""
    data = bytearray(rec_read(src))
    off = REC_HDR + frame * REC_STRIDE + FLIP_OFF
    data[off] ^= 0xFF
    with open(dst, "wb") as f:
        f.write(data)
    return off


def compare_streams(ref, other):
    """The replay leg also records what it simulated, so the two per-frame
    checksum streams can be compared byte for byte instead of trusting one log
    line. Returns (note, hard_fault) where hard_fault is a harness problem,
    not a divergence."""
    a, b = rec_read(ref), rec_read(other)
    n = min(rec_frames(a), rec_frames(b))
    if n < 1:
        return "wrote no frames of its own", "the replay leg recorded nothing"
    pads = [f for f in range(n)
            if a[REC_HDR + f * REC_STRIDE:REC_HDR + f * REC_STRIDE + 4 * PAD_STRIDE] !=
               b[REC_HDR + f * REC_STRIDE:REC_HDR + f * REC_STRIDE + 4 * PAD_STRIDE]]
    if pads:
        # The pads it simulated must be the pads from the file, whatever the
        # checksums say; if they are not, the replay never fed anything and an
        # "identical" verdict would be meaningless.
        return (f"pads differ from the recording at {len(pads)} frames (first {pads[0]})",
                f"replay did not feed the recorded pads (first mismatch frame {pads[0]})")
    bad = [f for f in range(n) if rec_ck(a, f) != rec_ck(b, f)]
    if not bad:
        return f"checksum stream identical over all {n} recorded frames", None
    agree = [f for f in range(bad[0], n) if rec_ck(a, f) == rec_ck(b, f)]
    return (f"first differs at {bad[0]}, {len(bad)} of {n - bad[0]} later frames differ, "
            f"{len(agree)} ever re-agree"), None


# ---- replaying --------------------------------------------------------------

DIVERGED = re.compile(r"net: REPLAY DIVERGED at frame (\d+) \(recorded (\w+) now (\w+)\)")
FINISHED = re.compile(r"net: replay finished at frame (\d+)")
OPENED = re.compile(r"net: (replaying|cannot open) ")


class Result:
    def __init__(self, platform, frames=None, diverged=None, detail="", blocked=None):
        self.platform = platform
        self.frames = frames
        self.diverged = diverged
        self.detail = detail
        self.blocked = blocked
        self.stream = ""      # byte-compare of the two checksum streams
        self.fault = None     # harness fault, not a platform divergence

    def cross_check(self, ref, own):
        """Second, independent verdict: compare the checksum stream this leg
        recorded against the reference instead of trusting one log line."""
        if not os.path.exists(own):
            self.fault = f"{self.platform}: the replay leg wrote no recording"
            return
        self.stream, self.fault = compare_streams(ref, own)
        if self.fault:
            self.fault = f"{self.platform}: {self.fault}"
            return
        first = None if "identical" in self.stream else int(self.stream.split()[3].rstrip(","))
        if first != self.diverged:
            self.fault = (f"{self.platform}: the log says {self.diverged} but the recorded "
                          f"checksum streams first differ at {first}")

    @property
    def state(self):
        if self.blocked:
            return "SKIPPED" if self.blocked.startswith("SKIPPED") else "BLOCKED"
        if self.diverged is not None:
            return f"frame {self.diverged}"
        return "identical"


def watch(run, platform, frames, timeout):
    """Wait for src/pc/net_snapshot.c to finish or report the replay."""
    deadline = time.time() + timeout
    opened = False
    while time.time() < deadline:
        text = run.text()
        if not opened and OPENED.search(text):
            opened = True
            if "net: cannot open" in text:
                return Result(platform, blocked="the build could not open the recording")
        d = DIVERGED.search(text)
        if d:
            return Result(platform, frames=int(d.group(1)), diverged=int(d.group(1)),
                          detail=f"recorded {d.group(2)} now {d.group(3)}")
        f = FINISHED.search(text)
        if f:
            return Result(platform, frames=int(f.group(1)))
        if not run.alive():
            break
        time.sleep(0.5)
    text = run.text()
    if not opened:
        tail = " | ".join(text.strip().splitlines()[-3:])[:220]
        return Result(platform, blocked=f"never reached the replay (last log: {tail})")
    if not run.alive():
        return Result(platform, blocked=f"exited at code {run.proc.returncode} mid-replay")
    return Result(platform, blocked=f"still short of {frames} frames after {timeout:.0f} s")


def replay_linux(args, rec, platform, port):
    cache = os.path.join(args.work, f"{platform}.cache")
    os.makedirs(cache, exist_ok=True)
    own = os.path.join(args.work, f"{platform}.rec")
    run = Run(platform, [args.exe, "--no-card", args.disc], args.work, {
        "SDL_VIDEO_DRIVER": "x11", "MELEE_VSYNC": "0",
        "MELEE_SEED": str(args.seed), "MELEE_DEBUG_VS": "cpu",
        "MELEE_NET_PORT": str(port), "MELEE_CACHE_DIR": cache,
        "MELEE_NET_REPLAY": rec, "MELEE_NET_RECORD": own,
        "MELEE_WINDOW_TITLE": f"net-determinism-{platform}",
    })
    print(f"determinism: [{platform}] replaying, pid {run.proc.pid}", flush=True)
    try:
        r = watch(run, platform, args.frames, args.frames / 8.0 + 120)
    finally:
        run.kill()
    if not r.blocked:
        r.cross_check(rec, own)
    return r


def replay_windows(args, rec, port):
    """build-win/melee.exe under Proton. Wine canonicalises away the trailing
    space in this checkout's parent directory, so the disc is handed over as a
    symlink on a space-free path; the recording and cache go the same way."""
    if not os.path.exists(args.win_exe):
        return Result("windows", blocked=f"{args.win_exe} missing; run tools/package_windows.sh "
                                         "(see doc: needs --defsym at HEAD)")
    proton = os.path.join(HERE, "run_proton.sh")
    cache = os.path.join(args.work, "windows.cache")
    os.makedirs(cache, exist_ok=True)
    disc = os.path.join(args.work, "disc" + os.path.splitext(args.disc)[1])
    if not os.path.exists(disc):
        os.symlink(os.path.realpath(args.disc), disc)
    own = os.path.join(args.work, "windows.rec")
    run = Run("windows", [proton, "--no-card", win_path(disc)], args.work, {
        "MELEE_VSYNC": "0", "MELEE_SEED": str(args.seed), "MELEE_DEBUG_VS": "cpu",
        "MELEE_NET_PORT": str(port), "MELEE_CACHE_DIR": win_path(cache),
        "MELEE_NET_REPLAY": win_path(rec), "MELEE_NET_RECORD": win_path(own),
        "MELEE_WINDOW_TITLE": "net-determinism-windows",
        "STEAM_COMPAT_DATA_PATH": os.environ.get("STEAM_COMPAT_DATA_PATH",
                                                 "/tmp/proton_melee_test"),
    })
    print(f"determinism: [windows] replaying under Proton, pid {run.proc.pid}", flush=True)
    try:
        r = watch(run, "windows", args.frames, args.frames / 4.0 + 420)
    finally:
        run.kill()
        # Proton's exec'd wine child is not in our process group, and its argv
        # carries the Windows-style path, so match the basename.
        subprocess.run(["pkill", "-9", "-f", r"melee\.exe"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if not r.blocked:
        r.cross_check(rec, own)
    return r


# ---- platforms with no hardware here ----------------------------------------

def skipped(args, rec):
    """Real invocations, reported as SKIPPED with the missing prerequisite
    named. Neither is faked: no result is better than a made-up one."""
    apk = os.path.join(os.path.dirname(HERE), "dist", "Melee-Android-arm64.apk")
    rows = [
        ("android",
         "no Android device or emulator attached (adb devices is empty)",
         [f"adb install -r {apk}",
          f"adb push {rec} /sdcard/Android/data/com.melee.pc/files/reference.rec",
          "adb shell am start -n com.melee.pc/.MainActivity "
          f"--es args '--no-card /sdcard/melee.ciso' --ei seed {args.seed} "
          "--es env 'MELEE_NET_REPLAY=/sdcard/Android/data/com.melee.pc/files/reference.rec "
          f"MELEE_SEED={args.seed} MELEE_DEBUG_VS=cpu'",
          "adb logcat -s melee:V | grep -m1 'net: \\(REPLAY DIVERGED\\|replay finished\\)'"]),
        ("macos",
         "no macOS host and no osxcross toolchain; note CMakeLists.txt:197 does not "
         "apply src/pc/melee_state.ld on APPLE either, so the link fails as on Windows",
         ["cmake -B build-mac -G Ninja -DCMAKE_BUILD_TYPE=Release && ninja -C build-mac melee",
          f"MELEE_SEED={args.seed} MELEE_DEBUG_VS=cpu MELEE_NET_REPLAY={rec} "
          f"MELEE_CACHE_DIR=/tmp/det-mac ./build-mac/melee --no-card {args.disc}"]),
    ]
    out = []
    for platform, why, cmds in rows:
        print(f"determinism: [{platform}] SKIPPED: {why}", flush=True)
        for c in cmds:
            print(f"determinism: [{platform}]   would run: {c}", flush=True)
        out.append(Result(platform, blocked=f"SKIPPED: {why}"))
    return out


# ---- driver -----------------------------------------------------------------

def parse_args(argv=None):
    root = os.path.dirname(HERE)
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--frames", type=int, default=2400, help="frames in the recording")
    ap.add_argument("--seed", type=int, default=7, help="MELEE_SEED for the reference run")
    ap.add_argument("--exe", default=os.path.join(root, "build", "melee"))
    ap.add_argument("--win-exe", default=os.path.join(root, "build-win", "melee.exe"))
    ap.add_argument("--disc", default=os.path.join(root, "..", "melee.ciso"))
    ap.add_argument("--work", default="/tmp/net_determinism")
    ap.add_argument("--port", type=int, default=42400, help="MELEE_NET_PORT base (42400-42419)")
    ap.add_argument("--only", default="linux,linux-flip,windows",
                    help="comma-separated subset of the rows that can run here")
    ap.add_argument("--rec", help="skip recording and replay this file instead")
    ap.add_argument("--keep", action="store_true",
                    help="keep the work directory contents (implied by --rec)")
    return ap.parse_args(argv)


def main():
    args = parse_args()
    only = [x.strip() for x in args.only.split(",") if x.strip()]
    if not args.keep and not args.rec:
        # --rec usually points inside the work directory.
        shutil.rmtree(args.work, ignore_errors=True)
    os.makedirs(args.work, exist_ok=True)

    if args.rec:
        rec = args.rec
        data = rec_read(rec)
        args.frames = rec_frames(data)
        args.seed = rec_seed(data)
        print(f"determinism: reusing {rec} ({args.frames} frames, seed {args.seed})", flush=True)
    else:
        rec, _ = record(args)
    digest = rec_digest(rec)
    print(f"determinism: recording {rec} sha256:{digest} ({args.frames} frames)", flush=True)

    results = []
    if "linux" in only:
        results.append(replay_linux(args, rec, "linux", args.port + 1))

    flip_frame = args.frames // 2
    flip = None
    if "linux-flip" in only:
        bad = os.path.join(args.work, "corrupted.rec")
        off = corrupt(rec, bad, flip_frame)
        print(f"determinism: flipping byte {off} (frame {flip_frame}, pad 0 stickX)", flush=True)
        flip = replay_linux(args, bad, "linux-flip", args.port + 2)
        results.append(flip)
    if "windows" in only:
        results.append(replay_windows(args, rec, args.port + 3))
    results.extend(skipped(args, rec))

    print()
    print(f"{'platform':<12} {'frames':>7}  {'first diverging frame':<22} recording")
    for r in results:
        frames = str(r.frames) if r.frames is not None else "-"
        print(f"{r.platform:<12} {frames:>7}  {r.state:<22} sha256:{digest}")
    for r in results:
        if r.detail:
            print(f"  {r.platform}: {r.detail}")
        if r.stream:
            print(f"  {r.platform}: recorded stream: {r.stream}")
        if r.blocked:
            print(f"  {r.platform}: {r.blocked}")

    fails = []
    for r in results:
        if r.fault:
            fails.append("harness fault: " + r.fault)
        if r.platform == "linux-flip":
            continue
        if r.blocked:
            # A platform with no hardware here is a SKIP, not a failure; the
            # reference platform failing to run means the harness proved nothing.
            if r.platform == "linux":
                fails.append(f"linux never ran: {r.blocked}")
            continue
        if r.diverged is not None:
            fails.append(f"{r.platform} diverged at frame {r.diverged}")
        elif r.frames != args.frames:
            fails.append(f"{r.platform} replayed {r.frames} of {args.frames} frames")
    if flip is not None:
        if flip.diverged != flip_frame:
            fails.append(f"sensitivity check FAILED: byte flipped at frame {flip_frame}, "
                         f"harness reported {flip.state} - a green table means nothing")
        else:
            print(f"\nsensitivity check: byte flip at frame {flip_frame} caught at frame "
                  f"{flip.diverged} ({flip.detail})")
    print()
    for f in fails:
        print(f"determinism: FAIL: {f}")
    print("determinism: " + ("PASS" if not fails else "FAIL"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
