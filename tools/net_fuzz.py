#!/usr/bin/env python3
"""Throw malformed UDP datagrams at a netplay instance and require it to shrug.

    tools/net_fuzz.py --port P --pid PID --log LOG [--seconds 30] [--bind PORT]
                      [--session HEX --player N --version N]
    tools/net_fuzz.py --launch [--seconds 30] [--session auto] [--exe build/melee]
                      [--disc ../melee.ciso]

Datagrams: random/known magic bytes, bogus version and session ids, counts up to
255, INT32_MIN/MAX frames, reliable len 0..65535 with truncated or oversized
bodies, every length 0..1500. --bind sends from a fixed source port; --launch
starts one instance with MELEE_NET pointed at that port so the garbage passes
the peer-address check and reaches the parser (the instance idles at frame 0
waiting for its "peer"). Pass = process alive afterwards, no DESYNC / "peer
silent" / "peer left" / crash in the log; rejected-packet lines
("net: dropped ..." / "net: incompatible version ...") are counted and shown.

--session HEX (--launch: "auto" reads it from the instance's "net: rollback
with ... session" line) crafts every datagram with that session id, --player
(the remote player, default 1) and --version (default: PC_NET_PROTO_VERSION as
read from src/pc/net.h), so they pass the
header gate and the frame-range/count validation of input packets, acks and
the reliable channel is what gets hit: first/count/newest at the RING/2
boundary around the instance's frame, INT32 extremes, count 0/16/17/255,
bodies one byte short. No BYE and no foreign version in this mode (either
ends the session and the rest would be inert). We also play peer: the
instance's own input packets tell us its newest frame and we feed neutral
pads up to it, so it keeps ticking instead of stalling out.
"""
import argparse
import os
import random
import re
import socket
import struct
import sys
import time

BAD = ("net: DESYNC", "peer silent", "peer left", "cannot roll back", "Segmentation",
       "FATAL", "abort", "Assertion")
REJECT = ("net: dropped", "net: peer speaks protocol")


def datagram(rng):
    """Wire layout (src/pc/net.c, big-endian): Hdr = magic u8, version u8,
    session u32, player u8. The session id is unknowable here, so most
    datagrams die at the header check; 0 exercises the guest's learn path."""
    kind = rng.randrange(9)
    magic = rng.choice(b"MARKMARKB" + bytes(range(256)))
    version = rng.choice([VERSION, VERSION, VERSION, 0, 1, 255, rng.randrange(256)])
    session = rng.choice([0, 0, 1, 0xFFFFFFFF, rng.getrandbits(32)])
    player = rng.choice([0, 1, 1, 2, 255])
    hdr = struct.pack(">BBIB", magic, version, session, player)
    i32 = lambda: rng.choice([0, -1, 1, 2**31 - 1, -2**31, rng.randrange(-2**31, 2**31)])
    if kind == 0:  # input packet, any count/frames, then maybe truncated
        count = rng.choice([0, 1, 16, 17, 128, 255])
        body = hdr + struct.pack(">HiiiIB", rng.getrandbits(16), i32(), i32(), i32(),
                                 rng.getrandbits(32), count) + rng.randbytes(8 * rng.randrange(0, 20))
    elif kind == 1:  # ack
        body = hdr + struct.pack(ACK_BODY, rng.getrandbits(16), i32())
    elif kind == 2:  # reliable: len field vs. real length disagree
        ln = rng.choice([0, 1, 255, 256, 257, 1024, 65535, rng.randrange(65536)])
        body = hdr + struct.pack(">BBH", rng.randrange(256), rng.randrange(256), ln)
        body += rng.randbytes(rng.choice([0, ln % 300, 256, 1400]))
    elif kind == 3:  # reliable ack / bye
        body = hdr + struct.pack(">B", rng.randrange(256))
    elif kind == 4:  # header with garbage body
        body = hdr + rng.randbytes(rng.randrange(0, 24))
    elif kind == 5:  # pre-version layout: magic, player, ... (must be dropped)
        body = struct.pack("<BBiiiIIB", magic, player, i32(), i32(), i32(), rng.getrandbits(32),
                           rng.getrandbits(32), 16) + rng.randbytes(128)
    else:  # pure noise of every size
        body = rng.randbytes(rng.choice([0, 1, 2, 3, rng.randrange(1500)]))
    if len(body) > 1472:
        body = body[:1472]
    return body


