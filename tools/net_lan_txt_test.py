#!/usr/bin/env python3
"""TXT-record contract of the LAN lobby, checked against a running instance.

    tools/net_lan_txt_test.py [--exe build/melee] [--disc ../melee.ciso]
                              [--port 42100] [--work /tmp/net_lan_txt]
                              [--c-harness] [--only NAME,NAME]

One instance is launched with MELEE_LAN_TEST=1 (src/pc/vi.c: it joins the
lobby at boot and never starts a match), then this script speaks mDNS to it
on 224.0.0.251:5353 with IP_MULTICAST_TTL 0, so every crafted frame is looped
back to this host and never reaches the network. It asserts:

  announce    the announced TXT carries the key set src/pc/net_lan.c:211-223
              writes, with our own game port, and disc= agrees with the
              "lan: announcing ... disc X" boot line
  ptr         a PTR question for _meleepc._udp.local. is answered
  accept      a well-formed record is listed ("lan: found")
  goodbye     the same record with ttl 0 drops it ("lan: lost")
  rev         another rev= is listed as incompatible, both values named
  disc        another disc= is listed as incompatible, both values named
  reject      every rejection class of parse_txt() produces its own
              "lan: dropped a malformed record: <reason>" line, exactly once
  full        PC_LAN_MAX_PEERS+1 peers -> "lan: lobby full" once

Each case counts its log line before and after and requires the count to move
by exactly the expected amount, so a case that asserts a rejection also proves
the record was not accepted, and vice versa.

The crafted records are built from the instance's OWN announced key set, so
this runs against whatever protocol version the binary implements: the cases
that need keys the running binary does not announce (disc=, and the rejection
classes that only exist with it) are reported as SKIP rather than failed. A
build predating the disc= key therefore still checks announce/ptr/accept/
goodbye/rev/full and the rejection classes it already had.

No compatible state=starting record naming the instance's own install id is
ever sent: that is the one record that would make it dial out (net_lan.c:993).

--c-harness runs only tools/test_net_lan_txt.c, which covers parse_txt() and
the peer table with no game process and no mDNS traffic at all.
"""
import argparse
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from net_test import Instance  # noqa: E402

GROUP, MPORT = "224.0.0.251", 5353
SERVICE = "_meleepc._udp.local."
BOOT_TIMEOUT = 180  # aurora init + disc open before "lan: announcing" on a loaded machine
SETTLE = 3.0        # a record is polled once per frame; allow for a slow boot frame
PROBE_PORT = 42101  # inside this task's range; nothing binds it, it is only announced


# ---- mDNS wire ----------------------------------------------------------

def enc_name(name):
    out = b""
    for label in name.rstrip(".").split("."):
        assert 0 < len(label) < 64, label
        out += bytes([len(label)]) + label.encode()
    return out + b"\x00"


def dec_name(buf, off):
    parts = []
    while True:
        n = buf[off]
        if n == 0:
            off += 1
            break
        if n & 0xC0 == 0xC0:
            parts.append(dec_name(buf, struct.unpack_from("!H", buf, off)[0] & 0x3FFF)[0].rstrip("."))
            off += 2
            break
        parts.append(buf[off + 1:off + 1 + n].decode("utf-8", "replace"))
        off += 1 + n
    return ".".join(parts) + ".", off


def records(buf):
    """(name, rtype, ttl, rdata) for every answer/authority/additional RR."""
    qd, an, ns, ar = struct.unpack_from("!HHHH", buf, 4)
    off = 12
    for _ in range(qd):
        _, off = dec_name(buf, off)
        off += 4
    out = []
    for _ in range(an + ns + ar):
        name, off = dec_name(buf, off)
        rtype, _rclass, ttl, rdlen = struct.unpack_from("!HHIH", buf, off)
        off += 10
        out.append((name, rtype, ttl, buf[off:off + rdlen]))
        off += rdlen
    return out


