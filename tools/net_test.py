#!/usr/bin/env python3
"""Netplay integration test: two instances on one machine, a match, then asserts.

    tools/net_test.py [--loss PCT] [--delay MS] [--rxdelay MS] [--jitter] [--reorder]
                      [--burst] [--dup] [--minutes N] [--lan] [--scenes] [--oom FRAME]
                      [--disconnect] [--fuzz] [--exe build/melee]
                      [--disc ../melee.ciso] [--port 42050] [--work /tmp/net_test]

Direct mode (default): MELEE_NET=127.0.0.1:<peer> + MELEE_DEBUG_VS=1, Start
pressed twice through MELEE_KEY_FIFO (title, then straight into Link vs Mario).
--lan: both instances walk the real menus into the LAN lobby (VS Mode -> ONLINE
-> LAN PLAY), Start on one elects the host, then CSS/SSS by fixed-hold cursor
moves; needs the shared LAN free (coordinate with anyone else in the lobby).
--scenes: --lan plus the rest of the set - match, out of it by L+R+A+Start,
results, CSS, SSS, rematch - and the cross-log assertions in check_scenes()
(same CSS frame and seed, identical SSS picks twice, identical barrier at every
shared stats frame). --oom FRAME: MELEE_NET_SIM_OOM_FRAME on A, so its first
snapshot at/after FRAME fails like a realloc would; check_oom() asserts the
session drops to lockstep and finishes anyway. --disconnect: B SIGKILLed
mid-match (no BYE), asserting A times the peer out within the documented 7 s
and keeps its frame loop running. --fuzz: tools/net_fuzz.py hammers instance
A's port during the match.

Each instance runs until MELEE_NET_EXIT_AFTER_FRAMES (net.c) prints
"net: test done", or the test times out. Pass = both done, exit 0, no DESYNC,
no "peer silent", no rollback lost. Exit code 0 on pass, 1 on fail.
Logs: <work>/a.log, <work>/b.log. tools/net_acceptance.py imports parse_args()
and run() to sweep the link-simulator matrix.
"""
import argparse
import errno
import os
import re
import shutil
import signal
import subprocess
import sys
import threading
import time

# net.frame counts every frame the loop runs, boot included, and a cold
# MELEE_CACHE_DIR spends ~150 s storing the disc's archives before the title
# appears (measured on this machine at load average 20, ~50 frames/s through
# boot). The exit frame is fixed at launch, so the budget covers the worst
# boot seen and check_match() fails a row whose match did not get its minutes.
BOOT_FRAMES = 9000
CACHE_ROOT = "/tmp/melee_net_cache"  # kept between runs; keyed by port, never shared
SIM_ENV = {  # CLI flag -> (env knob, value) in src/pc/net.c's link simulator
    "jitter": ("MELEE_NET_SIM_JITTER_MS", "20"),
    "reorder": ("MELEE_NET_SIM_REORDER", "10"),
    "burst": ("MELEE_NET_SIM_BURST", "5"),
    "dup": ("MELEE_NET_SIM_DUP", "10"),
}

# The only per-scene marker the game emits: the PC file layer prints a line per
# disc read and every scene loads its own archive (mnmain.c, mncharsel.c:5472,
# mnstagesel.c:569, gmvsmode.c:169 asks for St_Kind_Last = GrNLa). It must be
# the HIT line: file_cache.cpp:432-486 prewarms GrNLa/GrSt/GrPs/GrOp/GrYs by
# name at boot, so a STORED line says nothing about the scene. LOOSE HIT is the
# same read served from an extracted files/ dir (file_cache.cpp:325). These
# lines carry no timestamp, so they are matched by presence and count.
SCENE_FILE = {
    "title": r"\[FileCache\] (?:LOOSE )?HIT: GmTtAll\.",
    "menu": r"\[FileCache\] (?:LOOSE )?HIT: MnMaAll\.",
    "css": r"\[FileCache\] (?:LOOSE )?HIT: MnSlChr\.",
    "sss": r"\[FileCache\] (?:LOOSE )?HIT: MnSlMap\.",
    "match": r"\[FileCache\] (?:LOOSE )?HIT: Gr[A-Za-z0-9]+\.(?:dat|usd)",
}


def fifo_write(path, line, tries=100):
    """One key line to a MELEE_KEY_FIFO; the game reopens it after every writer."""
    for _ in range(tries):
        try:
            fd = os.open(path, os.O_WRONLY | os.O_NONBLOCK)
            break
        except OSError as e:
            if e.errno != errno.ENXIO:
                raise
            time.sleep(0.1)  # reader thread not up yet
    else:
        raise RuntimeError(f"no reader on {path}")
    with os.fdopen(fd, "w") as f:
        f.write(line + "\n")