INT_MAX, INT_MIN = 2**31 - 1, -2**31
# src/pc/net_internal.h: Packet = Hdr + seq u16, newest, first, ck_frame, ck, count;
# Ack = Hdr + seq u16, frame. Read the version the game speaks out of net.h so a
# wire bump cannot leave this sending datagrams that die at the version gate.
PACKET_HDR = ">BBIBHiiiIB"
ACK_BODY = ">Hi"


def wire_version(default=3):
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "pc", "net.h")
    try:
        with open(path) as f:
            m = re.search(r"define PC_NET_PROTO_VERSION\s+(\d+)", f.read())
        return int(m.group(1)) if m else default
    except OSError:
        return default


VERSION = wire_version()


def datagram_session(rng, session, player, version, base, extremes=True):
    """Valid header; bodies at the validation edges of on_inputs()/on_ack()/
    on_rel() (src/pc/net.c). `base` is the instance's newest frame from its
    last input packet (its s_frame + delay), so base + 32 straddles the
    "newest > s_frame + RING/2" cut. Accepted packets carry ck_frame -1 (no
    checksum, no DESYNC) and neutral pads.

    `extremes` covers the INT32 ends of the ack frame. They are off for the
    first half of a run because they MASK the subtler poison: an INT_MAX ack
    overflows s_last_acked + 1 into the negative clamp, which hands
    send_inputs() a sane window again, while a plain base + 1000 ack silently
    starves the peer for 1000 frames. One poison must not hide the other."""
    hdr = lambda magic: struct.pack(">BBIB", magic, version, session, player)
    kind = rng.randrange(10)
    if kind < 5:  # input packet
        edge = rng.randrange(14)
        ck_frame = -1
        if edge == 0:
            first, count, newest = 0, 0, 0
        elif edge == 1:  # around the RING/2 window, one frame at a time
            newest = base + rng.choice([28, 29, 30, 31, 32, 33, 34, 40, 63, 64, 65])
            first, count = newest, 1
        elif edge == 2:  # full redundancy ending at the window edge
            newest = base + rng.choice([30, 31, 32, 33])
            first, count = newest - 15, 16
        elif edge == 3:  # count clamped to 16 by the parser
            first = max(0, base - 15)
            count, newest = rng.choice([17, 128, 255]), base
        elif edge == 4:  # first < 0
            first, count, newest = rng.choice([-1, INT_MIN]), 1, 0
        elif edge == 5:  # first + count - 1 > newest
            first, count, newest = 0, 2, 0
        elif edge == 6:  # INT32_MAX newest
            first, count, newest = INT_MAX, 1, INT_MAX
        elif edge == 7:  # first + count - 1 overflows int32 in the parser
            first, count, newest = INT_MAX, 16, INT_MAX
        elif edge == 8:
            first, count, newest = INT_MAX - 15, 16, INT_MAX
        elif edge == 9:  # newest far ahead, frames fine
            first, count, newest = 0, 16, INT_MAX
        elif edge == 10:  # ck_frame below -1, otherwise valid
            first, count, newest = max(0, base - 15), 16, base
            ck_frame = rng.choice([-2, INT_MIN])
        elif edge == 11:  # ck_frame far ahead: stored, never confirmed
            first, count, newest = max(0, base - 15), 16, base
            ck_frame = INT_MAX
        elif edge == 12:  # newest < 0 with sane first/count
            first, count, newest = 0, 1, rng.choice([-1, INT_MIN])
        else:  # contiguous with what we fed, past the acceptance window
            first = max(0, base - 3)
            count = 16
            newest = first + count - 1 + rng.choice([0, 16, 17, 32])
        body = hdr(ord("M")) + struct.pack(">HiiiIB", rng.getrandbits(16), newest, first, ck_frame,
                                           rng.getrandbits(32), count & 0xFF)
        body += bytes(8 * min(count & 0xFF, 16))  # what the parser expects for that count
        trim = rng.choice([0, 0, 0, -1, 1, 8])   # one short, one long, one pad long
        body = body[:trim] if trim < 0 else body + bytes(trim)
    elif kind < 7:  # ack: just past what the instance sent, then the INT32 ends
        # No honest ack (frame == base) in the first half: that one legitimately
        # leaves the instance nothing to send, which is indistinguishable from
        # the starvation the pad floor in run() looks for.
        frames = [-1, 0, base + 1, base + 16, base + 1000]
        if extremes:
            frames += [base, INT_MAX, INT_MIN]
        body = hdr(ord("A")) + struct.pack(ACK_BODY, rng.getrandbits(16), rng.choice(frames))
        if rng.randrange(4) == 0:
            body = body[:-1]
    elif kind < 9:  # reliable: handshake types, user types, len vs body
        ln = rng.choice([0, 1, 255, 256, 257, 1024, 65535])
        body = hdr(ord("R")) + struct.pack(">BBH", rng.randrange(256),
                                           rng.choice([0, 1, 2, 3, 0x10, 0x11, 0x12, 0xFF]), ln)
        body += rng.randbytes(rng.choice([0, min(ln, 256), min(ln, 256) - 1, 256, 257, 1400]) % 1401)
    else:  # reliable ack, any seq, sometimes short/long
        body = hdr(ord("K")) + struct.pack(">B", rng.randrange(256)) + bytes(rng.choice([0, 0, 1]))
        if rng.randrange(4) == 0:
            body = body[:-1]
    return body[:1472]