def txt_keys(rdata):
    """TXT rdata -> {key: value}; a bare key maps to None. Later duplicates win,
    which is exactly what the old parser did and what this script must not rely on."""
    out, i = {}, 0
    while i < len(rdata):
        n = rdata[i]
        s = rdata[i + 1:i + 1 + n].decode("utf-8", "replace")
        k, eq, v = s.partition("=")
        out[k] = v if eq else None
        i += 1 + n
    return out


def txt_answer(instance, pairs, ttl=120):
    """An unsolicited response carrying one TXT record. `pairs` are raw TXT
    strings, so a case can send a bare key or a duplicate."""
    rd = b"".join(bytes([len(p)]) + p.encode() for p in pairs)
    for p in pairs:
        assert len(p) < 256, p
    body = enc_name(instance) + struct.pack("!HHIH", 16, 1, ttl, len(rd)) + rd
    return struct.pack("!HHHHHH", 0, 0x8400, 0, 1, 0, 0) + body


def ptr_question(name=SERVICE):
    return struct.pack("!HHHHHH", 0, 0, 1, 0, 0, 0) + enc_name(name) + struct.pack("!HH", 12, 1)


class Mdns:
    """A socket on the group that never transmits off this host (ttl 0)."""

    def __init__(self, iface_ip):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        if hasattr(socket, "SO_REUSEPORT"):
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
        s.bind(("", MPORT))
        s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                     socket.inet_aton(GROUP) + socket.inet_aton(iface_ip))
        s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(iface_ip))
        s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 0)
        s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
        s.settimeout(0.5)
        self.s = s

    def send(self, pkt):
        self.s.sendto(pkt, (GROUP, MPORT))

    def our_txt(self, seconds, ident=None):
        """The first TXT record of the service seen within `seconds`, as
        (instance name, {key: value}). `ident` restricts it to one install id,
        so a foreign melee-pc on the group is never mistaken for ours (and
        neither is one of our own crafted records looping back)."""
        deadline = time.time() + seconds
        while time.time() < deadline:
            try:
                buf, _ = self.s.recvfrom(4096)
            except socket.timeout:
                continue
            for name, rtype, _ttl, rdata in records(buf):
                if rtype != 16 or not name.endswith(SERVICE):
                    continue
                keys = txt_keys(rdata)
                if ident is None or keys.get("id") == ident:
                    return name, keys
        return None, None


# ---- cases --------------------------------------------------------------

IFACE_RX = re.compile(r"lan: interface \S+ (\d+\.\d+\.\d+\.\d+)")
ANNOUNCE_RX = re.compile(r"lan: (?:announcing|lobby without mDNS:) (\S+) id ([0-9a-f]+) "
                         r"proto (\d+) rev (\S+)(?: disc ([0-9a-f]+))? game port (\d+)")
FOUND = r"lan: found "
INCOMPAT = r"lan: found \S+ \S+ \(incompatible build, not eligible\)"
LOST = r"lan: lost "
FULL = r"lan: lobby full"


def count(inst, pat):
    return len(re.findall(pat, inst.text()))


def occupancy(inst):
    """Peers the lobby currently holds, from the log: every entry produces one
    "found" when it lands and one "lost" when it goes (goodbye or the 5 s
    silence timeout, net_lan.c:291-294)."""
    text = inst.text()
    return len(re.findall(FOUND, text)) - len(re.findall(LOST, text))


def foreign(inst):
    """Peers this script did not craft. Any hit means another melee-pc lobby
    was on the group, so no occupancy-dependent number from the run is
    trustworthy: the run is discarded, never corrected."""
    return sorted({n for n in re.findall(r"lan: found (\S+)", inst.text())
                   if not n.startswith("txtprobe")})


def wait_clean(inst, seconds=8.0):
    """Poll the table empty rather than sleeping a fixed settle: a drop can
    land just after any wait one picks."""
    deadline = time.time() + seconds
    while time.time() < deadline and occupancy(inst) != 0:
        time.sleep(0.2)
    return occupancy(inst) == 0


