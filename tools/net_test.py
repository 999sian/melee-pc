#!/usr/bin/env python3
"""Netplay integration test: two instances on one machine, a match, then asserts.

    tools/net_test.py [--loss PCT] [--delay MS] [--rxdelay MS] [--jitter] [--reorder]
                      [--burst] [--dup] [--minutes N] [--lan] [--scenes] [--oom FRAME]
                      [--disconnect [a|b]] [--stall SECONDS] [--reconnect-ms MS]
                      [--fuzz] [--exe build/melee]
                      [--disc ../melee.ciso] [--port 42050] [--work /tmp/net_test]

Direct mode (default): MELEE_NET=127.0.0.1:<peer> + MELEE_DEBUG_VS=1, Start
pressed twice through MELEE_KEY_FIFO (title, then straight into Link vs Mario).
--lan: both instances walk the real menus into the LAN lobby (VS Mode -> ONLINE
-> LAN PLAY), Start on one elects the host, then the CSS by sweeping the
portrait grid and the SSS by one Start on its random cell; needs the shared LAN
free (coordinate with anyone else in the lobby).
--scenes: --lan plus the rest of the set - match, out of it by L+R+A+Start,
results, CSS, SSS, rematch - and the cross-log assertions in check_scenes()
(same CSS frame and seed, identical SSS picks twice, same stage archives).
--oom FRAME: MELEE_NET_SIM_OOM_FRAME on A, so its first
snapshot at/after FRAME fails like a realloc would; check_oom() asserts the
session drops to lockstep and finishes anyway. --disconnect: B SIGKILLed
mid-match (no BYE), asserting A times the peer out within the stall timeout
and keeps its frame loop running. --stall SECONDS: B SIGSTOPped mid-match,
asserting the reconnect window (net.c:639-832) survives an interruption
inside it and expires with status 2 past it. --fuzz: tools/net_fuzz.py
hammers instance A's port during the match.

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
LOAD_STALL_FRAME = 900  # --load-stall: session up, menus lockstep, every frame waits
# A direct session derives a datagram key from its handshake nonces like any
# other (src/pc/net_handshake.c), but only once that handshake completes a few
# hundred frames into boot; before it there is nothing to authenticate with
# unless the peers were started with a shared secret. Every fixture here sets
# one, so the matrix exercises the authenticated path from the first datagram
# rather than the window no player should be exposed to; tools/net_fuzz.py
# reads this to tag its own datagrams. A LAN fixture leaves it unset and keys
# off the handshake.
NET_KEY = "melee-pc net fixture key"
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


def wait_port_free(port, timeout=30):
    """Block until `port` can be bound, or give up. The previous run's
    instances hold their UDP port through aurora's GPU teardown, which takes
    seconds; starting on top of that makes the new instances fail to bind and
    play unconnected offline games that look like a desync."""
    import socket
    deadline = time.time() + timeout
    while True:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.bind(("", port))
            return True
        except OSError:
            if time.time() > deadline:
                print(f"net_test: UDP {port} still in use after {timeout}s", flush=True)
                return False
            time.sleep(0.5)
        finally:
            s.close()


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
        # A prior direct/replay run must not silently change a LAN fixture.
        for key in ("MELEE_DEBUG_VS", "MELEE_NET", "MELEE_NET_PLAYER", "MELEE_NET_REPLAY",
                    "MELEE_NET_KEY"):
            e.pop(key, None)
        e.update({
            "SDL_VIDEO_DRIVER": os.environ.get("SDL_VIDEO_DRIVER", os.environ.get("SDL_VIDEODRIVER", "x11")),
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
            e["MELEE_NET_KEY"] = NET_KEY
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


def settle(insts, frames):
    """Wait out `frames` simulated frames of wall clock. Every menu hop plays
    an entry animation and ignores input while it runs, and an animation is
    counted in frames, so a fixed 1.5 s wait lands in the middle of one on a
    machine running at 20 fps and the press that follows is swallowed.
    Measured: /tmp/sf_scenes1, where b's Down at the main menu was dropped and
    the fixed schedule walked it into the vanilla CSS instead of the lobby
    while a reached the lobby from the identical key sequence."""
    time.sleep(max(frames * frame_ms(i) for i in insts) / 1000.0)


# From a *verified* main menu (cursor on SEL_MAIN_1P): Down = VS MODE, A enters
# the VS submenu, Up wraps to SEL_VS_ONLINE, A enters the ONLINE menu with
# SEL_ONLINE_LAN already selected, A takes it into the lobby. Every submenu
# entry resets the cursor to index 0 (mn_804A04F0.hovered_selection = 0,
# mnmain.c:2385) and the VS table has 6 entries on TARGET_PC (mn_803EB6B0,
# mnmain.c:420-424) with SEL_VS_ONLINE last (forward.h:166-171), so the walk
# is stateless: a retry starts from the same place. X = A button in the
# keyboard map, Z = B.
LAN_SUBMENU = [("Down", 7), ("X", 7), ("Up", 7), ("X", 7), ("X", 7)]
MENU_SETTLE = 90  # frames of animation a menu hop can eat a press during
CSS_BACK = 40     # frames of held B that leave a character-select screen (>30)


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
    deterministic instead of hopeful.

    B is held for CSS_BACK frames, not tapped: a character-select screen only
    leaves on 30 frames of held B (mncharsel.c:2579-2585), and a walk that
    dropped one press lands in exactly that screen, where every menu key is
    otherwise a no-op. A 7-frame tap left both instances stuck there for the
    rest of the run (/tmp/sf_scenes1)."""
    for _ in range(tries):
        want = [count(i, SCENE_FILE["menu"]) + 1 for i in insts]

        def there():
            return all(count(i, SCENE_FILE["menu"]) >= w for i, w in zip(insts, want))

        for _ in range(3):
            press(insts, "Z", CSS_BACK, 1.0)
        for _ in range(10):
            press(insts, "Return", 9, 2.0)
            if there():
                settle(insts, MENU_SETTLE)  # the menu is loading, not listening yet
                return True
            if any(i.proc.poll() is not None for i in insts):
                return False
    return False


