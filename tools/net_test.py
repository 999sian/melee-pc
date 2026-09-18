#!/usr/bin/env python3
"""Netplay integration test: two instances on one machine, a match, then asserts.

    tools/net_test.py [--loss PCT] [--delay MS] [--rxdelay MS] [--jitter] [--reorder]
                      [--burst] [--dup] [--minutes N] [--lan] [--fuzz] [--exe build/melee]
                      [--disc ../melee.ciso] [--port 42050] [--work /tmp/net_test]

Direct mode (default): MELEE_NET=127.0.0.1:<peer> + MELEE_DEBUG_VS=1, Start
pressed twice through MELEE_KEY_FIFO (title, then straight into Link vs Mario).
--lan: both instances walk the real menus into the LAN lobby (VS Mode -> ONLINE
-> LAN PLAY), Start on one elects the host, then CSS/SSS by fixed-hold cursor
moves; needs the shared LAN free (coordinate with anyone else in the lobby).
--fuzz: tools/net_fuzz.py hammers instance A's port during the match.

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
import time

BOOT_FRAMES = 1800  # ~30 s of title/menus before the match, counted by net.c too
SIM_ENV = {  # CLI flag -> (env knob, value) in src/pc/net.c's link simulator
    "jitter": ("MELEE_NET_SIM_JITTER_MS", "20"),
    "reorder": ("MELEE_NET_SIM_REORDER", "10"),
    "burst": ("MELEE_NET_SIM_BURST", "5"),
    "dup": ("MELEE_NET_SIM_DUP", "10"),
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
        cache = os.path.join(work, f"{name}.cache")
        os.makedirs(cache)
        e = dict(os.environ)
        e.update({
            "SDL_VIDEO_DRIVER": "x11",
            "MELEE_VSYNC": "0",
            "MELEE_NET_PORT": str(port),
            "MELEE_CACHE_DIR": cache,
            "MELEE_WINDOW_TITLE": f"melee-net-{name}",
            "MELEE_KEY_FIFO": self.fifo,
            "MELEE_SEED": "7",
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


def drive_direct(a, b):
    """MELEE_DEBUG_VS: Start in the opening movie (-> title), Start at the title
    (-> Link vs Mario). Paced by net.c's frame-600 stats lines, not wall time:
    lockstep through a simulated link runs well below 60 fps."""
    for frame in (600, 1200):
        for inst in (a, b):
            inst.wait_log(rf"net: frame {frame},", 120)
        for inst in (a, b):
            inst.key("Return 150")


# LAN: title -> main menu -> VS Mode -> ONLINE (Up wraps to the 6th slot) ->
# LAN PLAY (first entry). Then Start on one instance elects it host.
LAN_MENU = ["Return 150", "Return 150", "Down 120", "X 120", "Up 120", "X 120", "X 120"]


def both(a, b, key, pause):
    for inst in (a, b):
        inst.key(key)
    time.sleep(pause)


def drive_lan(a, b):
    time.sleep(14)
    for i, k in enumerate(LAN_MENU):
        both(a, b, k, 5 if i < 2 else 2.5)
    if not (a.wait_log(r"lobby: 2 players found", 20) and b.wait_log(r"lobby: 2 players found", 20)):
        return False
    a.key("Return 150")
    if not (a.wait_log(r"lobby: entering CSS", 30) and b.wait_log(r"lobby: entering CSS", 30)):
        return False
    time.sleep(6)
    # CSS: hold Up brings the hand out and rides it to the top edge; a short
    # Down lands on the first portrait row (both port columns sit under it),
    # A picks. Start (host) once both have picked -> SSS.
    both(a, b, "Up 1500", 2)
    both(a, b, "Down 350", 1)
    both(a, b, "X 120", 2)
    a.key("Return 150")
    time.sleep(6)
    # SSS: pin the cursor into the bottom-left corner (deterministic origin),
    # step out onto a stage, A picks; both picks resolve via the shared seed.
    both(a, b, "Left+Down 1200", 1.8)
    both(a, b, "Right+Up 260", 1)
    both(a, b, "X 120", 0)
    return a.wait_log(r"sss: picks", 30) and b.wait_log(r"sss: picks", 30)


# "net: frame N, rollbacks N (max depth N, lost N), stalls N (worst X ms), skips N,
# advances N, ping N ms (avg X, min Y, max Z, jitter J), loss L% (...)"; the
# ping parenthesis and loss are optional so older logs still parse.
STATS_RX = re.compile(r"net: frame (\d+), rollbacks (\d+) \(max depth (\d+), lost (\d+)\), "
                      r"stalls (\d+) \(worst ([\d.]+) ms\), skips (\d+), advances (\d+), "
                      r"ping (\d+) ms(?: \(avg [\d.]+, min \d+, max \d+, jitter ([\d.]+)\))?"
                      r"(?:, loss (\d+)%)?")


def summarize(inst):
    """(fails, one-line summary, stats dict for tools/net_acceptance.py)."""
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
    d = re.search(r"net: DESYNC.*", text)
    if d:
        line += "\n" + f"[{inst.name}] " + d.group(0)
    return fails, line, st


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

    shutil.rmtree(args.work, ignore_errors=True)
    os.makedirs(args.work)
    a = Instance("a", args.exe, args.disc, args.work, args.port, args.port + 1, sim, args.lan)
    b = Instance("b", args.exe, args.disc, args.work, args.port + 1, args.port, sim, args.lan)
    print(f"net_test: {'lan' if args.lan else 'direct'} loss={args.loss}% delay={args.delay}ms "
          f"rxdelay={args.rxdelay}ms {' '.join(k for k in SIM_ENV if getattr(args, k))} "
          f"minutes={args.minutes} frames={frames} pids={a.proc.pid},{b.proc.pid} "
          f"logs={args.work}", flush=True)
    ok = True
    try:
        if args.lan:
            ok = drive_lan(a, b)
            if not ok:
                print("net_test: LAN drive failed (see logs)", flush=True)
        else:
            drive_direct(a, b)
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
        # An instance that reached its exit frame (or took its peer's BYE that
        # close to it) is already tearing the GPU down, which takes seconds;
        # SIGKILL only what is still running after that.
        a.kill(20)
        b.kill(20)
        if a.proc.returncode == -9 or b.proc.returncode == -9:
            print("net_test: killed a run that would not exit (per-instance FAIL below)",
                  flush=True)
    results = []
    for inst in (a, b):
        fails, line, st = summarize(inst)
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