class Probe:
    """Crafted records built from the instance's own announced keys."""

    def __init__(self, inst, sock, keys):
        self.inst = inst
        self.sock = sock
        self.keys = keys
        self.n = 0
        self.fails = []
        self.skips = []

    def base(self, **over):
        """A well-formed record with a fresh install id, plus overrides; a key
        set to None is dropped."""
        self.n += 1
        d = dict(self.keys)
        d["id"] = "%016x" % (0xdeadbeef0000 + self.n)
        d["name"] = "txtprobe%d" % self.n
        d["port"] = str(PROBE_PORT)
        d["state"] = "lobby"
        d.pop("host", None)
        d.pop("peer", None)
        d.update(over)
        return ["%s=%s" % (k, v) for k, v in d.items() if v is not None]

    def instance_name(self, pairs):
        ident = next(p[3:] for p in pairs if p.startswith("id="))
        return "probe-%s.%s" % (ident, SERVICE)

    def send(self, pairs, ttl=120, name=None):
        self.sock.send(txt_answer(name or self.instance_name(pairs), pairs, ttl))

    def case(self, label, pattern, want, pairs, ttl=120, name=None):
        """Send one record and require `pattern` to appear `want` more times."""
        before = count(self.inst, pattern)
        self.send(pairs, ttl, name)
        deadline = time.time() + SETTLE
        got = 0
        while time.time() < deadline:
            got = count(self.inst, pattern) - before
            if got >= want and want > 0:
                break
            time.sleep(0.1)
        got = count(self.inst, pattern) - before
        if got != want:
            self.fails.append("%s: %r moved by %d, want %d" % (label, pattern, got, want))
            print("  FAIL %-40s %r %+d (want %+d)" % (label, pattern, got, want), flush=True)
        else:
            print("  ok   %-40s %+d %s" % (label, got, pattern), flush=True)
        return got == want

    def reject(self, label, reason, pairs, **kw):
        """A rejection class: its own line once, and the record not listed.
        One send covers both, so each class costs one settle at most."""
        pat = "lan: dropped a malformed record: " + re.escape(reason)
        listed = count(self.inst, FOUND)
        ok = self.case(label, pat, 1, pairs, **kw)
        moved = count(self.inst, FOUND) - listed
        if moved != 0:
            self.fails.append("%s: dropped, yet %r moved by %d" % (label, FOUND, moved))
            print("  FAIL %-40s listed anyway (%+d %s)" % (label, moved, FOUND), flush=True)
        return ok and moved == 0

    def not_listed(self, label, pairs, **kw):
        """Only that the record never reaches the lobby: the assertion that
        holds on a build without the reason lines too."""
        return self.case(label + " (not listed)", FOUND, 0, pairs, **kw)

    def incompatible(self, label, pairs, detail):
        """A peer listed as incompatible, with the detail line naming both
        sides. Both counts are taken across ONE send: the detail line is only
        logged when the entry is new (net_lan.c:592-604), so re-sending the
        record to check it separately would always read zero."""
        d0 = count(self.inst, detail)
        ok = self.case(label, INCOMPAT, 1, pairs)
        moved = count(self.inst, detail) - d0
        if moved != 1:
            self.fails.append("%s: the detail line %r moved by %d, want 1" % (label, detail, moved))
            print("  FAIL %-40s detail line %+d (want +1)" % (label, moved), flush=True)
        else:
            print("  ok   %-40s detail line +1" % (label + ", named"), flush=True)
        self.send(pairs, ttl=0)
        return ok and moved == 1


