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
boundary around the instance's frame, INT32 extremes, count 0/RED/RED+1/255,
bodies one byte short. No BYE and no foreign version in this mode (either
ends the session and the rest would be inert). We also play peer: the
instance's own input packets tell us its newest frame and we feed neutral
pads up to it, so it keeps ticking instead of stalling out.

--attack ack-overshoot is not fuzzing but the deterministic regression for the
ack sanity check in on_ack(): play peer for a window, measure the pads the
instance sends, send ONE ack for its newest frame + --ack-offset (default
1000, a frame it never sent), measure again. An instance that accepts it keeps
counting frames and keeps sending packets, with no pads in them, for the rest
of the session. The mixed stream only catches that by luck, so this mode is
its own pass/fail line.

Every datagram carries the 8-byte authentication tag the receiver requires
(src/pc/net_wire.c), computed with the session key: the fuzzer stands for a
peer that holds the key and sends garbage anyway, which is the only attacker
the body validators below the gate can still see. --key gives the secret
(default: MELEE_NET_KEY from the environment, else the fixture key net_test.py
starts its instances with); --key "" sends untagged datagrams, which an
authenticated instance drops at the gate and an unauthenticated one -- one
started without MELEE_NET_KEY -- still refuses as the wrong shape.
"""
import argparse
import hashlib
import os
import random
import re
import socket
import struct
import sys
import time

BAD = ("net: DESYNC", "peer silent", "peer left", "cannot roll back", "Segmentation",
       "FATAL", "abort", "Assertion")
# Refusals worth counting: the header/frame gates, plus the handshake's
# per-class lines ("net: RULES rejected: ...", "net: READY ignored (...)",
# net_handshake.c) and the resume lane's length check.
REJECT = ("net: dropped", "net: peer speaks protocol", "net: RULES rejected",
          "net: READY ignored", "net: RESUME of")

# src/pc/net_internal.h NET_MAC_LEN, and the direct-mode key derivation of
# net_key_direct() (src/pc/net_wire.c), which is BLAKE2b of a label and the
# secret. Cross-checked against monocypher: same key, same tag.
MAC_LEN = 8
DIRECT_LABEL = b"melee-pc netplay direct key v1"
KEY = None  # set from main(); None means "send untagged"


def derive_key(secret):
    return hashlib.blake2b(DIRECT_LABEL + secret.encode(), digest_size=32).digest()


def send(sock, body, port):
    """One datagram out, tagged the way tx() tags the real ones."""
    mac = hashlib.blake2b(body, key=KEY, digest_size=MAC_LEN).digest() if KEY else bytes(MAC_LEN)
    sock.sendto(body + mac, ("127.0.0.1", port))


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
        count = rng.choice([0, 1, RED, RED + 1, 128, 255])
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


def wire_redundancy(default=32):
    """Frames an input packet may carry (REDUNDANCY, src/pc/net_internal.h):
    the count edges below sit on it, and it moved 16 -> 32 with protocol 9."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "pc",
                        "net_internal.h")
    try:
        with open(path) as f:
            m = re.search(r"define REDUNDANCY\s+(\d+)", f.read())
        return int(m.group(1)) if m else default
    except OSError:
        return default


RED = wire_redundancy()


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
            first, count = newest - (RED - 1), RED
        elif edge == 3:  # count past REDUNDANCY: refused by the parser
            first = max(0, base - (RED - 1))
            count, newest = rng.choice([RED + 1, 128, 255]), base
        elif edge == 4:  # first < 0
            first, count, newest = rng.choice([-1, INT_MIN]), 1, 0
        elif edge == 5:  # first + count - 1 > newest
            first, count, newest = 0, 2, 0
        elif edge == 6:  # INT32_MAX newest
            first, count, newest = INT_MAX, 1, INT_MAX
        elif edge == 7:  # first + count - 1 overflows int32 in the parser
            first, count, newest = INT_MAX, RED, INT_MAX
        elif edge == 8:
            first, count, newest = INT_MAX - (RED - 1), RED, INT_MAX
        elif edge == 9:  # newest far ahead, frames fine
            first, count, newest = 0, RED, INT_MAX
        elif edge == 10:  # ck_frame below -1, otherwise valid
            first, count, newest = max(0, base - (RED - 1)), RED, base
            ck_frame = rng.choice([-2, INT_MIN])
        elif edge == 11:  # ck_frame far ahead: stored, never confirmed
            first, count, newest = max(0, base - (RED - 1)), RED, base
            ck_frame = INT_MAX
        elif edge == 12:  # newest < 0 with sane first/count
            first, count, newest = 0, 1, rng.choice([-1, INT_MIN])
        else:  # contiguous with what we fed, past the acceptance window
            first = max(0, base - 3)
            count = RED
            newest = first + count - 1 + rng.choice([0, 16, 17, 32])
        body = hdr(ord("M")) + struct.pack(">HiiiIB", rng.getrandbits(16), newest, first, ck_frame,
                                           rng.getrandbits(32), count & 0xFF)
        # neutral pads, delta-coded (pads_encode): each an all-zero mask byte
        body += bytes(min(count & 0xFF, RED))  # what the parser expects for that count
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
        # ponytail: 0x12 (REL_RESUME) is left out. on_rel dispatches it into
        # net_resume_rel (net_reliable.c:134-135), where a 20-byte payload with
        # a session or seed that is not ours ends the session with
        # PC_NET_PEER_RESUME by design (net.c:706-730) -- a legitimate teardown,
        # which is the opposite of what this mode asserts. Fuzzing that lane
        # needs its own mode with "the session ends cleanly" as the pass line;
        # tools/test_net_resume.c covers the arithmetic today.
        ln = rng.choice([0, 1, 255, 256, 257, 1024, 65535])
        body = hdr(ord("R")) + struct.pack(">BBH", rng.randrange(256),
                                           rng.choice([0, 1, 2, 3, 0x10, 0x11, 0x13, 0x14, 0xFF]),
                                           ln)
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
    send(sock, pk, port)


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
        if len(d) >= 26 + MAC_LEN and d[0] == ord("M") and d[6] != our_player:
            best = max(best, struct.unpack_from(">i", d, 9)[0])  # past Hdr(7) + seq(2)
            pads += d[25]  # count: Hdr(7) seq(2) newest/first/ck_frame(12) ck(4)


