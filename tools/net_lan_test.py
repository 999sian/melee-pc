#!/usr/bin/env python3
"""LAN lobby tests on the menu-less fixtures (src/pc/vi.c): two instances on one
machine, no key driving, log assertions only.

    tools/net_lan_test.py [--only start,direct,lost,host_dies] [--exe build/melee]
                          [--disc ../melee.ciso] [--port 42090] [--work /tmp/net_lan_test]

  start     MELEE_LAN_TEST=host on both: simultaneous Start. Exactly one
            "we host as P1", exactly one "joining as P2", "lan: match start" on both.
  direct    MELEE_LAN_DIRECT both ways, no discovery: match start, then the
            instance with the smaller MELEE_NET_EXIT_AFTER_FRAMES exits and the
            other logs "net: peer left" (BYE) within 2 s, never "peer silent".
  lost      MELEE_LAN_TEST=1 on both, SIGKILL one mid-lobby: the other logs
            "lan: lost" within 6 s (announce TTL, net_lan.c LOST_NS).
  host_dies MELEE_LAN_TEST=host on both, SIGKILL the elected host while the
            guest is still connecting: the guest logs "lan: failed:" within 20 s.

Each case gets fresh ports (--port + 2n, +1), cache dirs and logs under
<work>/<case>/. Needs the shared LAN free of other melee-pc lobbies.
Prints PASS/FAIL per case; exit 1 if any failed.
"""
import argparse
import os
import re
import shutil
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from net_test import Instance  # noqa: E402

LOBBY_TIMEOUT = 90  # boot + title + discovery; the host fixture starts at frame 300


def ports(args, case):
    pa = args.port + 2 * list(CASES).index(case)
    return pa, pa + 1


def pair(args, case, env_a, env_b):
    work = os.path.join(args.work, case)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    pa, pb = ports(args, case)
    a = Instance("a", args.exe, args.disc, work, pa, pb, env_a, True)
    b = Instance("b", args.exe, args.disc, work, pb, pa, env_b, True)
    print(f"[{case}] ports {pa},{pb} pids {a.proc.pid},{b.proc.pid} logs {work}", flush=True)
    return a, b


def count(inst, pat):
    return len(re.findall(pat, inst.text()))


def case_start(args):
    """(a) simultaneous Start on both."""
    env = {"MELEE_LAN_TEST": "host"}
    a, b = pair(args, "start", env, env)
    try:
        if not (a.wait_log(r"lan: match start", LOBBY_TIMEOUT) and b.wait_log(r"lan: match start", 30)):
            return "no 'lan: match start' on both"
        hosts = count(a, r"we host as P1") + count(b, r"we host as P1")
        guests = count(a, r"joining as P2") + count(b, r"joining as P2")
        if (hosts, guests) != (1, 1):
            return f"{hosts}x 'we host as P1', {guests}x 'joining as P2' (want 1 and 1)"
        return None
    finally:
        a.kill()
        b.kill()


def case_direct(args):
    """(b) no discovery; the early exiter's BYE reaches the other."""
    pa, pb = ports(args, "direct")
    a, b = pair(args, "direct",
                {"MELEE_LAN_DIRECT": f"127.0.0.1:{pb}", "MELEE_NET_EXIT_AFTER_FRAMES": "900"},
                {"MELEE_LAN_DIRECT": f"127.0.0.1:{pa}", "MELEE_NET_EXIT_AFTER_FRAMES": "3000"})
    try:
        if not (a.wait_log(r"lan: match start", LOBBY_TIMEOUT) and b.wait_log(r"lan: match start", 30)):
            return "no 'lan: match start' on both"
        if not a.wait_log(r"net: test done at frame", 60):
            return "A never reached MELEE_NET_EXIT_AFTER_FRAMES"
        a.proc.wait(10)
        if a.proc.returncode != 0:
            return f"A exit code {a.proc.returncode}"
        if not b.wait_log(r"net: peer left", 2):
            return "B has no 'net: peer left' within 2 s of A's exit"
        if count(b, r"peer silent"):
            return "B logged 'peer silent' (BYE was not honoured)"
        return None
    finally:
        a.kill()
        b.kill()


def case_lost(args):
    """(c) a lobby peer vanishes without a goodbye."""
    env = {"MELEE_LAN_TEST": "1"}
    a, b = pair(args, "lost", env, env)
    try:
        if not (a.wait_log(r"lan: found", LOBBY_TIMEOUT) and b.wait_log(r"lan: found", 30)):
            return "the two lobbies never found each other"
        b.kill()
        if not a.wait_log(r"lan: lost", 6):
            return "A has no 'lan: lost' within 6 s of B's SIGKILL"
        return None
    finally:
        a.kill()
        b.kill()


def case_host_dies(args):
    """(d) host killed while the guest is connecting. The rx delay on both
    keeps the guest in "connecting" (it hears the host's first packets 3 s
    late, before RULES) so the kill lands mid-handshake instead of in a race
    with a 60 ms handshake; the guest then times out the silent host
    (7 s, net.c STALL_TIMEOUT_MS) and the lobby reports the failure."""
    env = {"MELEE_LAN_TEST": "host", "MELEE_NET_SIM_DELAY_RX_MS": "3000"}
    a, b = pair(args, "host_dies", env, env)
    try:
        deadline = time.time() + LOBBY_TIMEOUT
        host = None
        while time.time() < deadline and host is None:
            for inst, other in ((a, b), (b, a)):
                if count(inst, r"we host as P1"):
                    host, guest = inst, other
            time.sleep(0.1)
        if host is None:
            return "no host election"
        if not guest.wait_log(r"lan: connect .* as P2", 20):
            return f"{guest.name} never connected as P2"
        time.sleep(1)  # the host has been sending inputs for a while now
        if count(guest, r"lan: match start"):
            return f"{guest.name} started the match before the kill (rx delay ineffective)"
        host.kill()
        t0 = time.time()
        if not guest.wait_log(r"lan: failed:", 20):
            return f"{guest.name} has no 'lan: failed:' within 20 s of the host's SIGKILL"
        print(f"[host_dies] {guest.name} failed {time.time() - t0:.1f} s after the kill: "
              + re.search(r"lan: failed:.*", guest.text()).group(0).strip(), flush=True)
        return None
    finally:
        a.kill()
        b.kill()


CASES = {"start": case_start, "direct": case_direct, "lost": case_lost, "host_dies": case_host_dies}


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--only", default=",".join(CASES), help="comma-separated case names")
    ap.add_argument("--exe", default=os.path.join(here, "..", "build", "melee"))
    ap.add_argument("--disc", default=os.path.join(here, "..", "..", "melee.ciso"))
    ap.add_argument("--port", type=int, default=42090, help="first UDP port; each case uses 2")
    ap.add_argument("--work", default="/tmp/net_lan_test")
    args = ap.parse_args()
    ok = True
    for name in args.only.split(","):
        why = CASES[name](args)
        print(f"[{name}] {'PASS' if why is None else 'FAIL: ' + why}", flush=True)
        ok = ok and why is None
    print("net_lan_test: " + ("PASS" if ok else "FAIL"))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
