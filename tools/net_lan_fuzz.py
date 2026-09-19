#!/usr/bin/env python3
"""Throw malformed mDNS answers at a lobby instance's discovery socket.

    tools/net_lan_fuzz.py [--port 42300] [--exe build/melee] [--disc ../melee.ciso]
                          [--mutations 400] [--seed 1] [--keep-peers]

Launches one MELEE_LAN_TEST=1 instance (the LAN lobby without the menu, it
never starts a match) and crafts DNS messages at it: bad service names, a
wrong protocol version, over-long and truncated TXT strings, duplicate and
missing keys, absurd ports, id 0, an answer count of 65535, a compression
pointer loop, a record claiming the instance's own install id, a rdlength
past the end of the datagram, non-TXT types, datagrams shorter than a DNS
header, and a seeded byte-mutation pass over a valid frame.

Frames go to the mDNS group (224.0.0.251:5353) with IP_MULTICAST_IF set to
the interface the instance joined on and IP_MULTICAST_TTL 0, so they reach
every socket on this machine that joined the group and never leave it. They
are still seen by other local instances, so the records that are meant to be
accepted carry a deliberately wrong rev= (incompatible builds are listed but
never eligible for a host election), every name is FUZZ<case> and every
accepted record is withdrawn with a goodbye (ttl 0) as soon as it has been
checked. mDNS is shared: announce a run before starting one.

Pass = the instance survives, keeps announcing, keeps its peer table within
PC_LAN_MAX_PEERS, and every case marked `reject` leaves no trace of its name
in the log while every case marked `accept` does appear in a `lan:` line.
That last group is the point: a fuzzer whose frames all die at the first
length check proves nothing, so at least one crafted record has to be parsed
all the way into the peer table for the run to count.

The parser is src/pc/mdns/mdns.h (vendored), the consumer is on_record() in
src/pc/net_lan.c. Assertions key on process survival, the peer table staying
sane and the presence or absence of a crafted name in the `lan:` lines, not
on the wording of any one rejection, so a change to the record rules does
not silently turn this green.
"""
import argparse
import os
import random
import re
import shutil
import socket
import struct
import sys
import time

GROUP, MDNS_PORT = "224.0.0.251", 5353
SERVICE = "_meleepc._udp.local."          # src/pc/net_lan.c:74
MAX_PEERS = 8                             # PC_LAN_MAX_PEERS, src/pc/net_lan.h:19
NAME_LEN = 16                             # PC_LAN_NAME_LEN: names are truncated to 15 chars
BAD = ("Segmentation", "FATAL", "abort", "Assertion", "SIGSEGV", "sanitizer")
TXT, SRV, PTR, A = 16, 33, 12, 1
IN = 1
ANSWER = 0x8400  # QR + AA: an unsolicited answer, what an announce looks like
SETTLE_MIN, SETTLE_MAX = 7.0, 20.0  # wait for crafted peers to age out (LOST_NS is 5 s)


# ---- DNS message construction -------------------------------------------

def labels(name):
    """`a.b.c.` -> wire labels, terminated."""
    return b"".join(bytes([len(p)]) + p.encode() for p in name.strip(".").split(".")) + b"\0"


def strings(items):
    """TXT rdata: each item length-prefixed. An item may be (declared_len, bytes)
    to lie about its length, which is what over-long values look like."""
    out = b""
    for it in items:
        if isinstance(it, tuple):
            n, b = it
        else:
            b = it.encode() if isinstance(it, str) else it
            n = len(b)
        out += bytes([n & 0xFF]) + b
    return out


def record(owner, rtype, ttl, rdata, rdlen=None):
    return owner + struct.pack(">HHIH", rtype, IN, ttl,
                               len(rdata) if rdlen is None else rdlen) + rdata


def message(body, qd=0, an=1, ns=0, ar=0, flags=ANSWER, qid=0):
    return struct.pack(">HHHHHH", qid, flags, qd, an, ns, ar) + body


def keys(proto, rev, ident, name, port, state="lobby", gen=1, disc="deadbeef"):
    """The TXT set announce_on() writes (src/pc/net_lan.c:148-217). A record
    claiming our own protocol version must carry the full key set including
    disc=, or parse_txt() rejects it as RJ_NO_OURS before any other rule is
    reached — which is what makes every 'accept' case here a real parse."""
    return [f"v={proto}", f"rev={rev}", f"id={ident:016x}", f"name={name}",
            f"port={port}", f"state={state}", f"gen={gen}", f"disc={disc}"]