def udp(bind_port):
    """Our peer socket. The instance only reads datagrams from the address in
    its MELEE_NET, so the source port is not optional in session mode."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    if bind_port:
        s.bind(("127.0.0.1", bind_port))
    s.setblocking(False)
    return s


class Peer:
    """Play peer: drain the instance's packets, feed neutral pads up to the
    newest frame it claims. `pads` is what it has sent us, `fed` the frame we
    have fed it up to, `base` its newest frame."""

    def __init__(self, s, port, session, player, version):
        self.s, self.port = s, port
        self.session, self.player, self.version = session, player, version
        self.base = self.fed = self.pads = 0

    def pump(self):
        newest, got = peer_newest(self.s, self.player)
        self.base = max(self.base, newest)
        self.pads += got
        if self.base > self.fed:
            feed(self.s, self.port, self.session, self.player, self.version, self.base)
            self.fed = self.base

    def ack(self, frame):
        """One Ack datagram for `frame` (src/pc/net_internal.h: Hdr 'A', seq, frame)."""
        send(self.s, struct.pack(">BBIB", ord("A"), self.version, self.session, self.player) +
             struct.pack(ACK_BODY, self.base & 0xFFFF, frame), self.port)

    def window(self, seconds):
        """Play peer for `seconds`; returns the pads that arrived in that time."""
        t0, first = time.time(), self.pads
        while time.time() - t0 < seconds:
            try:
                self.pump()
            except OSError:
                pass  # ICMP unreachable if the target died; alive() decides
            time.sleep(0.001)
        return self.pads - first


def ack_overshoot(port, pid, log_path, session, player=1, version=None, bind_port=None,
                  window=5.0, offset=1000, expect="hold"):
    """Deterministic regression for the ack sanity check in on_ack()
    (src/pc/net.c:435-441). send_inputs() ships frames s_last_acked + 1 ..
    s_wrote, so an ack for a frame the instance never sent leaves it with
    first > newest: it keeps counting frames and keeps sending packets, but
    with count 0 for the rest of the session. The peer is starved as dead as
    by silence, and nothing in the log says so.

    Play peer normally, measure the pads it sends over `window`, send exactly
    ONE ack for base + `offset`, measure again. Guard present: the rate holds.
    Guard removed: it collapses to whatever was already in flight.

    The pad-flow floor in run() catches the same wedge only probabilistically:
    it needs the mixed stream to land that ack and not a later INT32_MAX one,
    which overflows s_last_acked + 1 into the negative clamp and heals it.

    `offset`/`expect` exist to injection-validate the check itself: an ack for
    an old frame (offset well below -RING) is harmless and must NOT collapse
    the rate, so --ack-offset -600 --expect collapse has to report FAIL."""
    version = VERSION if version is None else version
    s = udp(bind_port)
    start = os.path.getsize(log_path)
    peer = Peer(s, port, session, player, version)
    t0 = time.time()
    while time.time() - t0 < 20 and peer.base < 60:
        peer.window(0.05)  # warm up: the frames must be moving to measure anything
    before = peer.window(window)
    base = peer.base
    peer.ack(base + offset)
    after = peer.window(window)
    ok = alive(pid)
    with open(log_path, "rb") as f:
        f.seek(start)
        bad = [l for l in f.read().decode("utf-8", "replace").splitlines()
               if any(b in l for b in BAD)]
    held = before > 0 and after >= before / 2
    print(f"net_fuzz ack-overshoot: session {session:08x} player {player} v{version}, "
          f"its frame {base} (now {peer.base}), one ack for {base + offset} (base{offset:+d})")
    print(f"  pads in {window} s before: {before}, after: {after}"
          f" ({100 * after // before if before else 0}%), rate "
          f"{'held' if held else 'COLLAPSED'}, expected {expect}, pid {pid} "
          f"{'alive' if ok else 'DEAD'}")
    for l in bad[:6]:
        print("  " + l.strip())
    if before < 300:
        print(f"  FAIL: only {before} pads before the ack; nothing was measured")
        return False
    if held != (expect == "hold"):
        print("  FAIL: " + ("one bogus ack starved the peer (on_ack() lost its "
                            "frame <= s_wrote check)" if expect == "hold" else
                            "the rate held, so this ack proves nothing about the guard"))
        return False
    return ok and not bad


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
    s = udp(bind_port)
    start = os.path.getsize(log_path)
    t0 = time.time()
    n = 0
    peer = Peer(s, port, session, player, version)
    mid = None  # (pads it sent back, frames fed) when the extremes start
    while time.time() - t0 < seconds:
        extremes = session is not None and time.time() - t0 > seconds / 2
        if extremes and mid is None:
            mid = (peer.pads, peer.fed)
        try:
            if session is None:
                send(s, datagram(rng), port)
            else:
                peer.pump()
                send(s, datagram_session(rng, session, player, version, peer.base, extremes), port)
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
          f"{f' session {session:08x} player {player} v{version}, fed to frame {peer.fed}, {peer.pads} pads back' if session is not None else ''}"
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
    global KEY
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
    ap.add_argument("--version", type=int, default=None,
                    help="protocol version byte (default: the proto the instance reports with "
                         "--launch, else PC_NET_PROTO_VERSION from src/pc/net.h). A build older "
                         "than the header speaks the older version, and every datagram at the "
                         "header's version dies at the version gate.")
    ap.add_argument("--attack", choices=["mixed", "ack-overshoot"], default="mixed",
                    help="mixed: the random stream. ack-overshoot: the deterministic "
                         "on_ack() regression (needs a session; --seconds is the window)")
    ap.add_argument("--ack-offset", type=int, default=1000,
                    help="ack-overshoot: frames past the instance's newest to ack")
    ap.add_argument("--expect", choices=["hold", "collapse"], default="hold",
                    help="ack-overshoot: what the pad rate must do (see --help of the mode)")
    ap.add_argument("--key", default=None,
                    help="shared secret the target was started with (default: MELEE_NET_KEY, "
                         "else net_test.py's fixture key). Empty sends untagged datagrams.")
    args = ap.parse_args()
    session = None
    if args.session not in (None, "auto"):
        session = int(args.session, 16)
    sys.path.insert(0, here)
    from net_test import NET_KEY
    secret = args.key if args.key is not None else os.environ.get("MELEE_NET_KEY", NET_KEY)
    KEY = derive_key(secret) if secret else None
    window = min(args.seconds / 2.0, 5.0) if args.attack == "ack-overshoot" else 0

    def go(pid, log_path):
        if args.attack == "mixed":
            return run(args.port, pid, log_path, args.seconds,
                       args.bind if session is not None else None, session=session,
                       player=args.player, version=args.version)
        if session is None:
            sys.exit("--attack ack-overshoot needs --session (a valid header, or 'auto')")
        return ack_overshoot(args.port, pid, log_path, session, args.player, args.version,
                             args.bind, window, args.ack_offset, args.expect)

    if not args.launch:
        if args.pid is None or args.log is None:
            sys.exit("--pid and --log are required without --launch")
        if args.session == "auto":
            sys.exit("--session auto needs --launch")
        sys.exit(0 if go(args.pid, args.log) else 1)

    import shutil
    from net_test import Instance
    shutil.rmtree(args.work, ignore_errors=True)
    os.makedirs(args.work)
    inst = Instance("a", args.exe, args.disc, args.work, args.port, args.bind, {}, False)
    try:
        if not inst.wait_log(r"net: rollback with", 60):
            print("net_fuzz: instance never opened its socket")
            sys.exit(1)
        # "net: rollback with ... session f2a239b5, proto 3": a build older than
        # the header still speaks its own version, and the session id is on the
        # same line.
        banner = re.search(r"session ([0-9a-f]{8}), proto (\d+)", inst.text())
        if args.session == "auto":
            session = int(banner.group(1), 16)
        if args.version is None and banner is not None:
            args.version = int(banner.group(2))
        if session is None and args.attack == "mixed":
            time.sleep(15)  # past the movie; the title screen then idles at frame 0
        # else: feed at once. The instance blocks in its frame-0 wait right after
        # connecting, and our first valid datagram starts its 7 s silence clock.
        ok = go(inst.proc.pid, inst.log_path)
    finally:
        inst.kill()
    print("net_fuzz: " + ("PASS" if ok else "FAIL"))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