def in_lobby(inst):
    """In the LAN lobby now: it announced and has not stopped since."""
    text = inst.text()
    return text.rfind("lan: announcing") > text.rfind("lan: stopped")


def to_lobby(inst, tries=4):
    """One instance from wherever it is into the LAN lobby, proven by its own
    announce. Per instance and not in lockstep, because until the session
    exists each instance needs its own keyboard and the two walks fail
    independently: a shared retry re-walks the peer that already made it, and
    to_main_menu's B presses take it straight back out of the lobby (measured:
    /tmp/sf_scenes1, a announced, b did not, and the retry lost both)."""
    for attempt in range(tries):
        if in_lobby(inst):
            return True
        if to_main_menu((inst,)):
            for key, frames in LAN_SUBMENU:
                press((inst,), key, frames, 0)
                settle((inst,), MENU_SETTLE)
            if inst.wait_log(r"lan: announcing", 25) and in_lobby(inst):
                return True
        print(f"net_test: [{inst.name}] no lobby on attempt {attempt + 1}, resetting the menus",
              flush=True)
    return False


def drive_lan(a, b):
    """Both instances walk the real menus into the LAN lobby, Start on A elects
    a host, then the CSS and SSS. Every step waits for the scene's own archive
    or log line."""
    for inst in (a, b):
        if not to_lobby(inst):
            print(f"net_test: [{inst.name}] never reached the LAN lobby "
                  "(no 'lan: announcing')", flush=True)
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


# CSS hand geometry, all read off src/melee/mn/mncharsel.c - measured
# constants, not a calibration:
#   speed   getStickDelta (:546) returns (|stick|^2 - 200) scaled onto the
#           stick's unit vector and CursorThink (:2595) adds 0.0002 of that
#           per frame. The keyboard map's full deflection is +-80
#           (keyboard.c:47-51), so a straight hold moves
#           0.0002*(80^2-200) = 1.24 units/frame and a diagonal 1.78 per axis.
#   clamps  x [-35, 26], y [-22, 25] (:2629-2640), so a long hold pins the
#           hand on a corner exactly, whatever frame rate the machine gives.
#   token   a hand inside y (0.2, 22) whose door has no pick takes its own
#           token automatically, no A needed (:3397-3446), and the token then
#           sits at hand + (2.7, -2.0) (:3433-3444). The hit test is on the
#           token, not the hand (:2729-2733).
#   grid    rows are token y (13,20) top, (6,13) middle, (-1,6) bottom
#           (:100-103); columns are 7 units wide from -30.0 to 30.2
#           (:111-120). Only icons with state >= 1 can be picked and state is
#           gm_IsCKindUnlocked() (:4473), so a locked character is a hole in
#           the grid: the middle row is swept because its columns 1-7 (Fox,
#           Ness, ICs, Kirby, Samus, Zelda, Link) are all starters.
#   pick    token in bounds + A sets players[].ckind and releases the token
#           (:2738-2780). It sticks: the hand can move on and press A again
#           without undoing it, as long as it stays 3 units clear of its own
#           dropped token (the re-grab test at :3336-3345).
#   Start   refused unless every door with p_kind != 3 has a pick AND no hand
#           is still holding a token (:3738-3785) - which is why the sweep
#           presses Start only between rows, never while a hand is in flight.
#   hazards A above y 22 is the rules panel: at x > 17.3 it leaves for the
#           lobby and at x < -25.5 it toggles teams (:2848-2853, :2994-3014),
#           and A below y 0.2 hits the HMN/CPU toggle (:3097-3131). The sweep
#           never presses A outside y (0.2, 22), and B is never held on the
#           CSS at all: 30 frames of it also leaves for the lobby (:2579-2585).
CSS_PIN = 40      # frames of Left+Up: 40*1.78 = 71 units, past both clamps
# Down frames from the y=25 clamp, one row of the grid per entry. A wall-clock
# hold delivers the frames the machine's current rate gives it, measured at
# 73-100% of the frames asked for (/tmp/sf_scenes2: 40 asked, 40 delivered at
# the pin; 11 asked, 8 delivered one press later), so the entries are spread
# far enough apart that both ends of that spread land inside the band, and a
# row that lands on a row *boundary* is followed by ones that do not.
CSS_ROWS = (11, 8, 14, 6, 17)
CSS_FIRST = 9     # frames of Right from x=-35: token x -21.1, column 1
CSS_STEP = 6      # frames of Right: 7.4 units, one column
CSS_COLS = 7      # columns 1..7 of the row