def run(args):
    work = os.path.join(args.work)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    inst = Instance("lan", args.exe, args.disc, work, args.port, args.port + 1,
                    {"MELEE_LAN_TEST": "1"}, True)
    print("[txt] pid %d port %d log %s/lan.log" % (inst.proc.pid, args.port, work), flush=True)
    fails, skips = [], []
    sock = None
    try:
        if not inst.wait_log(r"lan: (announcing|lobby without mDNS)", BOOT_TIMEOUT):
            return ["never reached the lobby (no 'lan: announcing' in %ds)" % BOOT_TIMEOUT], []
        text = inst.text()
        boot = ANNOUNCE_RX.search(text)
        if not boot:
            return ["the 'lan: announcing' line does not parse: %r" %
                    re.findall(r"lan: announcing.*", text)[:1]], []
        name, ident, proto, rev, boot_disc, port = boot.groups()
        iface = IFACE_RX.search(text)
        if not iface:
            return ["no 'lan: interface' line: no usable interface, cannot speak mDNS"], []
        print("[txt] instance %s id %s proto %s rev %s disc %s port %s on %s"
              % (name, ident, proto, rev, boot_disc, port, iface.group(1)), flush=True)

        sock = Mdns(iface.group(1))
        inst_name, keys = sock.our_txt(6, ident)
        if keys is None:
            return ["no announce of %s seen on the group in 6 s (multicast blocked?)" % SERVICE], []
        print("[txt] announced %s" % inst_name, flush=True)
        print("[txt] TXT %s" % " ".join("%s=%s" % kv for kv in sorted(keys.items())), flush=True)

        # announce: the key set and its agreement with the boot line
        want = {"v", "rev", "id", "name", "port", "state", "gen"}
        missing = want - set(keys)
        if missing:
            fails.append("announced TXT lacks %s" % ",".join(sorted(missing)))
        if keys.get("port") != str(args.port):
            fails.append("announced port=%s, want %d" % (keys.get("port"), args.port))
        if keys.get("v") != proto or keys.get("rev") != rev or keys.get("id") != ident:
            fails.append("announced v/rev/id %s/%s/%s disagree with the boot line %s/%s/%s"
                         % (keys.get("v"), keys.get("rev"), keys.get("id"), proto, rev, ident))
        if keys.get("state") != "lobby":
            fails.append("announced state=%s, want lobby" % keys.get("state"))

        has_disc = "disc" in keys
        if not has_disc:
            skips.append("disc=: this build does not announce it (pre-disc-identity binary)")
        else:
            if not re.fullmatch(r"[0-9a-f]{8}", keys["disc"]):
                fails.append("disc=%r is not 8 lowercase hex digits" % keys["disc"])
            if boot_disc != keys["disc"]:
                fails.append("boot line says disc %s, TXT says %s" % (boot_disc, keys["disc"]))
            print("[txt] disc identity %s (boot line and TXT agree)" % keys["disc"], flush=True)

        # The lobby must be verifiably empty before anything is counted: a
        # foreign melee-pc on the group would land in the same table.
        if occupancy(inst) != 0 or foreign(inst):
            return ["the lobby already holds %d peer(s) %s at the start of the run: another "
                    "melee-pc is on the group. Discard and rerun, do not subtract it."
                    % (occupancy(inst), foreign(inst))], skips

        p = Probe(inst, sock, keys)

        # ptr: a question for the service is answered
        before = time.time()
        sock.send(ptr_question())
        answered = False
        while time.time() < before + 3:
            got_name, got_keys = sock.our_txt(0.6, ident)
            if got_keys is not None and got_keys.get("id") == ident:
                answered = True
                break
        print("  %s   %-40s" % ("ok  " if answered else "FAIL", "ptr question answered"), flush=True)
        if not answered:
            fails.append("a PTR question for %s was not answered" % SERVICE)

        # accept / goodbye
        pairs = p.base()
        p.case("well-formed record listed", FOUND + "txtprobe%d" % p.n, 1, pairs)
        p.case("goodbye drops it", LOST + "txtprobe%d" % p.n, 1, pairs, ttl=0)

        # rev / disc mismatch: listed, never eligible, both values named
        detail = r"lan:   theirs: proto %s rev %s-other" % (re.escape(proto), re.escape(rev))
        p.incompatible("another rev is incompatible", p.base(rev=rev + "-other"), detail)
        if not wait_clean(inst):
            fails.append("the incompatible-rev peer did not leave the table after its goodbye "
                         "(occupancy %d)" % occupancy(inst))

        if has_disc:
            other = "%08x" % ((int(keys["disc"], 16) ^ 1) & 0xFFFFFFFF)
            detail = (r"theirs: proto \S+ rev \S+ disc %s, ours: proto \S+ rev \S+ disc %s"
                      % (re.escape(other), re.escape(keys["disc"])))
            p.incompatible("another disc is incompatible", p.base(disc=other), detail)
            if not wait_clean(inst):
                fails.append("the incompatible-disc peer did not leave the table after its "
                             "goodbye (occupancy %d)" % occupancy(inst))

        # Every rejection class of parse_txt(), one record each: a class is
        # logged once per session, so two records of the same class would make
        # the second look unrejected. The reason lines only exist on the new
        # parser; the "not listed" form of the same assertion holds on any
        # build, so that is what a pre-disc binary is checked with.
        strict = [
            ("unknown key", "unknown key on our own protocol version",
             p.base() + ["future=yes"]),
            ("duplicate key", "duplicate key", p.base() + ["port=1234"]),
            ("bare key, no '='", "empty, over-long or unprintable value",
             [x for x in p.base() if not x.startswith("gen=")] + ["gen"]),
            ("no name=", "no v/id/name/port", p.base(name=None)),
            ("bad protocol version", "bad protocol version", p.base(v=proto + "x")),
            ("id of 0", "bad install id", p.base(id="0" * 16)),
            # 16 chars: a build that truncates instead of rejecting still names it
            # txtprobe*, so the foreign-peer guard does not misfire on our own record.
            ("over-long name", "over-long name", p.base(name="txtprobelongname")),
            ("port with trailing junk", "bad game port", p.base(port="%dx" % PROBE_PORT)),
            ("no disc=", "no rev/disc/state/gen", p.base(disc=None)),
            ("non-hex disc", "bad disc id", p.base(disc="zzzzzzzz")),
            ("unknown state", "unknown state", p.base(state="hosting")),
            ("non-numeric gen", "bad gen", p.base(gen="7x")),
            ("starting without host/peer", "starting without host/peer",
             p.base(state="starting")),
            ("host/peer in a lobby record", "host/peer outside a starting record",
             p.base(host="10.0.0.9:1", peer="0" * 16)),
            ("non-hex peer=", "bad peer id",
             p.base(state="starting", host="10.0.0.9:1", peer="zz")),
        ]
        # Records BOTH parsers refuse (the old one checked id, port range and
        # an empty name, net_lan.c before this change), so the not-listed
        # assertion holds whichever binary is running. Their reason classes are
        # covered above; here only the outcome is asserted, which is why a
        # repeat of an already-logged class is harmless.
        quiet = [
            ("id of 0", p.base(id="0" * 16)),
            ("port of 0", p.base(port="0")),
            ("port above 65535", p.base(port="65536")),
            ("no name=", p.base(name=None)),
            ("empty name=", p.base(name="")),
        ]
        if has_disc:
            for label, reason, pairs in strict:
                p.reject(label, reason, pairs)
        else:
            skips.append("%d rejection reasons need the new parser (a pre-disc build drops "
                         "malformed records silently and tolerates most of these records, so "
                         "only the build-independent ones below are asserted)" % len(strict))
        for label, pairs in quiet:
            p.not_listed(label, pairs)

        # Full lobby: PC_LAN_MAX_PEERS peers, verified present, then one more.
        # A crafted record is announced once, so an entry left behind would
        # also drain on its own after LOST_NS; the occupancy poll is what makes
        # the count mean something either way.
        max_peers = 8  # PC_LAN_MAX_PEERS, src/pc/net_lan.h:19
        if not wait_clean(inst):
            fails.append("the table was not empty (%d) before the full-lobby case; its numbers "
                         "are not reported" % occupancy(inst))
        else:
            filled = [p.base() for _ in range(max_peers + 1)]
            before = count(inst, FULL)
            for pairs in filled[:-1]:
                p.send(pairs)
            deadline = time.time() + SETTLE
            while time.time() < deadline and occupancy(inst) < max_peers:
                time.sleep(0.1)
            if occupancy(inst) != max_peers:
                fails.append("only %d of %d crafted peers landed: the fill is inconclusive"
                             % (occupancy(inst), max_peers))
            else:
                p.case("the %dth peer is refused" % (max_peers + 1), FULL, 1, filled[-1])
                if count(inst, FULL) - before != 1:
                    fails.append("'lobby full' logged %d times, want once per session"
                                 % (count(inst, FULL) - before))
            for pairs in filled:
                p.send(pairs, ttl=0)
            if not wait_clean(inst):
                fails.append("%d peer(s) still in the table after every goodbye" % occupancy(inst))

        if foreign(inst):
            fails.append("a foreign peer %s appeared during the run: discard it and rerun"
                         % foreign(inst))
        fails += p.fails
        if inst.proc.poll() is not None:
            fails.append("the instance exited (code %d) during the run" % inst.proc.returncode)
    finally:
        if sock is not None:
            sock.s.close()
        inst.kill()
    return fails, skips