def query(name=SERVICE, rtype=PTR):
    """A question, which makes the instance answer with its own record."""
    return message(labels(name) + struct.pack(">HH", rtype, IN), qd=1, an=0, flags=0)


class Craft:
    """Frames for one target. `proto`/`rev`/`self_id` come from the instance's
    own announce banner; `rev` is deliberately mismatched on every crafted
    record so none of them can ever win a host election anywhere on this
    machine."""

    def __init__(self, proto, self_id, rev="fuzz-not-a-real-build"):
        self.proto, self.rev, self.self_id = proto, rev, self_id
        self.ids = {}

    def ident(self, tag):
        self.ids.setdefault(tag, 0xFA22000000000000 | (len(self.ids) + 1))
        return self.ids[tag]

    def owner(self, tag, svc=SERVICE):
        return labels(f"FUZZ{tag}-{self.ident(tag):016x}.{svc}")

    def txt(self, tag, items, ttl=120, svc=SERVICE, **kw):
        return message(record(self.owner(tag, svc), TXT, ttl, strings(items)), **kw)

    def valid(self, tag, port=41999, **kw):
        """A record that must be parsed all the way into the peer table."""
        return self.txt(tag, keys(self.proto, self.rev, self.ident(tag), "FUZZ" + tag, port), **kw)

    def goodbye(self, tag, port=41999):
        return self.valid(tag, port, ttl=0)