class Instance:
    def __init__(self, name, exe, disc, work, port, peer_port, env, lan):
        self.name = name
        self.port = port
        self.log_path = os.path.join(work, f"{name}.log")
        self.fifo = os.path.join(work, f"{name}.keys")
        os.mkfifo(self.fifo)
        self.rec_path = os.path.join(work, f"{name}.rec")
        cache = os.path.join(CACHE_ROOT, str(port))
        os.makedirs(cache, exist_ok=True)
        e = dict(os.environ)
        e.update({
            "SDL_VIDEO_DRIVER": "x11",
            "MELEE_VSYNC": "0",
            "MELEE_NET_PORT": str(port),
            "MELEE_CACHE_DIR": cache,
            "MELEE_WINDOW_TITLE": f"melee-net-{name}",
            "MELEE_KEY_FIFO": self.fifo,
            "MELEE_SEED": "7",
            "MELEE_FPS": "1",  # one line per second: the rate the drive scales its holds by
            "MELEE_NET_RECORD": self.rec_path,  # per-frame pads + checksum, for check_match()
        })
        if not lan:
            e["MELEE_NET"] = f"127.0.0.1:{peer_port}"
            e["MELEE_NET_PLAYER"] = "0" if name == "a" else "1"
            e["MELEE_DEBUG_VS"] = "1"
        e.update(env)
        e.pop("MELEE_LOG_FILE", None)  # stderr is captured below; avoid double lines
        self.log = open(self.log_path, "wb")
        self.proc = subprocess.Popen([exe, "--no-card", disc], env=e, stdout=self.log,
                                     stderr=subprocess.STDOUT)

    def key(self, line):
        fifo_write(self.fifo, line)

    def text(self):
        with open(self.log_path, "rb") as f:
            return f.read().decode("utf-8", "replace")

    def wait_log(self, pattern, timeout):
        """True once `pattern` (regex) appears in the log; False on timeout/exit."""
        deadline = time.time() + timeout
        rx = re.compile(pattern)
        while time.time() < deadline:
            if rx.search(self.text()):
                return True
            if self.proc.poll() is not None:
                return False
            time.sleep(0.25)
        return False

    def kill(self, grace=0.0):
        """SIGKILL, after `grace` seconds for a self-exit (aurora's GPU teardown
         takes a few seconds, and a killed instance reports exit code -9)."""
        deadline = time.time() + grace
        while self.proc.poll() is None and time.time() < deadline:
            time.sleep(0.25)
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGKILL)
            self.proc.wait()
        self.log.close()


def count(inst, pattern):
    return len(re.findall(pattern, inst.text()))


def frame_ms(inst):
    """Wall ms per simulated frame, from MELEE_FPS's per-second line (vi.c:97-128).
    Key holds are wall-clock but the cursor moves per frame, so a machine at
    20 fps moves it a third as far as the 60 fps the holds were written for:
    every hold below is given in frames and scaled through here."""
    fps = [float(x) for x in re.findall(r"^fps ([\d.]+) ", inst.text(), re.M)]
    recent = [x for x in fps[-3:] if x > 0]
    return 1000.0 / max(5.0, sum(recent) / len(recent)) if recent else 1000.0 / 60


def press(insts, key, frames, settle=0.6):
    """Hold `key` for `frames` simulated frames on each instance, then wait out
    the hold, the fifo thread's own 100 ms gap and `settle` seconds."""
    worst = 0.0
    for inst in insts:
        ms = frames * frame_ms(inst)
        worst = max(worst, ms)
        inst.key(f"{key} {int(ms)}")
    time.sleep(worst / 1000 + 0.1 + settle)


def press_until(insts, key, frames, pattern, want=1, tries=15, each=6.0, on=None):
    """Press until `pattern` has appeared `want` times in every instance's log.
    Fixed schedules are what left the old drive at the title screen for entire
    runs: how many frames a load takes depends on the machine's load, so every
    step waits for the scene's own archive instead. `on` narrows who is pressed
    - once the session is up both peers simulate both pads, so one is enough,
    and a stray Start inside a match would pause it."""
    def there():
        return all(count(i, pattern) >= want for i in insts)

    for _ in range(tries):
        if there():
            return True
        press(on or insts, key, frames, 0)
        deadline = time.time() + each
        while time.time() < deadline:
            if there():
                return True
            if any(i.proc.poll() is not None for i in insts):
                return False
            time.sleep(0.25)
    return there()


# Both players keep moving for as long as a match runs. Without this the matrix
# measures an idle link: an untouched Melee match has a *constant* frame
# checksum (frame_checksum folds position, motion id, percent, stocks and the
# RNG seed, and all of those stand still when two fighters stand still), and
# "repeat the last input" is only ever a wrong prediction when the input
# changes - so an idle match rolls back exactly zero times however bad the
# link is. Start is deliberately absent: it would pause the match.
WORKOUT = [("Right", 240), ("X", 100), ("Left", 240), ("C", 100), ("Right+X", 170),
           ("Z", 100), ("Up", 140), ("Down", 140)]


class Workout:
    """Cycles WORKOUT through both key fifos until done(). Holds are wall-clock
    (the exact frame count does not matter here, only that the input keeps
    changing) so the thread never has to read the logs."""

    def __init__(self, insts):
        self.insts = insts
        self.n = 0
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _run(self):
        i = 0
        while not self._stop.is_set():
            key, ms = WORKOUT[i % len(WORKOUT)]
            i += 1
            for inst in self.insts:
                if inst.proc.poll() is None:
                    try:
                        inst.key(f"{key} {ms}")
                    except Exception:
                        return  # the run is being torn down
            self.n += 1
            self._stop.wait(ms / 1000 + 0.15)

    def done(self):
        """Stop and wait for the last line to be written, so a drive step that
        follows has the fifo to itself."""
        self._stop.set()
        self._thread.join(10)
        return self.n