def c_harness(here):
    """Build and run tools/test_net_lan_txt.c with the project's own include
    paths, under ASan/UBSan. Nothing is written to build/."""
    import json
    import shlex
    db = json.load(open(os.path.join(here, "..", "build", "compile_commands.json")))
    entry = next(x for x in db if x["file"].endswith("src/pc/net_lan.c"))
    incs = [x for x in shlex.split(entry["command"]) if x.startswith("-I")]
    out = "/tmp/test_net_lan_txt"
    cmd = ["cc", "-DTARGET_PC=1", "-DXXH_INLINE_ALL", "-std=gnu11", "-g", "-O1",
           "-fsanitize=address,undefined", "-Wall", "-Wextra", "-Wno-unused-parameter",
           "-fdiagnostics-color=never"] + incs + \
          [os.path.join(here, "test_net_lan_txt.c"), "-o", out]
    build = subprocess.run(cmd, capture_output=True, text=True)
    if build.returncode != 0:
        print(build.stdout + build.stderr)
        return ["test_net_lan_txt.c did not build"]
    fails = []
    ids = []
    for image in ("a", "b"):
        r = subprocess.run([out, image], capture_output=True, text=True)
        print(r.stdout + r.stderr, end="", flush=True)
        if r.returncode != 0:
            fails.append("test_net_lan_txt %s failed" % image)
        m = re.search(r"disc id ([0-9a-f]{8})", r.stdout)
        ids.append(m.group(1) if m else None)
    if len(set(ids)) != 2 or None in ids:
        fails.append("the two pretend images produced disc ids %s: the identity does not "
                     "depend on the image" % ids)
    else:
        print("image a -> %s, image b -> %s: the identity follows the image" % tuple(ids))
    return fails


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", default=os.path.join(here, "..", "build", "melee"))
    ap.add_argument("--disc", default=os.path.join(here, "..", "..", "melee.ciso"))
    ap.add_argument("--port", type=int, default=42100, help="the instance's MELEE_NET_PORT")
    ap.add_argument("--work", default="/tmp/net_lan_txt")
    ap.add_argument("--c-harness", action="store_true",
                    help="only tools/test_net_lan_txt.c (no game process, no mDNS)")
    ap.add_argument("--only", default="c,live", help="comma-separated: c,live")
    args = ap.parse_args()

    only = ["c"] if args.c_harness else args.only.split(",")
    fails, skips = [], []
    if "c" in only:
        fails += c_harness(here)
    if "live" in only:
        f, s = run(args)
        fails += f
        skips += s
    for s in skips:
        print("SKIP " + s)
    for f in fails:
        print("FAIL " + f)
    print("net_lan_txt_test: " + ("PASS" if not fails else "FAIL (%d)" % len(fails)))
    sys.exit(0 if not fails else 1)


if __name__ == "__main__":
    main()