def feed(sock, port, session, player, version, newest):
    """Play peer: neutral pads for frames up to `newest`, all ck_frame -1."""
    first = max(0, newest - 15)
    count = newest - first + 1
    pk = struct.pack(PACKET_HDR, ord("M"), version, session, player, newest & 0xFFFF, newest,
                     first, -1, 0, count) + bytes(8 * count)
    sock.sendto(pk, ("127.0.0.1", port))


def peer_newest(sock, our_player):
    """Drain what the instance sent us: (largest `newest` of its 'M' packets,
    pads carried). A session that keeps its frame counter moving but stops
    putting pads in its packets starves its peer just as thoroughly as
    silence, so both are reported."""
    best, pads = -1, 0
    while True:
        try:
            d, _ = sock.recvfrom(2048)
        except BlockingIOError:
            return best, pads
        except OSError:
            return best, pads  # ICMP unreachable
        if len(d) >= 26 and d[0] == ord("M") and d[6] != our_player:
            best = max(best, struct.unpack_from(">i", d, 9)[0])  # past Hdr(7) + seq(2)
            pads += d[25]  # count: Hdr(7) seq(2) newest/first/ck_frame(12) ck(4)


def alive(pid):
    try:
        with open(f"/proc/{pid}/status") as f:
            return "zombie" not in f.read()
    except OSError:
        return False