def drive_direct(a, b):
    """MELEE_DEBUG_VS: Start clears the card message box, skips the opening
    movie, and at the title jumps straight into Link vs Mario (gmtitlemode.c:61,
    gmvsmode.c:163-201). Both instances are in the session from boot, so A's pad
    is B's too and only A is driven - one Start per side would risk pausing the
    match it just started. Press until both logs show the match's own stage
    archive: waiting for the title first deadlocks, because the boot needs a
    Start of its own to get past the memory-card box."""
    if not press_until((a, b), "Return", 9, SCENE_FILE["match"], tries=40, each=8.0, on=(a,)):
        print("net_test: never got a stage archive: the match never loaded", flush=True)
        return False
    if wait_match((a, b), 150):
        return True
    # A Start that landed while the match was loading pauses it; one more
    # unpauses, and a paused match shows up as a frozen checksum either way.
    print("net_test: the match is not simulating, trying one unpause", flush=True)
    press((a,), "Return", 9, 2)
    return wait_match((a, b), 120)


# From a *verified* main menu (cursor on SEL_MAIN_1P): Down = VS MODE, A enters
# the VS submenu on index 0 (mnmain.c:2822-2824), Up wraps to SEL_VS_ONLINE
# (6 entries, mnmain.c:423), A enters the ONLINE menu with SEL_ONLINE_LAN
# already selected (mnmain.c:2629), A takes it into the lobby. X = A button in
# the keyboard map, Z = B.
LAN_SUBMENU = [("Down", 7), ("X", 7), ("Up", 7), ("X", 7), ("X", 7)]


def both(a, b, key, pause):
    for inst in (a, b):
        inst.key(key)
    time.sleep(pause)


def to_main_menu(insts, tries=8):
    """Reach the main menu from anywhere the boot leaves us, and prove it.
    Start is a menu confirm as well as the boot's "dismiss" button
    (PAD_CONFIRM = A | START, gm_1A36.c:118), so pressing it through the card
    message box, the movie and the title walks on into whatever submenu the
    cursor sits on. B backs out one layer per press and the main menu's own B
    returns to the title (mnmain.c:2851-2857), so three B's reach the title
    from any of those depths. The title hands itself over to the opening movie
    after a few idle seconds and cycles back (gmtitlemode.c:73-75), so Start is
    then offered every couple of seconds until one lands inside a title window:
    that re-enters the menu scene, which reloads MnMaAll (a submenu hop does
    not) and always starts on SEL_MAIN_1P (gmmenumode.c:100-103). The rising
    archive count is the proof, and it is what makes the hops below
    deterministic instead of hopeful."""
    for _ in range(tries):
        want = [count(i, SCENE_FILE["menu"]) + 1 for i in insts]

        def there():
            return all(count(i, SCENE_FILE["menu"]) >= w for i, w in zip(insts, want))

        for _ in range(3):
            press(insts, "Z", 7, 1.0)
        for _ in range(10):
            press(insts, "Return", 9, 2.0)
            if there():
                return True
            if any(i.proc.poll() is not None for i in insts):
                return False
    return False


def drive_lan(a, b):
    """Both instances walk the real menus into the LAN lobby, Start on A elects
    a host, then the CSS and SSS. Every step waits for the scene's own archive
    or log line."""
    # No session yet, so each instance needs its own keyboard until the lobby.
    for attempt in range(3):
        if not to_main_menu((a, b)):
            print("net_test: could not get back to a known main menu", flush=True)
            return False
        for key, frames in LAN_SUBMENU:
            press((a, b), key, frames, 1.5)
        if a.wait_log(r"lan: announcing", 25) and b.wait_log(r"lan: announcing", 25):
            break
        print(f"net_test: no lobby on attempt {attempt + 1}, resetting the menus", flush=True)
    else:
        print("net_test: never reached the LAN lobby (no 'lan: announcing')", flush=True)
        return False
    if not (a.wait_log(r"lobby: \d+ players found", 40) and
            b.wait_log(r"lobby: \d+ players found", 40)):
        print("net_test: the two lobbies never saw each other", flush=True)
        return False
    press((a,), "Return", 9, 0)  # Start: elects a host among the ready peers
    if not (a.wait_log(r"lobby: entering CSS", 60) and b.wait_log(r"lobby: entering CSS", 60)):
        print("net_test: the lobby never handed over to the CSS", flush=True)
        return False
    return drive_css(a, b) and drive_sss(a, b)


def drive_css(a, b, want=1):
    """CSS: bring the hand out and ride it to the top edge, then step down a
    row at a time and try A after each step until a pick sticks; Start on A
    once both have picked -> SSS. Stepping beats one calibrated hold: the hand
    moves per frame, so the distance a fixed hold covers depends on the frame
    rate the machine happens to give (one hold of 21 frames left both hands
    off any portrait on a 60 fps run, and the row hung there). The sweep starts
    at the top edge and only works downwards a few rows, so it cannot reach the
    HMN/CPU panel toggles at the bottom of a column - an A press there would
    switch our own slot out of Human."""
    if not (a.wait_log(SCENE_FILE["css"], 120) and b.wait_log(SCENE_FILE["css"], 60)):
        print("net_test: the CSS never loaded (no 'HIT: MnSlChr')", flush=True)
        return False
    press((a, b), "Up", 90, 1.5)
    for _ in range(6):
        press((a, b), "X", 7, 0.8)  # A: pick whatever is under the hand
        if press_until((a, b), "Return", 9, SCENE_FILE["sss"], want=want, tries=2, each=6.0,
                       on=(a,)):
            return True
        press((a, b), "Down", 10, 0.6)
    print("net_test: no CSS pick stuck: the SSS never loaded", flush=True)
    return False