def cases(c):
    """(tag, expectation, [datagram, ...]). `accept`: the name must show up in
    a `lan:` line. `reject`: it must not. `info`: reported, not asserted, for
    rules another task may be changing under us."""
    out = []

    # ---- must be parsed: the proof that these frames are not dying early ---
    out.append(("ok", "accept", [c.valid("ok")]))

    # The owner name as a compression pointer into the SERVICE labels of a
    # preceding PTR record, which is how a real announce writes it.
    svc = labels(SERVICE)
    ptr = record(svc, PTR, 120, labels("FUZZptr-x." + SERVICE))
    owner = bytes([len(f"FUZZptr-{c.ident('ptr'):016x}")]) + \
        f"FUZZptr-{c.ident('ptr'):016x}".encode() + struct.pack(">H", 0xC000 | 12)
    body = ptr + record(owner, TXT, 120,
                        strings(keys(c.proto, c.rev, c.ident("ptr"), "FUZZptr", 41998)))
    out.append(("ptr", "accept", [message(body, an=2)]))

    # An answer count far past what the datagram holds: the one real record
    # must still be parsed before mdns_records_parse() runs out of bytes.
    out.append(("cnt", "accept", [c.valid("cnt", 41997)[:2] +
                                  struct.pack(">HHHHH", ANSWER, 0, 0xFFFF, 0, 0) +
                                  c.valid("cnt", 41997)[12:]]))

    # Duplicate keys used to be "last one wins"; parse_txt() now rejects the
    # whole record (RJ_DUP, net_lan.c:439), so this is a refusal case with a
    # reason of its own rather than a peer on port 41996.
    dup = keys(c.proto, c.rev, c.ident("dup"), "FUZZdup", 1) + ["port=41996", "id=%016x" % c.ident("dup")]
    out.append(("dup", "reject", [c.txt("dup", dup)]))

    # parse_txt() refuses a record whose pair count reaches TXT_MAX_PAIRS
    # (net_lan.c:426): a full buffer means mdns_record_parse_txt may have
    # truncated it, so nothing in it can be trusted. Probe the boundary with a
    # record claiming ANOTHER protocol version, because unknown keys are only
    # tolerated there (RFC 6763 6.6, net_lan.c:474-479); padding a record that
    # claims our own version is now rejected on the pad keys alone, which
    # would test the wrong rule. Accepted here means listed-as-incompatible,
    # which still requires every one of v/id/name/port to have been parsed
    # from inside the first 12 strings.
    fill = [f"pad{i}=x" for i in range(3)]  # 8 keys + 3 = 11, one below the cap of 12
    out.append(("in12", "accept",
                [c.txt("in12", keys("999", c.rev, c.ident("in12"), "FUZZin12", 41995) + fill)]))
    out.append(("past12", "reject",
                [c.txt("past12", [f"pad{i}=x" for i in range(12)] +
                       keys(c.proto, c.rev, c.ident("past12"), "FUZZpast12", 41994))]))

    # ---- must be refused --------------------------------------------------
    k = lambda tag, port=41993, **kw: keys(c.proto, c.rev, c.ident(tag), "FUZZ" + tag, port, **kw)
    out += [
        # Another service, and a name that is only the service itself.
        ("svc", "reject", [c.txt("svc", k("svc"), svc="_meleepcx._udp.local."),
                           message(record(labels(SERVICE), TXT, 120, strings(k("svc"))))]),
        # id missing / zero / not hex at all.
        ("noid", "reject", [c.txt("noid", [x for x in k("noid") if not x.startswith("id=")])]),
        ("id0", "reject", [c.txt("id0", [x if not x.startswith("id=") else "id=" + "0" * 16
                                         for x in k("id0")])]),
        ("idtext", "reject", [c.txt("idtext", [x if not x.startswith("id=") else "id=zzzz"
                                               for x in k("idtext")])]),
        # Absurd ports: 0, past 16 bits, negative, empty, text.
        ("port0", "reject", [c.txt("port0", k("port0", 0))]),
        ("portbig", "reject", [c.txt("portbig", k("portbig", 99999)),
                               c.txt("portbig", k("portbig", 65536)),
                               c.txt("portbig", k("portbig", 2**31))]),
        ("portneg", "reject", [c.txt("portneg", k("portneg", -1))]),
        ("portnan", "reject", [c.txt("portnan", k("portnan", "")),
                               c.txt("portnan", k("portnan", "eighty"))]),
        ("noname", "reject", [c.txt("noname", [x for x in k("noname") if not x.startswith("name=")])]),
        # A record claiming to be the instance itself.
        ("self", "reject", [c.txt("self", [x if not x.startswith("id=") else
                                           "id=%016x" % c.self_id for x in k("self")])]),
        # A compression pointer at offset 12 pointing at itself.
        ("loop", "reject", [message(record(struct.pack(">H", 0xC000 | 12), TXT, 120,
                                           strings(k("loop"))))]),
        # rdlength past the end of the datagram, and one that lies short.
        ("rdlen", "reject", [c.txt("rdlen", k("rdlen"))[:-1],
                             message(record(c.owner("rdlen"), TXT, 120,
                                            strings(k("rdlen")), rdlen=4000)),
                             message(record(c.owner("rdlen"), TXT, 120,
                                            strings(k("rdlen")), rdlen=3))]),
        # A TXT string that claims 255 bytes and carries ten.
        ("long", "reject", [c.txt("long", [(255, b"name=FUZZlong")] + k("long"))]),
        ("long2", "reject", [c.txt("long2", k("long2")[:2] + [(255, b"x" * 250)] + k("long2")[2:])]),
        # Right name, wrong record type.
        ("type", "reject", [message(record(c.owner("type"), SRV, 120,
                                           struct.pack(">HHH", 0, 0, 41000) + labels("fuzz.local."))),
                            message(record(c.owner("type"), A, 120, socket.inet_aton("10.0.0.1")))]),
    ]

    # Truncation and pure noise: nothing to assert per case beyond survival.
    out.append(("trunc", "reject",
                [b"", b"\0", b"\0\0\0", b"\0" * 11, message(b"", an=5), message(b"", an=0xFFFF),
                 c.valid("trunc")[:12], c.valid("trunc")[:20], c.valid("trunc")[:-4],
                 query()[:8]]))

    # Questions: the instance answers service PTR questions, so these make it
    # announce. A handful only; each one is an extra announce on the group.
    out.append(("ask", "info", [query(), query(rtype=255), query("_meleepcx._udp.local."),
                                query(SERVICE, rtype=TXT)]))

    # Over-long values that are legal DNS but too long for the fields they
    # land in: txt_copy() truncates into name[16]/val[64].
    out.append(("huge", "info", [c.txt("huge", [f"v={c.proto}", f"rev={c.rev}",
                                                "id=" + "f" * 200,
                                                "name=" + "N" * 200,
                                                "port=" + "9" * 200, "state=" + "s" * 200,
                                                "gen=" + "9" * 200])]))
    return out


# ---- the run -------------------------------------------------------------

def route_addr():
    """The local address on the route to the mDNS group: the interface
    net_lan.c's socket joined on, the one our frames must appear to arrive
    on for the kernel's group membership check to pass."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((GROUP, MDNS_PORT))
        return s.getsockname()[0]
    finally:
        s.close()


def sender(addr):
    """TTL 0: delivered to local group members, never put on the wire."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(addr))
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 0)
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
    return s