def drive_css(a, b, want=1):
    """CSS: pin both hands on the top-left clamp, drop into a row of
    portraits and press A once per column until a pick sticks on both ports,
    then Start -> SSS. Both peers press: each instance drives its own port's
    hand (P1 on the host, P2 on the guest) and the Start gate needs both.
    The row is swept rather than aimed at one portrait because the only
    imprecise quantity is how many frames a wall-clock hold covers; a column
    is 7 units wide and the whole swept row is 49, so the sweep absorbs the
    frame-rate error that a single calibrated hold could not (the inherited
    Up 90 + Down 21 left both hands off the grid entirely). Start between rows
    is the only evidence the game gives that a pick landed: the CSS logs
    nothing, and the SSS's own archive load is what proves the gate opened."""
    if not (a.wait_log(SCENE_FILE["css"], 180) and b.wait_log(SCENE_FILE["css"], 60)):
        print("net_test: the CSS never loaded (no 'HIT: MnSlChr')", flush=True)
        return False
    settle((a, b), MENU_SETTLE)  # the doors are still opening; the hands are not live yet
    for row, down in enumerate(CSS_ROWS):
        press((a, b), "Left+Up", CSS_PIN, 1.2)  # both hands on the (-35, 25) clamp
        press((a, b), "Down", down, 0.8)        # into a row; the token comes out here
        press((a, b), "Right", CSS_FIRST, 0.6)
        for col in range(CSS_COLS):
            press((a, b), "X", 7, 0.5)  # A: pick whatever the token is over
            press((a, b), "Right", CSS_STEP, 0.4)
        if press_until((a, b), "Return", 9, SCENE_FILE["sss"], want=want, tries=2, each=8.0,
                       on=(a,)):
            print(f"net_test: CSS picked on row {row} (Down {down}), SSS loaded", flush=True)
            return True
        print(f"net_test: no pick on CSS row {row} (Down {down}), trying the next row",
              flush=True)
    print("net_test: no CSS pick stuck: the SSS never loaded", flush=True)
    return False


# Confirm through just one port: the other peer must see the synchronized input.
PICKED_RX = re.compile(r"sss: we picked (\d+)")


def drive_sss(a, b, want=1):
    return press_until((a, b), "Return", 9, r"sss: we picked", want=want,
                       tries=8, each=6.0)


# L+R+A+Start on the pauser's own pad ends a paused VS match as NO CONTEST
# (gmvs.c:1359-1383; a debug build drops START from the mask, and the extra
# buttons are masked out either way). Keyboard map: Q=L, E=R, X=A, Return=Start.
LRAS = "Q+E+X+Return"


# ---- was this a match at all? ------------------------------------------
# MELEE_NET_RECORD writes "MRC2" + seed, then per fresh tick the four PADStatus
# simulated and the frame checksum (net_snapshot.c:36-113). frame_checksum only
# folds fighter position, facing, percent, motion id and stocks while in_fight()
# (:117-139), so at a menu it is a pure function of the four pads and the RNG
# seed and moves only when a key is pressed, while a running match moves it
# every single frame. That is the one signal that separates a real row from the
# title screen - where this whole matrix used to pass with rollbacks 0.
# FrameRecord = four PADStatus + u32 checksum + u32 tick-start seed.
# PADStatus is 16 bytes in this
# build, not the 12 the GameCube header's 11 used bytes suggest (measured with
# the build's own flags: `sizeof(PADStatus)=16`), so the stride is 72 and
# record_stride_ok() refuses a file that does not divide by it rather than
# reading garbage checksums.
REC = 76
CK_OFF = 64
REC_FORMATS = {b"MRC1": 68, b"MRC2": 72, b"MRC3": 76, b"MRC4": REC}  # earlier captures stay readable
MATCH_WINDOW = 600
MATCH_RATIO = 0.5
MATCH_MIN = 1800  # frames of moving state a row has to get, i.e. 30 s of match


def record_stride_ok(size, stride=REC):
    """A recording has magic + seed once per session plus whole records."""
    return any((size - 8 * h) % stride == 0 for h in range(1, 13))


def record_base(data):
    """Offset of the first record. A session restart rewrites the header
    mid-file (session_reset takes net.frame back to 0), so the last header
    that leaves a whole number of records is the live one."""
    magic = data[:4]
    stride = REC_FORMATS.get(magic)
    if stride is None:
        raise RuntimeError(f"unknown recording magic {magic!r}")
    p = len(data)
    while True:
        p = data.rfind(magic, 0, p)
        if p < 0:
            return 0
        if (len(data) - p - 8) % stride == 0:
            return p + 8