def drive_sss(a, b, want=1):
    """SSS: pin the cursor into the bottom-left corner (a deterministic
    origin), then step out along the row trying A after each step, same reason
    as the CSS. Both peers pick and the two picks resolve through the shared
    seed (mnstagesel.c:60-89)."""
    press((a, b), "Left+Down", 72, 1.5)
    for _ in range(6):
        press((a, b), "X", 7, 0.8)
        if all(count(i, r"sss: picks") >= want for i in (a, b)):
            break
        press((a, b), "Right+Up", 12, 0.6)
    else:
        print("net_test: no 'sss: picks' on both after the sweep", flush=True)
        return False
    return press_until((a, b), "X", 7, SCENE_FILE["match"], want=want, tries=6, each=12.0,
                       on=(a,))


# L+R+A+Start on the pauser's own pad ends a paused VS match as NO CONTEST
# (gmvs.c:1359-1383; a debug build drops START from the mask, and the extra
# buttons are masked out either way). Keyboard map: Q=L, E=R, X=A, Return=Start.
LRAS = "Q+E+X+Return"


# ---- was this a match at all? ------------------------------------------
# MELEE_NET_RECORD writes "MRC1" + seed, then per fresh tick the four PADStatus
# simulated and the frame checksum (net_snapshot.c:36-113). frame_checksum only
# folds fighter position, facing, percent, motion id and stocks while in_fight()
# (:117-139), so at a menu it is a pure function of the four pads and the RNG
# seed and moves only when a key is pressed, while a running match moves it
# every single frame. That is the one signal that separates a real row from the
# title screen - where this whole matrix used to pass with rollbacks 0.
# FrameRecord = four PADStatus + u32 checksum. PADStatus is 16 bytes in this
# build, not the 12 the GameCube header's 11 used bytes suggest (measured with
# the build's own flags: `sizeof(PADStatus)=16`), so the stride is 68 and
# record_stride_ok() refuses a file that does not divide by it rather than
# reading garbage checksums.
REC = 68
CK_OFF = REC - 4
MATCH_WINDOW = 600
MATCH_RATIO = 0.5
MATCH_MIN = 1800  # frames of moving state a row has to get, i.e. 30 s of match


def record_stride_ok(size):
    """A recording is "MRC1" + seed once per session plus whole records."""
    return any((size - 8 * h) % REC == 0 for h in range(1, 13))


def record_base(data):
    """Offset of the first record. A session restart rewrites the header
    mid-file (session_reset takes net.frame back to 0), so the last header
    that leaves a whole number of records is the live one."""
    p = len(data)
    while True:
        p = data.rfind(b"MRC1", 0, p)
        if p < 0:
            return 0
        if (len(data) - p - 8) % REC == 0:
            return p + 8