def run(port, pid, log_path, seconds=30, bind_port=None, seed=1, session=None, player=1,
        version=None):
    version = VERSION if version is None else version
    rng = random.Random(seed)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    if bind_port:
        s.bind(("127.0.0.1", bind_port))
    s.setblocking(False)
    start = os.path.getsize(log_path)
    t0 = time.time()
    n = 0
    base = fed = 0  # session mode: the instance's newest frame; what we fed it
    pads, mid = 0, None  # pads it sent back; (pads, fed) when the extremes start
    while time.time() - t0 < seconds:
        extremes = session is not None and time.time() - t0 > seconds / 2
        if extremes and mid is None:
            mid = (pads, fed)
        try:
            if session is None:
                s.sendto(datagram(rng), ("127.0.0.1", port))
            else:
                newest, got = peer_newest(s, player)
                base, pads = max(base, newest), pads + got
                if base > fed:
                    feed(s, port, session, player, version, base)
                    fed = base
                s.sendto(datagram_session(rng, session, player, version, base, extremes),
                         ("127.0.0.1", port))
        except OSError:
            pass  # ICMP port unreachable surfaces here if the target died; alive() decides
        n += 1
        if n % 20 == 0:
            time.sleep(0.005)  # ~4000 pkt/s
    time.sleep(1)
    ok = alive(pid)
    with open(log_path, "rb") as f:
        f.seek(start)
        new = f.read().decode("utf-8", "replace").splitlines()
    rejects = [l for l in new if any(r in l for r in REJECT)]
    bad = [l for l in new if any(b in l for b in BAD)]
    print(f"net_fuzz: {n} datagrams in {seconds} s to :{port}"
          f"{f' session {session:08x} player {player} v{version}, fed to frame {fed}, {pads} pads back' if session is not None else ''}"
          f", pid {pid} {'alive' if ok else 'DEAD'}, {len(rejects)} reject lines, {len(bad)} bad lines")
    for l in rejects[:6] + bad[:6]:
        print("  " + l.strip())
    if mid is not None and mid[0] < mid[1]:
        # One pad per frame is the floor, measured over the first (no-extremes)
        # half: below that the instance is still counting frames but has
        # stopped feeding its peer, which starves a real session just as dead
        # as silence.
        print(f"  FAIL: only {mid[0]} pads sent for {mid[1]} frames (peer starved)")
        ok = False
    return ok and not bad


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=42050)
    ap.add_argument("--pid", type=int)
    ap.add_argument("--log")
    ap.add_argument("--bind", type=int, default=42051, help="source port (the peer's, with --launch)")
    ap.add_argument("--seconds", type=int, default=30)
    ap.add_argument("--launch", action="store_true", help="start one instance whose peer is us")
    ap.add_argument("--exe", default=os.path.join(here, "..", "build", "melee"))
    ap.add_argument("--disc", default=os.path.join(here, "..", "..", "melee.ciso"))
    ap.add_argument("--work", default="/tmp/net_fuzz")
    ap.add_argument("--session", help="hex session id: valid headers, fuzz the bodies; "
                                      "'auto' with --launch reads it from the log")
    ap.add_argument("--player", type=int, default=1, help="player byte to claim (the remote's)")
    ap.add_argument("--version", type=int, default=VERSION, help="protocol version byte")
    args = ap.parse_args()
    session = None
    if args.session not in (None, "auto"):
        session = int(args.session, 16)
    if not args.launch:
        if args.pid is None or args.log is None:
            sys.exit("--pid and --log are required without --launch")
        if args.session == "auto":
            sys.exit("--session auto needs --launch")
        sys.exit(0 if run(args.port, args.pid, args.log, args.seconds,
                          args.bind if session is not None else None, session=session,
                          player=args.player, version=args.version) else 1)

    import shutil
    sys.path.insert(0, here)
    from net_test import Instance
    shutil.rmtree(args.work, ignore_errors=True)
    os.makedirs(args.work)
    inst = Instance("a", args.exe, args.disc, args.work, args.port, args.bind, {}, False)
    try:
        if not inst.wait_log(r"net: rollback with", 60):
            print("net_fuzz: instance never opened its socket")
            sys.exit(1)
        if args.session == "auto":
            session = int(re.search(r"session ([0-9a-f]{8})", inst.text()).group(1), 16)
        if session is None:
            time.sleep(15)  # past the movie; the title screen then idles at frame 0
        # else: feed at once. The instance blocks in its frame-0 wait right after
        # connecting, and our first valid datagram starts its 7 s silence clock.
        ok = run(args.port, inst.proc.pid, inst.log_path, args.seconds, args.bind, session=session,
                 player=args.player, version=args.version)
    finally:
        inst.kill()
    print("net_fuzz: " + ("PASS" if ok else "FAIL"))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