def record_cks(path, tail=None):
    """Frame checksums, oldest first (index = net.frame within the session).
    `tail` returns the last N records of the latest session, excluding any
    earlier session header. MRC1 captures remain readable after MRC2 ships."""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return []
    if not data:
        return []
    size = len(data)
    stride = REC_FORMATS.get(data[:4])
    if stride is None:
        raise RuntimeError(f"{path}: unknown recording magic {data[:4]!r}")
    if size > 8 and not record_stride_ok(size, stride):
        raise RuntimeError(f"{path}: {size} bytes is not a whole number of {stride}-byte records; "
                           "sizeof(PADStatus) changed and REC needs remeasuring")
    body = data[record_base(data):]
    if tail is not None:
        body = body[-tail * stride:] if tail > 0 else b""
    return [body[i * stride + CK_OFF:i * stride + CK_OFF + 4]
            for i in range(len(body) // stride)]


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


def in_match_scene(inst):
    """Count of evidence that this instance's scene is GS_VS. The stage
    archive is the obvious marker but it is not sufficient on its own: the
    file layer prewarms five stages by name at boot, and a stkind of 0 makes
    the loader ask for the empty name "Gr.dat", which STAGE_RX does not match
    either. The snapshot report is the one that cannot be fooled: a snapshot
    is only ever taken off the prediction path, prediction is refused unless
    in_fight() (net.c:1345-1362, in_fight() = scene_kind() is GS_VS or
    GS_SUDDEN_DEATH at :173), so `n` above zero IS "the sim is in a match"."""
    snaps = [int(n) for n in SNAP_RX.findall(inst.text())]
    return count(inst, SCENE_FILE["match"]) + sum(1 for n in snaps if n > 0)


def wait_match(insts, timeout=240):
    """Block until every instance is simulating a match: its scene is GS_VS
    and the recent recording has a MATCH_RUN-long run of moving checksum.
    The scene alone would also accept an idle match, whose checksum is
    constant; the checksum alone would also accept the opening movie, which
    moves the RNG every frame."""
    deadline = time.time() + timeout

    def simulating(inst):
        return (in_match_scene(inst) > 0 and
                match_entry(record_cks(inst.rec_path, MATCH_WINDOW)) is not None)

    while time.time() < deadline:
        if all(simulating(i) for i in insts):
            return True
        if any(i.proc.poll() is not None for i in insts):
            break
        time.sleep(2)
    for inst in insts:
        print(f"net_test: [{inst.name}] {in_match_scene(inst)} GS_VS markers and "
              f"{match_ratio(inst.rec_path) * 100:.0f}% of the last {MATCH_WINDOW} frames moving:"
              " not a running match", flush=True)
    return False


def check_match(inst):
    """(fails, note) for the summary line. Every row inherits this: a run that
    never entered a match proves datagram plumbing and time sync only, because
    at a menu the checksum carries no game state at all."""
    cks = record_cks(inst.rec_path)
    scene = in_match_scene(inst)
    if len(cks) < MATCH_WINDOW:
        return [f"{len(cks)} recorded frames: too few to tell a match from a menu"], "no match"
    entry = match_entry(cks) if scene else None
    if entry is None:
        m = moved(cks)
        return ([f"never entered a match ({scene} GS_VS markers, "
                 f"{100 * sum(m) / len(m):.0f}% of {len(cks)} frames moved the checksum; "
                 "a menu barely moves it)"], "no match")
    n = sum(moved(cks[entry:]))
    fails = []
    if n < MATCH_MIN:
        fails.append(f"only {n} frames of match after frame {entry} (want {MATCH_MIN})")
    return fails, f"match at frame {entry}, {n} frames simulated"


def drive_scenes(a, b):
    """Play, leave a match through results, then start a rematch."""
    if not drive_lan(a, b) or not wait_match((a, b)):
        return False
    settle((a, b), 2100)  # require sustained gameplay before ending the game
    press((a,), "Return", 9, 1.0)
    if not press_until((a, b), LRAS, 9, r"online: enter RESULTS", tries=4,
                       each=8.0, on=(a,)):
        return False
    if not press_until((a, b), "Return", 9, r"online: enter CSS", want=2,
                       tries=12, each=5.0):
        return False
    return drive_css(a, b, want=2) and drive_sss(a, b, want=2)


def check_delay(a, b):
    """Auto delay is decided by one side and applied by frame number, so both
    peers must land every change on the same frame (net_sync.c delay_apply).
    A peer that applies one early or late writes its local sample into a
    different ring slot than the other expects, which is the fairness half of
    a desync and the half a checksum never sees. The menu/fight split makes
    this fire several times a match instead of never, so it is worth pinning."""
    applied = [re.findall(r"net: delay (\d+) -> (\d+) at frame (\d+)", i.text()) for i in (a, b)]
    if applied[0] != applied[1]:
        return [f"peers applied different delays: a {applied[0]} b {applied[1]}"]
    return []


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
# snapshot report
# (net_snapshot.c:382).
CSS_RX = re.compile(r"lobby: entering CSS at frame (\d+), seed (\d+)")
PICKS_RX = re.compile(r"sss: picks P1=(-?\d+) P2=(-?\d+) -> (-?\d+)")
CONNECT_RX = re.compile(r"lan: connect (\d+\.\d+\.\d+\.\d+):(\d+) as P(\d)")
STAGE_RX = re.compile(r"\[FileCache\] (?:LOOSE )?HIT: (Gr[A-Za-z0-9]+)\.(?:dat|usd)")
OOM_RX = re.compile(r"net: out of memory for snapshots at frame (\d+), lockstep from here")
SNAP_RX = re.compile(r"net:\s+snapshot take [\d.]+ ms \(max [\d.]+, n (\d+)\)")


def check_scenes(a, b):
    """Require both peers to complete the same frame-exact set flow."""
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
    for scene in ("css", "sss"):
        got = [count(inst, SCENE_FILE[scene]) for inst in (a, b)]
        if min(got) < 1:
            fails.append(f"{scene.upper()} loaded {got[0]}/{got[1]} times (want 1 each)")
    picks = [PICKS_RX.findall(inst.text()) for inst in (a, b)]
    if min(map(len, picks)) < 2:
        fails.append(f"{len(picks[0])}/{len(picks[1])} 'sss: picks' lines (want 2 each: both "
                     "peers' picks have to resolve to one stage)")
    elif picks[0] != picks[1]:
        fails.append(f"SSS picks differ: a {picks[0]} b {picks[1]}")
    transitions = [re.findall(r"online: enter (CSS|SSS|VS|RESULTS) at frame (\d+)",
                              inst.text()) for inst in (a, b)]
    expected = ["CSS", "SSS", "VS", "RESULTS", "CSS", "SSS", "VS"]
    for inst, events in zip((a, b), transitions):
        if [scene for scene, frame in events][:len(expected)] != expected:
            fails.append(f"{inst.name}: incomplete set flow: {events}")
        resolved = re.findall(r"sss: resolved cell (\d+) stage (\d+)", inst.text())
        if not STAGE_RX.search(inst.text()) and not (len(resolved) >= 2 and
                all(0 <= int(cell) < 29 and 2 <= int(stage) <= 32 for cell, stage in resolved)):
            fails.append(f"{inst.name}: no valid resolved stage or stage archive loaded")
    if transitions[0] != transitions[1]:
        fails.append(f"scene transition frames differ: {transitions}")
    return fails


def check_load_stall(a, b):
    """--load-stall: B's game thread parks mid-run while its tx timer keeps
    sending (MELEE_NET_STALL_TEST, net.c). That is a load -- a stage, a
    character, a first-time shader compile on a phone -- not a lost peer, so
    the link must carry it with no reconnect phase at all. "peer silent" is
    already a failure pattern for every row; what this adds is that the
    session must not even be *interrupted*, because a reconnect that is
    entered on a talking peer is the bug that dropped phone<->PC sessions at
    the CSS->match hand-off (silence was timed from the start of the wait
    rather than from the last datagram).

    The stall must also actually have happened: an injection that silently
    did nothing would pass every other assertion in the row."""
    fails = []
    if "net: stall test:" not in b.text():
        fails.append("b never ran the stall injection (MELEE_NET_STALL_TEST did not fire)")
    for inst in (a, b):
        if "interrupted at frame" in inst.text():
            fails.append(f"{inst.name} opened a reconnect phase for a peer that never "
                         "stopped sending")
    return fails


def check_oom(inst, oom_frame):
    """--oom: the counters, not just the log line. A snapshot that cannot be
    taken disables further prediction while preserving earlier rollback
    snapshots, so snapshots must have been taken before the failure
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
    # Snapshot failure must leave earlier rollback snapshots usable. The old
    # INT32_MAX barrier assertion required the very bug this test exercises.
    # No further takes above, and no lost rollback/desync in summarize(),
    # check the fallback's behavior without invalidating that history.
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


def summarize(inst, need_match=True):
    """(fails, one-line summary, stats dict for tools/net_acceptance.py). The
    match precondition is in here, so every row inherits it and none can pass
    from a menu again. need_match=False is for a row whose flow cannot reach
    a match on this build at all (--scenes, see check_scenes): the note still
    says what the checksum stream saw, it just is not a failure."""
    text = inst.text()
    fails = []
    if not re.search(r"net: test done at frame (\d+)", text):
        fails.append("no 'net: test done'")
    # A session that never came up makes every other number in this row
    # meaningless: the instance plays a normal offline game, its checksum
    # stream moves, check_match() sees a match, and the row reads like a
    # netplay result. The usual cause is the previous run's processes still
    # holding the UDP port, which looks from the outside exactly like two
    # peers that went out of sync.
    if re.search(r"net: bind\(\d+\) failed", text):
        fails.append("netplay never started: bind failed (port still in use?)")
    elif not re.search(r"net: rollback with ", text):
        fails.append("netplay never started: no session line")
    if inst.proc.returncode != 0:
        fails.append(f"exit code {inst.proc.returncode}")
    # "peer silent" has to be the whole leaving-netplay line, not a substring:
    # the benign `net: interrupted at frame N (peer silent 3000 ms),
    # reconnecting for up to M ms` that opens the reconnect phase contains the
    # same two words, and matching loosely failed a `--stall` row that had
    # resumed correctly (measured, /tmp/sf_acc "resume 11 s").
    for pat in (r"net: DESYNC", r"peer silent for \d+ ms at frame \d+, leaving netplay",
                r"cannot roll back", r"REPLAY DIVERGED"):
        n = len(re.findall(pat, text))
        if n:
            fails.append(f"{n}x '{pat}'")
    match_fails, note = check_match(inst)
    if need_match:
        fails += match_fails
    st, line = stats_line(inst)
    st["match"] = note
    line += f", {note}"
    d = re.search(r"net: DESYNC.*", text)
    if d:
        line += "\n" + f"[{inst.name}] " + d.group(0)
    return fails, line, st


# STALL_TIMEOUT_MS (net_internal.h) plus room for the loaded machine to
# notice it. 7 s until faf888914 shortened it to 3 s.
PEER_GONE_S = 15


def run_disconnect(args):
    """B SIGKILLed mid-match, no BYE: A must report the peer gone within the
    stall timeout and carry on running instead of hanging. Same return shape
    as run(). MELEE_NET_RECONNECT_MS=0 pins the hard-drop path, MELEE_FPS=1
    gives the per-second line that shows the frame loop is still alive after
    the session is gone (a clean exit by BYE is tools/net_lan_test.py's
    `direct` case; this is the ungraceful half).

    `--disconnect a` kills A instead, i.e. the very instance every assertion
    below is made against. That is the injection this row is validated with:
    it must fail, and fail on "a exited on its own", because a row that
    passes whichever process it kills is not testing a survivor at all."""
    frames = int(args.minutes * 3600) + BOOT_FRAMES
    env = {"MELEE_NET_EXIT_AFTER_FRAMES": str(frames), "MELEE_NET_RECONNECT_MS": "0"}
    shutil.rmtree(args.work, ignore_errors=True)
    os.makedirs(args.work)
    a = Instance("a", args.exe, args.disc, args.work, args.port, args.port + 1, env, False)
    b = Instance("b", args.exe, args.disc, args.work, args.port + 1, args.port, env, False)
    victim = a if args.disconnect == "a" else b
    print(f"net_test: disconnect (killing {victim.name}), pids={a.proc.pid},{b.proc.pid} "
          f"logs={args.work}", flush=True)
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
            stats = STATS_RX.findall(victim.text())
            v_frame = stats[-1][0] if stats else "?"
            victim.kill()  # SIGKILL: no BYE, the survivor has to time the peer out
            t0 = time.time()
            print(f"net_test: SIGKILLed {victim.name} around frame {v_frame}", flush=True)
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
        if inst is victim:
            line += ", SIGKILLed mid-match on purpose"
        print(line)
        if f:
            print(f"[{inst.name}] FAIL: " + ", ".join(f))
        results.append((inst.name, f, line, st))
    return not fails, results


# The reconnect phase (net.c:639-832): a silence past STALL_TIMEOUT_MS opens a
# bounded window instead of ending the session, and the frames predicted
# across the interruption roll back as usual when the peer's input arrives.
# MELEE_NET_RECONNECT_MS bounds the window (default 3 s, 0 = the hard drop
# the `disconnect` row covers). SIGSTOP is the honest interruption to test it
# with: the stopped instance keeps its socket, so the datagrams it missed are
# queued in its receive buffer when it continues, which is exactly what a
# peer whose Wi-Fi came back sees. A killed process cannot resume at all.
STALL_TIMEOUT_S = 3  # STALL_TIMEOUT_MS, src/pc/net_internal.h:114
RESUME_SLACK_S = 25  # room for a loaded machine to notice and report


def stall_expires(stall_s, window_ms):
    """Whether an interruption of `stall_s` outlives the reconnect phase: the
    stall timeout opens the phase and the window bounds it (net.c's
    resume_begin / resume_poll). Its own function because it is the seam the
    row is injection-validated through: patch it to the wrong answer and the
    row must fail, which is the only way to prove the assertions below are
    reading the real window and not just "a long stall kills the session"."""
    return stall_s > STALL_TIMEOUT_S + window_ms / 1000.0


def run_stall(args):
    """--stall SECONDS: B SIGSTOPped mid-match for SECONDS, then SIGCONT.
    Which half of the contract the row asserts follows from the arithmetic,
    not from a second flag: a stall inside STALL_TIMEOUT_S + the reconnect
    window must be survived (interrupted, then resumed, then both peers run
    on to their exit frame in sync), and a stall past it must expire and end
    both sessions with status 2. Same return shape as run().

    Both halves are needed to pin the window down: "survived" alone would
    also pass a build that never times anything out, and "expired" alone
    would also pass a build with no resume phase at all."""
    window_s = args.reconnect_ms / 1000.0
    expire = stall_expires(args.stall, args.reconnect_ms)
    frames = int(args.minutes * 3600) + BOOT_FRAMES
    env = {"MELEE_NET_EXIT_AFTER_FRAMES": str(frames),
           "MELEE_NET_RECONNECT_MS": str(args.reconnect_ms)}
    shutil.rmtree(args.work, ignore_errors=True)
    os.makedirs(args.work)
    a = Instance("a", args.exe, args.disc, args.work, args.port, args.port + 1, env, False)
    b = Instance("b", args.exe, args.disc, args.work, args.port + 1, args.port, env, False)
    print(f"net_test: stall {args.stall} s with a {window_s:.0f} s window -> must "
          f"{'EXPIRE' if expire else 'RESUME'}, pids={a.proc.pid},{b.proc.pid} "
          f"logs={args.work}", flush=True)
    fails = []
    work = None
    stopped = False
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
            b.proc.send_signal(signal.SIGSTOP)
            stopped = True
            t0 = time.time()
            print(f"net_test: SIGSTOPped b at {time.strftime('%H:%M:%S')}", flush=True)
            # Every wait here is capped by the time left of the stall, and the
            # SIGCONT happens at the stall duration whatever was or was not
            # logged: waiting for the expiry line BEFORE continuing b made the
            # stall as long as the assertion needed, so the expectation became
            # self-fulfilling and the injection that forces the wrong
            # expectation passed (measured, /tmp/sf_stall_expiry: an 11 s stall
            # ran 22.3 s and duly expired).
            def left():
                return max(0.0, args.stall - (time.time() - t0))

            if not a.wait_log(r"net: interrupted at frame \d+ \(peer silent \d+ ms\), "
                              r"reconnecting for up to \d+ ms", left()):
                fails.append(f"a never opened a reconnect phase within {args.stall} s of the "
                             "SIGSTOP")
            else:
                print(f"net_test: a interrupted {time.time() - t0:.1f} s after the SIGSTOP",
                      flush=True)
            if expire and not a.wait_log(r"net: resume window of \d+ ms expired at frame \d+",
                                         left()):
                fails.append(f"a's {window_s:.0f} s resume window never expired inside the "
                             f"{args.stall} s stall")
            time.sleep(left())
            b.proc.send_signal(signal.SIGCONT)
            stopped = False
            print(f"net_test: SIGCONTed b after {time.time() - t0:.1f} s", flush=True)
            if expire:
                # A ended the session on its own; B learns it from A's BYE
                # (net.c:539-545) or, if that datagram was dropped while it
                # was stopped, from its own expiry. Either way: status 2.
                for inst in (a, b):
                    if not inst.wait_log(r"net: disconnected at frame \d+ \(status 2\)",
                                         window_s + RESUME_SLACK_S):
                        fails.append(f"{inst.name} has no 'net: disconnected ... (status 2)' "
                                     "after the window expired")
                if not re.search(r"net: peer silent for \d+ ms at frame \d+, leaving netplay",
                                 a.text()):
                    fails.append("a never logged 'peer silent ... leaving netplay': the expiry "
                                 "did not fall through to the timeout path")
                time.sleep(10)
                if a.proc.poll() is not None:
                    fails.append(f"a exited on its own with code {a.proc.returncode}")
                elif a.text().count("\nfps ") - fps_before < 5:
                    fails.append("a stopped printing its per-second fps line: the frame loop "
                                 "hung after the expiry")
                for inst in (a, b):
                    for pat in ("net: DESYNC", "cannot roll back"):
                        if pat in inst.text():
                            fails.append(f"{inst.name} logged '{pat}'")
            else:
                if not a.wait_log(r"net: resumed at frame \d+ after [\d.]+ s", RESUME_SLACK_S):
                    fails.append(f"a never resumed within {RESUME_SLACK_S} s of the SIGCONT")
                for pat in (r"net: resume window of \d+ ms expired",
                            r"net: disconnected at frame"):
                    if re.search(pat, a.text() + b.text()):
                        fails.append(f"the session ended anyway ('{pat}')")
                # Both peers now have to finish the run they were in the
                # middle of: the resume is only proven by the frames after it.
                deadline = time.time() + frames / 12.0 + 60
                while time.time() < deadline and (a.proc.poll() is None or
                                                  b.proc.poll() is None):
                    time.sleep(1)
                    if any(t in a.text() + b.text() for t in
                           ("net: DESYNC", "net: disconnected", "cannot roll back")):
                        break
        r = re.findall(r"net: resumed at frame (\d+) after ([\d.]+) s", a.text())
        if r:
            print(f"net_test: a resumed at frame {r[-1][0]} after {r[-1][1]} s", flush=True)
    finally:
        if work is not None:
            work.done()
        if stopped:
            b.proc.send_signal(signal.SIGCONT)  # SIGKILL lands either way, but do not
        a.kill(20)                              # leave a stopped melee behind on a crash
        b.kill(20)
    results = []
    for inst in (a, b):
        if expire:  # no self-exit: net.active is false, so nothing counts frames to the end
            st, line = stats_line(inst)
            entry = match_entry(record_cks(inst.rec_path))
            st["match"] = f"match at frame {entry}" if entry is not None else "no match"
            line += f", {st['match']}"
            f = []
        else:
            f, line, st = summarize(inst)
        if inst is a:
            f = f + fails
        print(line)
        if f:
            print(f"[{inst.name}] FAIL: " + ", ".join(f))
        results.append((inst.name, f, line, st))
    return not any(f for _, f, _, _ in results), results


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
    ap.add_argument("--disconnect", nargs="?", const="b", choices=("a", "b"), default=None,
                    metavar="WHO",
                    help="SIGKILL B mid-match; assert A times the peer out and keeps running. "
                         "'--disconnect a' kills the instance the row asserts on instead: the "
                         "injection that proves the row is not passing on a dead process")
    ap.add_argument("--stall", type=float, default=0, metavar="SECONDS",
                    help="SIGSTOP B mid-match for SECONDS, then SIGCONT: inside the reconnect "
                         "window the session must survive, past it it must expire with status 2")
    ap.add_argument("--state-log", action="store_true",
                    help="write per-frame state dumps to <work>/a.state and b.state")
    ap.add_argument("--cold-cache", action="store_true",
                    help="disable archive prewarm on A and enable it on B; delay A's DVD "
                         "reads by 1.5 ms. Loose-file cache reads bypass the DVD delay, "
                         "so this does not guarantee different scene-entry frames")
    ap.add_argument("--load-stall", type=float, default=0, metavar="SECONDS",
                    help="park B's game thread for SECONDS mid-run while its sender keeps "
                         "running (a load, not a lost peer): the session must carry it with no "
                         "reconnect phase, however long it is")
    ap.add_argument("--reconnect-ms", type=int, default=3000,
                    help="MELEE_NET_RECONNECT_MS for --stall (net.c's own default is 3000)")
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
    if args.stall:
        return run_stall(args)
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
    if args.cold_cache:
        # Exercise prewarm asymmetry, even if the shell disabled it globally.
        # OS caches remain warm; loose-file reads bypass the DVD delay below.
        # Verify actual scene transitions instead of assuming different timing.
        sim_a["MELEE_PREWARM"] = "0"
        sim["MELEE_PREWARM"] = "1"
        # Aurora's DVD readFromHandle hook delays reads that reach that layer.
        sim_a["MELEE_DISC_READ_DELAY_US"] = "1500"
    if args.load_stall:
        # B only, and well past boot so the session is established: the wait
        # this exercises is the one a scene hand-off makes, not the connect.
        sim["MELEE_NET_STALL_TEST"] = f"{LOAD_STALL_FRAME}:{int(args.load_stall * 1000)}"
    shutil.rmtree(args.work, ignore_errors=True)
    os.makedirs(args.work)
    if args.state_log:
        # Per-frame state dumps beside the logs, so a DESYNC can be diffed
        # field by field (src/pc/net_snapshot.c).
        sim_a["MELEE_NET_STATE_LOG"] = os.path.join(args.work, "a.state")
        sim["MELEE_NET_STATE_LOG"] = os.path.join(args.work, "b.state")
    wait_port_free(args.port)
    wait_port_free(args.port + 1)
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
        # Terminal markers only. A reconnect phase that OPENS is not terminal
        # (it may resume), so this matches the leaving-netplay line rather
        # than the two words "peer silent", which the interrupted line shares:
        # a 7 s hiccup on a loaded machine must not end a 60-minute soak.
        term = (r"net: DESYNC", r"peer silent for \d+ ms at frame \d+, leaving netplay",
                r"net: disconnected", r"cannot roll back")
        # A deliberate load stall is exactly "no frame progress", so the
        # watchdog has to outlast it or the row kills the run it is measuring.
        quiet_budget = 25 + args.load_stall
        last_frame, last_progress = -1, time.time()
        while time.time() < deadline and (a.proc.poll() is None or b.proc.poll() is None):
            time.sleep(1)
            texts = a.text() + b.text()
            if any(re.search(t, texts) for t in term):
                break  # let summarize() report it; do not burn the whole budget
            fr = max([int(x) for x in re.findall(r"net: frame (\d+)", texts)] or [-1])
            if fr > last_frame:
                last_frame, last_progress = fr, time.time()
            elif time.time() - last_progress > quiet_budget and last_frame >= 0:
                print(f"net_test: no frame progress for {quiet_budget:.0f} s, giving up",
                      flush=True)
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
    extra = check_entry(a, b) + check_delay(a, b)
    extra += check_scenes(a, b) if args.scenes else check_oom(a, args.oom) if args.oom else []
    if args.load_stall:
        extra += check_load_stall(a, b)
    results = []
    for inst in (a, b):
        fails, line, st = summarize(inst, need_match=True)
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