def record_cks(path, tail=None):
    """Frame checksums, oldest first (index = net.frame within the session).
    `tail` reads only the last N records; records end at EOF, so seeking back
    a multiple of the stride stays aligned whatever headers precede it."""
    try:
        size = os.path.getsize(path)
    except OSError:
        return []
    if size > 8 and not record_stride_ok(size):
        raise RuntimeError(f"{path}: {size} bytes is not a whole number of {REC}-byte records; "
                           "sizeof(PADStatus) changed and REC needs remeasuring")
    with open(path, "rb") as f:
        if tail is not None and size > 8 + tail * REC:
            f.seek(size - tail * REC)
            body = f.read()
        else:
            data = f.read()
            body = data[record_base(data):]
    return [body[i * REC + CK_OFF:i * REC + REC] for i in range(len(body) // REC)]


def moved(cks):
    return [cks[i] != cks[i - 1] for i in range(1, len(cks))]


def match_ratio(path):
    """Share of the last MATCH_WINDOW frames that moved the checksum."""
    m = moved(record_cks(path, MATCH_WINDOW))
    return sum(m) / len(m) if m else 0.0


MATCH_RUN = 60  # consecutive moving frames that only a running match produces


def match_entry(cks):
    """Frame the first real match starts on, or None: the first frame that
    begins MATCH_RUN consecutive frames of moving checksum. A menu moves it
    only when the pads change - a held key is the same pad every frame - so it
    never strings more than a handful together, while a match moves it every
    frame (measured: a driven title run 91/600 frames, a match 600/600)."""
    n = 0
    for i, x in enumerate(moved(cks)):
        n = n + 1 if x else 0
        if n == MATCH_RUN:
            return i - MATCH_RUN + 2  # m[i] compares frame i+1 against frame i
    return None


def wait_match(insts, timeout=240):
    """Block until every instance is simulating a match: its stage archive in
    the log and a MATCH_RUN-long run of moving checksum in the recent
    recording. The archive alone would also match the opening movie (which
    moves the RNG every frame); the checksum alone would not tell a loaded
    match from a paused one."""
    deadline = time.time() + timeout

    def simulating(inst):
        return (count(inst, SCENE_FILE["match"]) > 0 and
                match_entry(record_cks(inst.rec_path, MATCH_WINDOW)) is not None)

    while time.time() < deadline:
        if all(simulating(i) for i in insts):
            return True
        if any(i.proc.poll() is not None for i in insts):
            break
        time.sleep(2)
    for inst in insts:
        print(f"net_test: [{inst.name}] {count(inst, SCENE_FILE['match'])} stage archives and "
              f"{match_ratio(inst.rec_path) * 100:.0f}% of the last {MATCH_WINDOW} frames moving:"
              " not a running match", flush=True)
    return False


def check_match(inst):
    """(fails, note) for the summary line. Every row inherits this: a run that
    never entered a match proves datagram plumbing and time sync only, because
    at a menu the checksum carries no game state at all."""
    cks = record_cks(inst.rec_path)
    stages = count(inst, SCENE_FILE["match"])
    if len(cks) < MATCH_WINDOW:
        return [f"{len(cks)} recorded frames: too few to tell a match from a menu"], "no match"
    entry = match_entry(cks) if stages else None
    if entry is None:
        m = moved(cks)
        return ([f"never entered a match ({stages} stage archives loaded, "
                 f"{100 * sum(m) / len(m):.0f}% of {len(cks)} frames moved the checksum; "
                 "a menu barely moves it)"], "no match")
    n = sum(moved(cks[entry:]))
    fails = []
    if n < MATCH_MIN:
        fails.append(f"only {n} frames of match after frame {entry} (want {MATCH_MIN})")
    return fails, f"match at frame {entry}, {n} frames simulated"


def drive_scenes(a, b):
    """A whole set under sync: LAN lobby -> CSS -> SSS -> match -> results ->
    CSS -> SSS -> match. Every hop waits for its own evidence: the scene's
    archive load, the flow's own log lines ("lobby: entering CSS",
    "sss: picks") and the recorded checksum stream for "the match is really
    simulating"."""
    if not drive_lan(a, b):
        return False
    if not wait_match((a, b)):
        print("net_test: the first match never started simulating", flush=True)
        return False
    w = Workout((a, b))  # make the first match a real one before leaving it
    time.sleep(20)
    print(f"net_test: first match played, {w.done()} key lines", flush=True)
    # Out of the match the way a player does it: pause, then L+R+A+Start on the
    # pauser's own pad is NO CONTEST and the set goes to results.
    press((a,), "Return", 9, 2)
    press((a,), LRAS, 30, 3)
    if not press_until((a, b), "Return", 9, SCENE_FILE["css"], want=2, tries=10, each=8.0,
                       on=(a,)):
        print("net_test: results never handed back to the CSS (no second 'HIT: MnSlChr')",
              flush=True)
        return False
    if not drive_css(a, b, want=2):
        return False
    if not drive_sss(a, b, want=2):
        return False
    return wait_match((a, b))


def check_entry(a, b):
    """Both peers must have entered the match on the same frame. Inputs are
    synced and the clocks are locked together, so the scene hand-off is
    frame-exact; this is the frame-stamped stage anchor the direct rows have
    (the LAN flow also stamps the CSS hand-off, see check_scenes)."""
    e = [match_entry(record_cks(i.rec_path)) for i in (a, b)]
    if None in e:
        return []  # check_match reports that with its own detail
    return [] if e[0] == e[1] else [f"match entered on different frames: a {e[0]} b {e[1]}"]


# "net: frame N, rollbacks N (max depth N, lost N), stalls N (worst X ms), skips N,
# advances N, ping N ms (avg X, min Y, max Z, jitter J), loss L% (...)"; the
# ping parenthesis and loss are optional so older logs still parse.
STATS_RX = re.compile(r"net: frame (\d+), rollbacks (\d+) \(max depth (\d+), lost (\d+)\), "
                      r"stalls (\d+) \(worst ([\d.]+) ms\), skips (\d+), advances (\d+), "
                      r"ping (\d+) ms(?: \(avg [\d.]+, min \d+, max \d+, jitter ([\d.]+)\))?"
                      r"(?:, loss (\d+)%)?")

# Scene-flow anchors: the online lobby's frame-stamped hand-off into the CSS
# (gmonlinemode.c:378), the SSS's resolved pick (mnstagesel.c:89), the peer we
# actually elected (net_lan.c:663,677), the stage archive a match loads, the
# barrier field STATS_RX stops short of, and the snapshot report
# (net_snapshot.c:382).
CSS_RX = re.compile(r"lobby: entering CSS at frame (\d+), seed (\d+)")
PICKS_RX = re.compile(r"sss: picks P1=(-?\d+) P2=(-?\d+) -> (-?\d+)")
CONNECT_RX = re.compile(r"lan: connect (\d+\.\d+\.\d+\.\d+):(\d+) as P(\d)")
STAGE_RX = re.compile(r"\[FileCache\] (?:LOOSE )?HIT: (Gr[A-Za-z0-9]+)\.(?:dat|usd)")
BARRIER_RX = re.compile(r"net: frame (\d+),.*?barrier (-?\d+)")
OOM_RX = re.compile(r"net: out of memory for snapshots at frame (\d+), lockstep from here")
SNAP_RX = re.compile(r"net:\s+snapshot take [\d.]+ ms \(max [\d.]+, n (\d+)\)")


def check_scenes(a, b):
    """--scenes: what a single log cannot show. Both peers must have elected
    each other (not a third lobby on the LAN), left the lobby for the CSS on
    the same frame from the same seed - the one stage hand-off the flow itself
    frame-stamps - resolved both SSS picks to the same stage, and loaded the
    same stage archives in the same order. Everything between those anchors is
    covered by the checksum stream: net.c compares a per-frame checksum with
    the peer on almost every frame, so "no DESYNC" over the whole set is the
    frame-by-frame agreement, and check_match() proves the matches ran.
    The rollback barrier is deliberately NOT compared: it is raised to
    frame+120 by any game-thread disc request (net.c:1091-1097) and the two
    sides issue those 1-2 frames apart, measured, so it is not an assertion."""
    fails = []
    for inst, peer in ((a, b), (b, a)):
        got = CONNECT_RX.findall(inst.text())
        if not got:
            fails.append(f"{inst.name}: no 'lan: connect', never left the lobby")
        elif int(got[0][1]) != peer.port:
            fails.append(f"{inst.name}: elected udp/{got[0][1]}, not its partner's "
                         f"udp/{peer.port} (another melee-pc lobby was on the LAN)")
    css = [CSS_RX.findall(inst.text()) for inst in (a, b)]
    if not all(css):
        fails.append("no 'lobby: entering CSS' on both")
    elif css[0][0] != css[1][0]:
        fails.append(f"CSS entered at different (frame, seed): a {css[0][0]} b {css[1][0]}")
    picks = [PICKS_RX.findall(inst.text()) for inst in (a, b)]
    if min(len(p) for p in picks) < 2:
        fails.append(f"{len(picks[0])}/{len(picks[1])} 'sss: picks' lines (want 2 each: the set "
                     "has to come back round to the SSS through results)")
    elif picks[0][:2] != picks[1][:2]:
        fails.append(f"SSS picks differ: a {picks[0][:2]} b {picks[1][:2]}")
    for scene, want in (("css", 2), ("sss", 2)):
        got = [count(inst, SCENE_FILE[scene]) for inst in (a, b)]
        if min(got) < want:
            fails.append(f"{scene.upper()} loaded {got[0]}/{got[1]} times (want {want} each)")
    stages = [STAGE_RX.findall(inst.text()) for inst in (a, b)]
    if stages[0] != stages[1]:
        fails.append(f"different stage archives loaded: a {stages[0]} b {stages[1]}")
    return fails


def check_oom(inst, oom_frame):
    """--oom: the counters, not just the log line. A snapshot that cannot be
    taken pins the barrier at INT32_MAX (net.c:961-964), which stops every
    further prediction, so snapshots must have been taken before the failure
    and none after it, and the run has to carry on to its exit frame in sync
    (summarize() and check_match() cover that half). Ordering is read off the
    log rather than frame numbers because the FileCache lines carry none."""
    text = inst.text()
    hits = OOM_RX.findall(text)
    if len(hits) != 1:
        return [f"{len(hits)}x 'out of memory for snapshots' (want exactly 1): "
                "MELEE_NET_SIM_OOM_FRAME only fires on a snapshot the engine really takes, "
                "and only a match past the barrier takes any"]
    fails = []
    f = int(hits[0])
    if f < oom_frame:
        fails.append(f"snapshot failed at frame {f}, before MELEE_NET_SIM_OOM_FRAME={oom_frame}")
    lines = text.splitlines()
    oom_at = next(i for i, ln in enumerate(lines) if OOM_RX.search(ln))
    stage_at = next((i for i, ln in enumerate(lines) if STAGE_RX.search(ln)), None)
    if stage_at is None or stage_at > oom_at:
        fails.append("no stage archive loaded before the failure: the snapshot that failed was "
                     "not one from a match")
    takes = [(i, int(m.group(1))) for i, ln in enumerate(lines) for m in [SNAP_RX.search(ln)] if m]
    before = [n for i, n in takes if i < oom_at]
    after = [n for i, n in takes if i > oom_at]
    if not before or max(before) == 0:
        fails.append("no snapshot was taken before the failure: nothing was there to lose")
    if len(after) < 2:
        fails.append(f"{len(after)} snapshot reports after the failure: too short to show "
                     "lockstep")
    elif any(after[1:]):  # the first report still counts takes from before the failure
        fails.append(f"snapshots still taken after the failure (n {after}): the session did not "
                     "drop to lockstep")
    bar = BARRIER_RX.findall(text)
    if not bar:
        fails.append("no barrier field in the stats lines")
    elif int(bar[-1][1]) != 2147483647:
        fails.append(f"barrier {bar[-1][1]} at the end, want INT32_MAX (lockstep for the "
                     "session)")
    return fails


def stats_line(inst):
    """(stats dict for tools/net_acceptance.py, one-line summary) from the
    periodic "net: frame" reports."""
    text = inst.text()
    stats = STATS_RX.findall(text)
    line = f"[{inst.name}] "
    st = {}
    if stats:
        s = stats[-1]
        pings = [int(x[8]) for x in stats]
        worst = max(float(x[5]) for x in stats)
        st = {"frame": s[0], "rollbacks": s[1], "max_depth": s[2], "lost": s[3], "stalls": s[4],
              "worst_ms": f"{worst:.1f}", "skips": s[6], "advances": s[7],
              "ping": f"{min(pings)}-{max(pings)}", "jitter": s[9] or "-", "loss": s[10] or "-"}
        line += (f"frame {s[0]}, rollbacks {s[1]} (max depth {s[2]}, lost {s[3]}), stalls {s[4]} "
                 f"(worst {worst:.1f} ms), skips {s[6]}, advances {s[7]}, "
                 f"ping {st['ping']} ms")
        if s[9]:
            line += f", jitter {s[9]} ms"
        if s[10]:
            line += f", loss {s[10]}%"
    else:
        line += "no stats line"
    q = re.findall(r"quality (\d+) \(worst (\d+)\)", text)
    if q:
        st["quality"] = f"{q[-1][0]} (worst {max(int(x[1]) for x in q)})"
        line += f", quality {st['quality']}"
    return st, line


def summarize(inst):
    """(fails, one-line summary, stats dict for tools/net_acceptance.py). The
    match precondition is in here, so every row inherits it and none can pass
    from a menu again."""
    text = inst.text()
    fails = []
    if not re.search(r"net: test done at frame (\d+)", text):
        fails.append("no 'net: test done'")
    if inst.proc.returncode != 0:
        fails.append(f"exit code {inst.proc.returncode}")
    for pat in ("net: DESYNC", "peer silent", "cannot roll back", "REPLAY DIVERGED"):
        n = len(re.findall(re.escape(pat), text))
        if n:
            fails.append(f"{n}x '{pat}'")
    match_fails, note = check_match(inst)
    fails += match_fails
    st, line = stats_line(inst)
    st["match"] = note
    line += f", {note}"
    d = re.search(r"net: DESYNC.*", text)
    if d:
        line += "\n" + f"[{inst.name}] " + d.group(0)
    return fails, line, st


# Documented stall limit (docs/netcode-plan.md §6.2, STALL_TIMEOUT_MS,
# net_internal.h:100) plus room for the loaded machine to notice it.
PEER_GONE_S = 15


def run_disconnect(args):
    """B SIGKILLed mid-match, no BYE: A must report the peer gone within the
    stall timeout and carry on running instead of hanging. Same return shape
    as run(). MELEE_NET_RECONNECT_MS=0 pins the hard-drop path, MELEE_FPS=1
    gives the per-second line that shows the frame loop is still alive after
    the session is gone (a clean exit by BYE is tools/net_lan_test.py's
    `direct` case; this is the ungraceful half)."""
    frames = int(args.minutes * 3600) + BOOT_FRAMES
    env = {"MELEE_NET_EXIT_AFTER_FRAMES": str(frames), "MELEE_NET_RECONNECT_MS": "0"}
    shutil.rmtree(args.work, ignore_errors=True)
    os.makedirs(args.work)
    a = Instance("a", args.exe, args.disc, args.work, args.port, args.port + 1, env, False)
    b = Instance("b", args.exe, args.disc, args.work, args.port + 1, args.port, env, False)
    print(f"net_test: disconnect, pids={a.proc.pid},{b.proc.pid} logs={args.work}", flush=True)
    fails = []
    work = None
    try:
        if not drive_direct(a, b):
            fails.append("the drive never reached a match")
        elif not wait_match((a, b)):
            fails.append("no match to interrupt: the checksum stream says neither side was "
                         "simulating one")
        if not fails:
            work = Workout((a, b))  # a real match to interrupt, not two idle fighters
            time.sleep(20)
            entry = [match_entry(record_cks(i.rec_path)) for i in (a, b)]
            print(f"net_test: match entered at frame {entry[0]}/{entry[1]}, "
                  f"{work.done()} key lines played", flush=True)
            work = None
            fps_before = a.text().count("\nfps ")
            stats = STATS_RX.findall(b.text())
            b_frame = stats[-1][0] if stats else "?"
            b.kill()  # SIGKILL: no BYE, the survivor has to time the peer out
            t0 = time.time()
            print(f"net_test: SIGKILLed b around frame {b_frame}", flush=True)
            if not a.wait_log(r"net: peer silent for \d+ ms at frame \d+, leaving netplay",
                              PEER_GONE_S):
                fails.append(f"a never reported the peer gone within {PEER_GONE_S} s")
            gone = time.time() - t0
            if not a.wait_log(r"net: disconnected at frame \d+ \(status 2\)", 5):
                fails.append("a has no 'net: disconnected ... (status 2)' (PEER_TIMEOUT)")
            time.sleep(10)  # a sane state means it keeps running, not that it survived one frame
            if a.proc.poll() is not None:
                fails.append(f"a exited on its own with code {a.proc.returncode}")
            elif a.text().count("\nfps ") - fps_before < 5:
                fails.append("a stopped printing its per-second fps line: the frame loop hung")
            for pat in ("net: DESYNC", "cannot roll back", "net: peer left"):
                if pat in a.text():
                    fails.append(f"a logged '{pat}'")
            print(f"net_test: a reported the peer gone {gone:.1f} s after the SIGKILL, "
                  f"{a.text().count(chr(10) + 'fps ') - fps_before} fps lines since", flush=True)
    finally:
        if work is not None:
            work.done()
        a.kill()
        b.kill()
    results = []
    for inst, f in ((a, fails), (b, [])):
        st, line = stats_line(inst)
        entry = match_entry(record_cks(inst.rec_path))
        st["match"] = f"match at frame {entry}" if entry is not None else "no match"
        line += f", {st['match']}"
        if inst is b:
            line += ", SIGKILLed mid-match on purpose"
        print(line)
        if f:
            print(f"[{inst.name}] FAIL: " + ", ".join(f))
        results.append((inst.name, f, line, st))
    return not fails, results


def parse_args(argv=None):
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--loss", type=int, default=0, help="MELEE_NET_SIM_LOSS percent")
    ap.add_argument("--delay", type=int, default=0, help="MELEE_NET_SIM_DELAY_MS one way")
    ap.add_argument("--rxdelay", type=int, default=0, help="MELEE_NET_SIM_DELAY_RX_MS (asymmetric)")
    ap.add_argument("--jitter", action="store_true")
    ap.add_argument("--reorder", action="store_true")
    ap.add_argument("--burst", action="store_true")
    ap.add_argument("--dup", action="store_true")
    ap.add_argument("--minutes", type=float, default=2)
    ap.add_argument("--lan", action="store_true")
    ap.add_argument("--scenes", action="store_true",
                    help="LAN menus through a whole set: CSS, SSS, match, results, rematch")
    ap.add_argument("--oom", type=int, default=0, metavar="FRAME",
                    help="MELEE_NET_SIM_OOM_FRAME=FRAME on A (snapshot allocation failure)")
    ap.add_argument("--disconnect", action="store_true",
                    help="SIGKILL B mid-match; assert A times the peer out and keeps running")
    ap.add_argument("--fuzz", action="store_true", help="run tools/net_fuzz.py against A")
    ap.add_argument("--fuzz-seconds", type=int, default=30)
    ap.add_argument("--exe", default=os.path.join(here, "..", "build", "melee"))
    ap.add_argument("--disc", default=os.path.join(here, "..", "..", "melee.ciso"))
    ap.add_argument("--port", type=int, default=42050, help="A's UDP port; B uses +1")
    ap.add_argument("--work", default="/tmp/net_test")
    return ap.parse_args(argv)


def run(args):
    """One two-instance run. Returns (ok, [(name, fails, line, stats), ...])."""
    here = os.path.dirname(os.path.abspath(__file__))
    if args.disconnect:
        return run_disconnect(args)
    if args.scenes:
        args.lan = True  # the set flow is the online one: lobby, CSS, SSS, VS, results
    frames = int(args.minutes * 3600) + BOOT_FRAMES
    sim = {"MELEE_NET_EXIT_AFTER_FRAMES": str(frames)}
    if args.loss:
        sim["MELEE_NET_SIM_LOSS"] = str(args.loss)
    if args.delay:
        sim["MELEE_NET_SIM_DELAY_MS"] = str(args.delay)
    if args.rxdelay:
        sim["MELEE_NET_SIM_DELAY_RX_MS"] = str(args.rxdelay)
    for flag, (var, val) in SIM_ENV.items():
        if getattr(args, flag):
            sim[var] = val

    sim_a = dict(sim)
    if args.oom:
        sim_a["MELEE_NET_SIM_OOM_FRAME"] = str(args.oom)
    shutil.rmtree(args.work, ignore_errors=True)
    os.makedirs(args.work)
    a = Instance("a", args.exe, args.disc, args.work, args.port, args.port + 1, sim_a, args.lan)
    b = Instance("b", args.exe, args.disc, args.work, args.port + 1, args.port, sim, args.lan)
    print(f"net_test: {'lan' if args.lan else 'direct'} loss={args.loss}% delay={args.delay}ms "
          f"rxdelay={args.rxdelay}ms {' '.join(k for k in SIM_ENV if getattr(args, k))} "
          f"minutes={args.minutes} frames={frames} pids={a.proc.pid},{b.proc.pid} "
          f"logs={args.work}", flush=True)
    ok = True
    work = None
    try:
        if args.scenes:
            ok = drive_scenes(a, b)
            if not ok:
                print("net_test: scene drive failed (see logs)", flush=True)
        elif args.lan:
            ok = drive_lan(a, b)
            if not ok:
                print("net_test: LAN drive failed (see logs)", flush=True)
        else:
            ok = drive_direct(a, b)
            if not ok:
                print("net_test: never got into the match (see logs)", flush=True)
        # Every row, not just the new ones: a run that never entered a match
        # measures the transport at a menu, which is how this matrix used to
        # pass green from the title screen.
        if ok and not args.scenes:
            ok = wait_match((a, b))
        if ok:
            work = Workout((a, b))  # keep both players moving for the rest of the run
        if ok and args.fuzz:
            sys.path.insert(0, here)
            import net_fuzz
            time.sleep(5)  # into the match first
            f = net_fuzz.run(args.port, a.proc.pid, a.log_path, args.fuzz_seconds)
            print(f"net_test: fuzz {'pass' if f else 'FAIL'}", flush=True)
            ok = ok and f
        # net.c self-exits at `frames`; this is a safety net. Slow links and a
        # loaded machine run well below 60 fps, so budget by frame progress
        # (assume a floor of 12 fps) rather than wall-clock minutes. A run
        # that stops advancing its "net: frame N" for 25 s is stuck (a
        # disconnect leaves the title looping but never self-exits), and a
        # terminal marker means it already failed: kill early either way.
        deadline = time.time() + frames / 12.0 + 60
        term = ("net: DESYNC", "peer silent", "net: disconnected", "cannot roll back")
        last_frame, last_progress = -1, time.time()
        while time.time() < deadline and (a.proc.poll() is None or b.proc.poll() is None):
            time.sleep(1)
            texts = a.text() + b.text()
            if any(t in texts for t in term):
                break  # let summarize() report it; do not burn the whole budget
            fr = max([int(x) for x in re.findall(r"net: frame (\d+)", texts)] or [-1])
            if fr > last_frame:
                last_frame, last_progress = fr, time.time()
            elif time.time() - last_progress > 25 and last_frame >= 0:
                print("net_test: no frame progress for 25 s, giving up", flush=True)
                break
    finally:
        if work is not None:
            print(f"net_test: workout wrote {work.done()} key lines", flush=True)
        # An instance that reached its exit frame (or took its peer's BYE that
        # close to it) is already tearing the GPU down, which takes seconds;
        # SIGKILL only what is still running after that.
        a.kill(20)
        b.kill(20)
        if a.proc.returncode == -9 or b.proc.returncode == -9:
            print("net_test: killed a run that would not exit (per-instance FAIL below)",
                  flush=True)
    # Cross-instance assertions belong to neither log; they ride on A's row.
    extra = check_entry(a, b)
    extra += check_scenes(a, b) if args.scenes else check_oom(a, args.oom) if args.oom else []
    results = []
    for inst in (a, b):
        fails, line, st = summarize(inst)
        if inst is a:
            fails = fails + extra
        print(line)
        if fails:
            ok = False
            print(f"[{inst.name}] FAIL: " + ", ".join(fails))
        results.append((inst.name, fails, line, st))
    return ok, results


def main():
    ok, _ = run(parse_args())
    print("net_test: " + ("PASS" if ok else "FAIL"))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