def listener():
    """Watches the group for the instance's own announces."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                 struct.pack("=4s4s", socket.inet_aton(GROUP), socket.inet_aton("0.0.0.0")))
    s.bind(("0.0.0.0", MDNS_PORT))
    s.setblocking(False)
    return s


def announces(rx, self_id):
    """Datagrams carrying the instance's own id= TXT since the last call."""
    want = b"id=%016x" % self_id
    n = 0
    while True:
        try:
            d = rx.recvfrom(4096)[0]
        except (BlockingIOError, OSError):
            return n
        n += want in d


def lan_lines(log_path, start):
    with open(log_path, "rb") as f:
        f.seek(start)
        return f.read().decode("utf-8", "replace").splitlines()


def table(lines):
    """The peer table as the log describes it: (live names, high-water mark).
    An insert logs "lan: found <name> <ip>:<port>" and a drop logs
    "lan: lost ...", neither with the install id the table is keyed on, and the
    address in the two lines can differ (an entry re-announced with another
    port is updated in place, then logged with the newer one on the way out).
    So entries are counted per name: several ids sharing a name, which the byte
    mutations produce, count separately and still balance."""
    live, high = {}, 0
    for l in lines:
        m = re.search(r"lan: (found|lost) (\S*) ", l)
        if m is None:
            continue
        name = m.group(2)
        live[name] = live.get(name, 0) + (1 if m.group(1) == "found" else -1)
        high = max(high, sum(live.values()))
    return sorted(n for n, c in live.items() if c > 0), high


