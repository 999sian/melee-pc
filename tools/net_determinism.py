#!/usr/bin/env python3
"""Milestone M0 (docs/netcode-plan.md section 5 and 12): one recorded run must
replay bit-identical on every platform we ship.

    tools/net_determinism.py [--frames 2400] [--seed 7] [--exe build/melee]
                             [--win-exe build-win/melee.exe] [--disc ../melee.ciso]
                             [--work /tmp/net_determinism] [--port 42400]
                             [--only linux,linux-flip,windows] [--keep] [--state-log]

One reference recording is made on this Linux build with a fixed MELEE_SEED, a
fixed frame count and input driven through MELEE_KEY_FIFO (MELEE_DEBUG_VS=cpu,
so Link is keyboard-driven and Mario is a level-4 CPU: a real match, not an
idle one). That single file is then fed back with MELEE_NET_REPLAY to every
platform we can reach here, and src/pc/net_snapshot.c reports the first frame
whose checksum differs ("net: REPLAY DIVERGED at frame N").

Rows:
  linux-record  the reference build replaying the RECORDING leg's file. This
              row is about the recording, not about a platform: the recording
              leg runs with the 1 kHz keyboard poller live (MELEE_KEY_FIFO)
              and is paced differently from a replay, and the simulation is
              sensitive to that pacing, so this row currently diverges at the
              first frame of an input transition. Its own recording - written
              while being fed the recorded pads - is the canonical file every
              row below is judged against, so one defect cannot masquerade as
              the other.
  linux       the reference build replaying the canonical file; must be
              identical.
  linux-flip  the same recording with ONE byte flipped at a known frame; must
              report divergence at exactly that frame. Without this row a green
              table proves nothing - a replay harness that silently compares
              nothing looks identical on every platform.
  windows     build-win/melee.exe (tools/package_windows.sh) under Proton
              (tools/run_proton.sh).
  android     the APK from dist/ on a device over adb (not in the default
              --only: it installs and launches on whatever is attached).
              SKIPPED only when nothing is attached; with a device present
              every way it can fail - no env file, an unreadable disc, a
              crash - is a BLOCKED row naming the logcat line that said so.
  macos       SKIPPED here, no macOS host; the invocation it would use is
              printed so the row can be filled on real hardware.

Exit 0 when every platform that actually ran was identical (and the flip row
was caught), 1 otherwise. A platform that never reached the replay is reported
BLOCKED, never "identical" and never "diverged".

--state-log points MELEE_NET_STATE_LOG at <work>/<leg>.state, so every leg
writes "net: state f<N> ..." plus "net: bits f<N> ..." for each frame
(src/pc/net_snapshot.c). That is how a divergence is localised from "the
checksums differ at frame N" to "this field of this fighter differs": run it,
then diff two legs' .state files (or the recording leg's against a replay's)
over the frames around N. The bits line carries the raw float bits of exactly
the fields frame_checksum() folds, because the readable line rounds to three
decimals and hides a 1-ULP difference. Off by default.
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

# src/pc/net_snapshot.c's on-disk format: "MRC2", u32 seed (host order), then
# one FrameRecord per frame = PADStatus pads[4], u32 ck, u32 tick-start seed.
# PADStatus is 16 bytes on TARGET_PC, not 12: dolphin/pad.h adds extButton.
# tools/test_net_replay_seed.py compiles the real header and recording code
# and checks the 8 + 72*n layout and checksum/seed offsets independently.
REC_MAGIC = b"MRC2"
REC_HDR = 8
REC_STRIDE = 72
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
        **state_log(args, "record"),
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


def watch(run, platform, frames, timeout, run_to_end=False):
    """Wait for src/pc/net_snapshot.c to finish or report the replay.
    run_to_end keeps waiting after a divergence is reported, so the leg's own
    recording still covers every frame instead of stopping at the mismatch."""
    deadline = time.time() + timeout
    opened = False
    div = None
    while time.time() < deadline:
        text = run.text()
        if not opened and OPENED.search(text):
            opened = True
            if "net: cannot open" in text:
                return Result(platform, blocked="the build could not open the recording")
        d = DIVERGED.search(text)
        if d and div is None:
            div = Result(platform, frames=int(d.group(1)), diverged=int(d.group(1)),
                         detail=f"recorded {d.group(2)} now {d.group(3)}")
            if not run_to_end:
                return div
        f = FINISHED.search(text)
        if f:
            return div if div is not None else Result(platform, frames=int(f.group(1)))
        if not run.alive():
            break
        time.sleep(0.5)
    text = run.text()
    if not opened:
        tail = " | ".join(text.strip().splitlines()[-3:])[:220]
        return Result(platform, blocked=f"never reached the replay (last log: {tail})")
    if div is not None:
        return div
    if not run.alive():
        return Result(platform, blocked=f"exited at code {run.proc.returncode} mid-replay")
    return Result(platform, blocked=f"still short of {frames} frames after {timeout:.0f} s")


def state_log(args, name, wine=False):
    """--state-log: one file per leg, including the recording one, so a replay
    can be diffed against the run it came from as well as against another
    platform. src/pc/net_snapshot.c writes it block-buffered; it deliberately
    does not go through the pc_log_line path, which flushes twice per line and
    perturbed the frame pacing enough to move the divergence it was measuring."""
    if not args.state_log:
        return {}
    path = os.path.join(args.work, f"{name}.state")
    return {"MELEE_NET_STATE_LOG": win_path(path) if wine else path}


def replay_linux(args, rec, platform, port, run_to_end=False):
    cache = os.path.join(args.work, f"{platform}.cache")
    os.makedirs(cache, exist_ok=True)
    own = os.path.join(args.work, f"{platform}.rec")
    run = Run(platform, [args.exe, "--no-card", args.disc], args.work, {
        "SDL_VIDEO_DRIVER": "x11", "MELEE_VSYNC": "0",
        "MELEE_SEED": str(args.seed), "MELEE_DEBUG_VS": "cpu",
        "MELEE_NET_PORT": str(port), "MELEE_CACHE_DIR": cache,
        "MELEE_NET_REPLAY": rec, "MELEE_NET_RECORD": own,
        "MELEE_WINDOW_TITLE": f"net-determinism-{platform}",
        **state_log(args, platform),
    })
    print(f"determinism: [{platform}] replaying, pid {run.proc.pid}", flush=True)
    try:
        r = watch(run, platform, args.frames, args.frames / 8.0 + 120, run_to_end)
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
        return Result("windows", blocked=f"{args.win_exe} missing; build it with "
                                         "tools/package_windows.sh or cmake/ninja into build-win")
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
        **state_log(args, "windows", wine=True),
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


# ---- android ----------------------------------------------------------------
#
# src/pc/main.c:391 hardcodes the one path the app reads its knobs from, so
# melee-env.txt has to be exactly there; everything else lives beside it
# because that is the only directory the app can both read and write.
ANDROID_PKG = "dev.melee.game"
ANDROID_ACT = "dev.melee.MeleeActivity"
ANDROID_DIR = f"/sdcard/Android/data/{ANDROID_PKG}/files"
# adb push must never CREATE a directory under /sdcard/Android/data: the ones
# it makes are shell:ext_data_rw 0770 and the app then gets EACCES on every
# file in its own directory, the disc included, which reads as a startup crash
# (docs/building.md:179-186). Stage here and copy across on the device.
ANDROID_STAGE = "/sdcard/Download"
# logcat is the only channel: pc_log_line goes there through main.c:156 and
# the env bootstrap reports itself there directly (main.c:402) because it runs
# before any log sink exists. Aurora's own level is LOG_DEBUG on Android
# (main.c:521), so take the tags we read and silence the rest.
ANDROID_TAGS = ["melee:V", "Melee:V", "OSReport:I", "AndroidRuntime:E", "DEBUG:V",
                "libc:F", "*:S"]

ENV_MISSING = re.compile(r"env: no (/sdcard/\S+) \(errno (\d+)\)")
DISC_FAIL = re.compile(r"disc: (?:cannot open|nod_disc_open|\S+ opened but nod rejected)[^\n]*")
CRASHED = re.compile(r"(?:Fatal signal \d+[^\n]*|FATAL signal \d+[^\n]*|FATAL EXCEPTION[^\n]*)")


def adb(*cmd, timeout=180, check=True):
    p = subprocess.run(["adb", *cmd], capture_output=True, text=True, timeout=timeout)
    if check and p.returncode != 0:
        raise RuntimeError(f"`adb {' '.join(cmd)}` failed ({p.returncode}): "
                           f"{(p.stderr or p.stdout).strip()[:300]}")
    return p.stdout


def adb_devices():
    """None when adb is missing at all, so "no tool" and "no device" stay
    different answers."""
    if shutil.which("adb") is None:
        return None
    out = subprocess.run(["adb", "devices"], capture_output=True, text=True).stdout
    return [f[0] for f in (line.split() for line in out.splitlines()[1:])
            if len(f) == 2 and f[1] == "device"]


def adb_exists(path):
    return adb("shell", f"test -e '{path}' && echo yes || echo no").strip() == "yes"


def adb_put(local, remote, timeout=600):
    stage = f"{ANDROID_STAGE}/{os.path.basename(remote)}"
    adb("push", local, stage, timeout=timeout)
    adb("shell", "cp", stage, remote, timeout=timeout)
    adb("shell", "chmod", "666", remote)


def android_prepare(args):
    """Install the APK, make a directory the app can actually use, and put the
    disc in it. Returns the on-device disc path."""
    apk = args.android_apk
    if not os.path.exists(apk):
        print(f"determinism: [android] {apk} missing, running tools/build_android.sh",
              flush=True)
        subprocess.run(["bash", os.path.join(HERE, "build_android.sh")],
                       cwd=os.path.dirname(HERE), check=True)
        if not os.path.exists(apk):
            raise RuntimeError(f"tools/build_android.sh produced no {apk}")
    print(f"determinism: [android] installing {apk}", flush=True)
    out = adb("install", "-r", apk, timeout=900)
    if "Success" not in out:
        raise RuntimeError(f"adb install said: {out.strip()[:200]}")
    # Nothing on the app side ever calls getExternalFilesDir(), so this
    # directory does not exist until something makes it and adb is the only
    # thing that can. 2777 is the whole fix, setgid bit included: without it
    # the app's own uid owns the state log it writes, group and all, and
    # `adb pull` then fails with EACCES; with it new files land in
    # ext_data_rw, which shell is a member of.
    adb("shell", "mkdir", "-p", ANDROID_DIR)
    adb("shell", "chmod", "2777", os.path.dirname(ANDROID_DIR), ANDROID_DIR)
    mode = adb("shell", "ls", "-ld", ANDROID_DIR).split()[0]
    if not mode.startswith("drwxrwsrwx"):
        raise RuntimeError(f"{ANDROID_DIR} is {mode}, not drwxrwsrwx: the app would get "
                           "EACCES on its own files (docs/building.md:179)")
    disc = f"{ANDROID_DIR}/melee.ciso"
    if not adb_exists(disc):
        # A plain path, not the file picker's content:// URI: the app has no
        # storage permission on Android 13+ (MeleeActivity.java:58), so this
        # directory is the only place a path it can fopen() can live.
        src = f"{ANDROID_STAGE}/melee.ciso"
        if not adb_exists(src):
            print(f"determinism: [android] pushing {args.disc} (this takes a while)", flush=True)
            adb("push", os.path.realpath(args.disc), src, timeout=3600)
        adb("shell", "cp", src, disc, timeout=3600)
        adb("shell", "chmod", "666", disc)
    return disc


def android_watch(log, timeout):
    """Every way this row can fail silently gets its own verdict: no env file
    means the knobs never reached the app, a disc it cannot open leaves it
    sitting in the launcher, and a native crash just stops the log."""
    deadline = time.time() + timeout
    div = None
    opened = False
    text = ""
    i = 0
    while time.time() < deadline:
        try:
            with open(log, "rb") as f:
                text = f.read().decode("utf-8", "replace")
        except FileNotFoundError:
            text = ""
        m = ENV_MISSING.search(text)
        if m:
            return Result("android", blocked=(
                f"melee-env.txt was not applied ({m.group(0)}): errno 13 means the directory "
                "is adb-owned 0770 (docs/building.md:179), errno 2 means the push never "
                "landed - either way none of the MELEE_* knobs reached the app"))
        d = DISC_FAIL.search(text)
        if d:
            return Result("android", blocked=f"the disc could not be opened: {d.group(0)}")
        c = CRASHED.search(text)
        if c:
            return Result("android", blocked=f"the app crashed: {c.group(0)[:200]}")
        if not opened and OPENED.search(text):
            opened = True
            if "net: cannot open" in text:
                return Result("android", blocked="the app could not open the recording it was "
                                                 "pointed at (check the pushed .rec)")
        dv = DIVERGED.search(text)
        if dv and div is None:
            div = Result("android", frames=int(dv.group(1)), diverged=int(dv.group(1)),
                         detail=f"recorded {dv.group(2)} now {dv.group(3)}")
        fin = FINISHED.search(text)
        if fin:
            # The state log's tail only reaches the card on net_snapshot.c's
            # every-8-frames flush, and the force-stop below is a SIGKILL.
            time.sleep(2.0)
            return div if div is not None else Result("android", frames=int(fin.group(1)))
        i += 1
        if i % 6 == 0 and not adb("shell", "pidof", ANDROID_PKG, check=False).strip():
            tail = " | ".join(text.strip().splitlines()[-3:])[:300]
            return div or Result("android", blocked=f"the app is gone (last log: {tail})")
        time.sleep(0.5)
    tail = " | ".join(text.strip().splitlines()[-3:])[:300]
    if div is not None:
        return div
    return Result("android", blocked=(f"never {'finished' if opened else 'reached'} the replay "
                                      f"after {timeout:.0f} s (last log: {tail})"))


def replay_android(args, rec):
    devs = adb_devices()
    if devs is None:
        return Result("android", blocked="SKIPPED: adb is not on PATH")
    if not devs:
        return Result("android", blocked="SKIPPED: no device attached (adb devices is empty)")
    if len(devs) > 1 and not os.environ.get("ANDROID_SERIAL"):
        return Result("android", blocked=f"{len(devs)} devices attached ({', '.join(devs)}); "
                                         "set ANDROID_SERIAL to pick one")
    own = os.path.join(args.work, "android.rec")
    state = os.path.join(args.work, "android.state")
    rec_dev, own_dev = f"{ANDROID_DIR}/reference.rec", f"{ANDROID_DIR}/android.rec"
    state_dev = f"{ANDROID_DIR}/android.state"
    fed = rec
    try:
        disc = android_prepare(args)
        adb("shell", "am", "force-stop", ANDROID_PKG)
        adb("shell", "rm", "-f", own_dev, state_dev)
        if args.android_flip is not None:
            fed = os.path.join(args.work, "android-flip.rec")
            off = corrupt(rec, fed, args.android_flip)
            print(f"determinism: [android] INJECTION: byte {off} flipped (frame "
                  f"{args.android_flip}, pad 0 stickX); the row must report that frame",
                  flush=True)
        adb_put(fed, rec_dev)
        env = {"MELEE_SEED": str(args.seed), "MELEE_DEBUG_VS": "cpu", "MELEE_VSYNC": "0",
               "MELEE_NET_REPLAY": rec_dev, "MELEE_NET_RECORD": own_dev,
               # net.c:1927 only honours this while netplay is active, so it
               # does nothing for a solo replay and the force-stop below is
               # what ends the run; written anyway so the device carries the
               # same knobs as every other leg.
               "MELEE_NET_EXIT_AFTER_FRAMES": str(args.frames)}
        if args.state_log:
            env["MELEE_NET_STATE_LOG"] = state_dev
        envfile = os.path.join(args.work, "melee-env.txt")
        with open(envfile, "w") as f:
            f.write("".join(f"{k}={v}\n" for k, v in env.items()))
        adb_put(envfile, f"{ANDROID_DIR}/melee-env.txt")
    except (RuntimeError, OSError, subprocess.SubprocessError) as e:
        return Result("android", blocked=f"device setup failed: {e}")

    log = os.path.join(args.work, "android.log")
    out = open(log, "wb")
    adb("logcat", "-c")
    tail = subprocess.Popen(["adb", "logcat", "-v", "time", *ANDROID_TAGS],
                            stdout=out, stderr=subprocess.STDOUT)
    try:
        adb("shell", "am", "start", "-W", "-n", f"{ANDROID_PKG}/{ANDROID_ACT}",
            "--esa", "args", f"--no-card,{disc}")
        print(f"determinism: [android] {devs[0]} replaying {args.frames} frames", flush=True)
        # A phone boots the disc slower than this machine does, and the first
        # run after an install compiles every pipeline from cold.
        r = android_watch(log, args.frames / 4.0 + 900)
    except (RuntimeError, subprocess.SubprocessError) as e:
        r = Result("android", blocked=f"launch failed: {e}")
    finally:
        tail.terminate()
        tail.wait()
        out.close()
        subprocess.run(["adb", "shell", "am", "force-stop", ANDROID_PKG],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if r.blocked:
        return r
    for remote, local in [(own_dev, own)] + ([(state_dev, state)] if args.state_log else []):
        p = subprocess.run(["adb", "pull", remote, local], capture_output=True, text=True)
        if p.returncode != 0:
            r.fault = f"android: could not pull {remote}: {(p.stderr or p.stdout).strip()[:200]}"
            return r
    print(f"determinism: [android] pulled {own}"
          f"{' and ' + state if args.state_log else ''}", flush=True)
    r.cross_check(fed, own)
    return r


# ---- platforms with no hardware here ----------------------------------------

def skipped(args, rec):
    """A real invocation, reported as SKIPPED with the missing prerequisite
    named. It is not faked: no result is better than a made-up one."""
    why = ("no macOS host and no osxcross toolchain; note CMakeLists.txt:197 does not "
           "apply src/pc/melee_state.ld on APPLE either, so the link fails as on Windows")
    cmds = ["cmake -B build-mac -G Ninja -DCMAKE_BUILD_TYPE=Release && ninja -C build-mac melee",
            f"MELEE_SEED={args.seed} MELEE_DEBUG_VS=cpu MELEE_NET_REPLAY={rec} "
            f"MELEE_CACHE_DIR=/tmp/det-mac ./build-mac/melee --no-card {args.disc}"]
    print(f"determinism: [macos] SKIPPED: {why}", flush=True)
    for c in cmds:
        print(f"determinism: [macos]   would run: {c}", flush=True)
    return [Result("macos", blocked=f"SKIPPED: {why}")]


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
    ap.add_argument("--state-log", action="store_true",
                    help="MELEE_NET_STATE_LOG=1 on the replay legs: per-frame state and "
                         "exact-bits lines, so a divergence can be diffed field by field")
    ap.add_argument("--android-apk",
                    default=os.path.join(root, "dist", "Melee-Android-arm64.apk"),
                    help="APK the android row installs; built with tools/build_android.sh "
                         "if it is missing")
    ap.add_argument("--android-flip", type=int, metavar="FRAME",
                    help="injection check for the android row: flip one byte of the file the "
                         "device replays at FRAME; the row must report exactly FRAME")
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
    # The recording leg drives input through MELEE_KEY_FIFO, so its 1 kHz
    # keyboard poller is live and its frames are paced differently from a
    # replay's, and the simulation turns out to be sensitive to that pacing
    # (local/WindowsM0-doc.md: the same build replaying the same file diverges
    # from the recording at the first frame of an input transition, and even
    # turning --state-log on moves that frame). So the recording's PAD stream
    # is the input truth, but its own checksum column is not a sound reference
    # for a platform comparison. One Linux replay canonicalises it: its own
    # recording, written while being fed those pads, is the file every
    # platform row is then judged against. The record-vs-replay difference is
    # reported as its own row, "linux-record", so the pacing defect stays
    # visible instead of masquerading as a platform divergence.
    canon = rec
    if only:
        pacing = replay_linux(args, rec, "linux-record", args.port + 4, run_to_end=True)
        results.append(pacing)
        own = os.path.join(args.work, "linux-record.rec")
        if pacing.blocked or not os.path.exists(own):
            print("determinism: canonicalisation leg did not run; rows below are judged "
                  "against the recording leg's own checksums", flush=True)
        else:
            # The replay keeps recording after the last record is consumed, so
            # cut it back to the reference length.
            with open(own, "r+b") as f:
                f.truncate(REC_HDR + args.frames * REC_STRIDE)
            if rec_frames(rec_read(own)) == args.frames:
                canon = own
                print(f"determinism: canonical reference {canon} "
                      f"sha256:{rec_digest(canon)} ({args.frames} frames)", flush=True)
            else:
                print(f"determinism: canonicalisation leg only reached "
                      f"{rec_frames(rec_read(own))} frames; keeping the recording leg's file",
                      flush=True)
    digest = rec_digest(canon)

    if "linux" in only:
        results.append(replay_linux(args, canon, "linux", args.port + 1))

    flip_frame = args.frames // 2
    flip = None
    if "linux-flip" in only:
        bad = os.path.join(args.work, "corrupted.rec")
        off = corrupt(canon, bad, flip_frame)
        print(f"determinism: flipping byte {off} (frame {flip_frame}, pad 0 stickX)", flush=True)
        flip = replay_linux(args, bad, "linux-flip", args.port + 2)
        results.append(flip)
    if "windows" in only:
        results.append(replay_windows(args, canon, args.port + 3))
    if "android" in only:
        results.append(replay_android(args, canon))
    results.extend(skipped(args, canon))

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
        if r.platform == "android" and args.android_flip is not None:
            # --android-flip is the device's own sensitivity check: that row is
            # judged against the planted frame below, not against "identical".
            continue
        if r.blocked:
            # A platform with no hardware here is a SKIP, not a failure; the
            # reference platform failing to run means the harness proved
            # nothing, and a device that IS attached and still produced no
            # result is a failure too - a row that quietly says nothing on
            # real hardware is exactly what the android stub used to do.
            if r.platform == "linux":
                fails.append(f"linux never ran: {r.blocked}")
            elif not r.blocked.startswith("SKIPPED"):
                fails.append(f"{r.platform} never produced a result: {r.blocked}")
            continue
        if r.diverged is not None:
            if r.platform == "linux-record":
                fails.append(f"the recording leg's own run is not reproducible: replaying it "
                             f"diverges at frame {r.diverged} (frame pacing, not a platform "
                             f"difference; the rows below use the canonical file)")
            else:
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
    android = next((r for r in results if r.platform == "android"), None)
    if args.android_flip is not None and android is not None and not android.blocked:
        if android.diverged != args.android_flip:
            fails.append(f"android injection check FAILED: byte flipped at frame "
                         f"{args.android_flip}, the device reported {android.state} - the row "
                         "cannot be trusted to see a real divergence")
        else:
            print(f"\nandroid injection check: byte flip at frame {args.android_flip} caught "
                  f"at frame {android.diverged} ({android.detail})")
    print()
    for f in fails:
        print(f"determinism: FAIL: {f}")
    print("determinism: " + ("PASS" if not fails else "FAIL"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