def run(args):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from net_test import Instance
    shutil.rmtree(args.work, ignore_errors=True)
    os.makedirs(args.work)
    inst = Instance("lanfuzz", args.exe, args.disc, args.work, args.port, args.port + 1,
                    {"MELEE_LAN_TEST": "1"}, True)
    fails, notes = [], []
    try:
        if not inst.wait_log(r"lan: (announcing|lobby without mDNS)", 90):
            print("net_lan_fuzz: the instance never started its lobby")
            return False
        banner = re.search(r"lan: announcing (\S+) id ([0-9a-f]{16}) proto (\S+) rev (\S+)"
                           r"(?: disc \S+)? game port (\d+)", inst.text())
        if banner is None:
            # Do not raise here: if the banner ever changes shape again, the
            # tool must say so, not die in a StopIteration from the fallback.
            why = next((l.strip() for l in inst.text().splitlines()
                        if "lobby without mDNS" in l), None)
            print("net_lan_fuzz: no mDNS socket on this host (" + why + "): nothing to fuzz"
                  if why is not None else
                  "net_lan_fuzz: the lobby announced but the banner did not parse; "
                  "the 'lan: announcing' format changed, fix the regex above")
            return False
        self_id, proto = int(banner.group(2), 16), banner.group(3)
        addr = route_addr()
        print(f"net_lan_fuzz: target {banner.group(1)} id {self_id:016x} proto {proto} "
              f"rev {banner.group(4)} game port {banner.group(5)}, "
              f"frames from {addr} ttl 0 to {GROUP}:{MDNS_PORT}")
        tx, rx = sender(addr), listener()
        start = os.path.getsize(inst.log_path)
        announces(rx, self_id)  # drain
        time.sleep(1.5)
        if announces(rx, self_id) == 0:
            fails.append("the instance was not announcing before the run")

        craft = Craft(proto, self_id)
        seen = {}
        sent = 0
        for tag, expect, frames in cases(craft):
            for d in frames:
                tx.sendto(d, (GROUP, MDNS_PORT))
                sent += 1
                time.sleep(0.02)
            time.sleep(0.5)  # a few of its lobby polls
            lines = lan_lines(inst.log_path, start)
            hit = any("FUZZ" + tag in l for l in lines if l.startswith("lan:") or "lan: " in l)
            seen[tag] = hit
            if expect == "accept" and not hit:
                fails.append(f"{tag}: a record meant to be parsed left no trace "
                             f"({len(frames)} frame(s)); the case is not reaching on_record()")
            if expect == "reject" and hit:
                fails.append(f"{tag}: a malformed record was accepted")
            if expect == "accept" and hit and not args.keep_peers:
                tx.sendto(craft.goodbye(tag), (GROUP, MDNS_PORT))  # withdraw it again
                time.sleep(0.3)

        # Seeded byte mutations of a valid frame. Names are unpredictable here,
        # so this pass asserts survival and the table cap only.
        rng = random.Random(args.seed)
        base = craft.valid("mut")
        for _ in range(args.mutations):
            d = bytearray(base)
            for _ in range(rng.randrange(1, 6)):
                op = rng.randrange(4)
                if op == 0 and d:
                    d[rng.randrange(len(d))] = rng.randrange(256)
                elif op == 1 and len(d) > 13:
                    del d[rng.randrange(12, len(d)):]
                elif op == 2:
                    d += rng.randbytes(rng.randrange(1, 64))
                elif d:
                    i = rng.randrange(len(d))
                    d[i:i] = rng.randbytes(1)
            tx.sendto(bytes(d[:1472]), (GROUP, MDNS_PORT))
            sent += 1
            if sent % 25 == 0:
                time.sleep(0.01)
        t_last = time.time()
        # Nothing crafted may outlive the run: a mutated frame can carry an id
        # we cannot address a goodbye to, so the last entries have to age out on
        # their own (LOST_NS, 5 s, src/pc/net_lan.c:76). Waiting a fixed 5 s + a
        # margin races the instance's own poll, so wait for the table to come
        # back clean and give up at SETTLE_MAX. The same window is the announce
        # check: what it announced during the barrage was drained above, so only
        # datagrams from here on count as "still alive".
        announces(rx, self_id)
        after, waited = 0, 0.0
        while True:
            time.sleep(0.5)
            after += announces(rx, self_id)
            waited = time.time() - t_last
            lines = lan_lines(inst.log_path, start)
            live, high = table(lines)
            if waited >= SETTLE_MIN and not [e for e in live if e.startswith("FUZZ")]:
                break
            if waited >= SETTLE_MAX:
                break
        alive = inst.proc.poll() is None
        bad = [l for l in lines if any(b in l for b in BAD)]
        full = [l for l in lines if "lobby full" in l]
        if not alive:
            fails.append(f"the instance exited (code {inst.proc.poll()})")
        if after < 3:
            fails.append(f"only {after} announces in the {waited:.1f} s after the run "
                         f"(it stopped announcing)")
        if high > MAX_PEERS:
            fails.append(f"the peer table held {high} peers, PC_LAN_MAX_PEERS is {MAX_PEERS}")
        if any("lan: found  " in l for l in lines):
            fails.append("a record with an empty name= was accepted")
        if [e for e in live if e.startswith("FUZZ")]:
            fails.append(f"crafted peers still in the table {waited:.1f} s after the last frame: "
                         f"{live} (they never timed out)")
        if bad:
            fails.append(f"{len(bad)} bad line(s) in the log")
        notes = [l.strip() for l in lines if l.startswith("lan:") or "] lan:" in l]
        parsed = [t for t, hit in seen.items() if hit]
        print(f"  {sent} datagrams, {len(cases(craft))} cases, {args.mutations} mutations; "
              f"pid {inst.proc.pid} {'alive' if alive else 'DEAD'}, {after} announces in the "
              f"{waited:.1f} s after, peer table high-water {high}/{MAX_PEERS} "
              f"({len(full)} lobby-full lines), live now {live}")
        print(f"  parsed into the table: {parsed or 'NOTHING (see below)'}")
        print(f"  refused: {sorted(t for t, hit in seen.items() if not hit)}")
        for l in notes[:24]:
            print("    " + l)
        for l in bad[:6]:
            print("    " + l)
        if not parsed:
            print("  FAIL: not one crafted record reached the peer table. Every frame died "
                  "at a length or name check, so this run proves nothing about the parser.")
            fails.append("no crafted record was parsed")
    finally:
        inst.kill(2.0)
    for f in fails:
        print("  FAIL: " + f)
    return not fails


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=42300, help="the instance's MELEE_NET_PORT")
    ap.add_argument("--exe", default=os.path.join(here, "..", "build", "melee"))
    ap.add_argument("--disc", default=os.path.join(here, "..", "..", "melee.ciso"))
    ap.add_argument("--work", default="/tmp/net_lan_fuzz")
    ap.add_argument("--mutations", type=int, default=400)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--keep-peers", action="store_true",
                    help="skip the goodbye after an accepted record (leaves it to time out)")
    ok = run(ap.parse_args())
    print("net_lan_fuzz: " + ("PASS" if ok else "FAIL"))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
