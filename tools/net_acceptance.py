#!/usr/bin/env python3
"""Netplay acceptance matrix (docs/netcode-plan.md §12): tools/net_test.py once
per case, one markdown table at the end.

    tools/net_acceptance.py [--quick] [--long] [--only "name,name"] [--table]
                            [--minutes N] [--exe build/melee] [--disc ../melee.ciso]
                            [--port 42050] [--work /tmp/net_acceptance]

Link conditions: loss 0/1/5/20 %, one-way delay 50/100/200 ms, burst, reorder,
jitter, dup, asymmetric rx delay 100 ms. Then the cases that are about the flow
rather than the link: "scene flow" walks a whole set (CSS -> SSS -> match ->
results -> rematch) through the LAN lobby and asserts both instances crossed
every stage together; "snapshot oom" fails A's first in-match snapshot
allocation and asserts the session falls back to lockstep and still finishes;
"disconnect" SIGKILLs B mid-match and asserts A times the peer out and keeps
running; "resume 11 s" and "resume expiry 30 s" SIGSTOP B inside and past the
reconnect window (net.c:639-832) and assert it survives the first and ends both
sessions with status 2 on the second. --long adds a 60-minute soak
(loss 1 % + delay 50 ms + jitter).
--quick: four representative link cases at one minute each. --only runs the
named cases (comma-separated, matching the table's case names).

Every case's rows are written to <work>/rows.json as soon as it finishes and
the table always covers the whole matrix, so the matrix can be run in pieces
(--only) and still print as one table; a case that has never run shows as
NOT RUN, and each row carries the minute it took and the build it ran against.
--table prints the stored table and runs nothing. The table is also written to
<work>/acceptance.md; per-case logs are in <work>/<case>/. Exit 1 if any case
that has run failed.
"""
import argparse
import hashlib
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import net_test  # noqa: E402

# name -> net_test.py flags. The flow cases pin their own --minutes: "scene
# flow to SSS" needs room for the boot, the two menu walks into the lobby and
# a CSS sweep that may take several rows, and --quick's one minute would cut
# it off in the lobby.
MATRIX = [
    ("clean", []),
    ("loss 1%", ["--loss", "1"]),
    ("loss 5%", ["--loss", "5"]),
    ("loss 20%", ["--loss", "20"]),
    ("delay 50 ms", ["--delay", "50"]),
    ("delay 100 ms", ["--delay", "100"]),
    ("delay 200 ms", ["--delay", "200"]),
    ("burst", ["--burst"]),
    ("reorder", ["--reorder"]),
    ("jitter", ["--jitter"]),
    ("dup", ["--dup"]),
    ("rx delay 100 ms", ["--rxdelay", "100"]),
    ("scene flow to SSS", ["--scenes", "--minutes", "4"]),
    # 6000 rather than 2400, and loss 5 % rather than a clean link: the OOM
    # knob only fires on a snapshot the engine really takes, snapshots are
    # only taken off the prediction path, and prediction is sporadic on a
    # clean local link - one re-run at --oom 6000 on a clean link predicted
    # exactly 0 times in 3 minutes and the row failed its own "0x out of
    # memory (want exactly 1)" guard. 5 % loss predicts hundreds of times per
    # run and is desync-free (see the `loss 5%` row).
    ("snapshot oom", ["--oom", "6000", "--minutes", "3", "--loss", "5"]),
    ("disconnect", ["--disconnect", "--minutes", "2"]),
    ("resume 11 s", ["--stall", "11", "--minutes", "2"]),
    ("resume expiry 30 s", ["--stall", "30", "--minutes", "2"]),
]

# --quick: one case per class of link fault -- a clean baseline, sustained
# loss, a long trip and reordering -- rather than a sample of the whole
# matrix. Named here because the flag has always referenced this set; without
# it --quick raised NameError and could never have run.
QUICK = ("clean", "loss 5%", "delay 100 ms", "jitter")
# The specified soak is loss 1 % + delay 50 ms + jitter. The delay-free variant
# is not a substitute for it: it is the longest run that CAN complete on this
# build, because every delayed row desyncs within a minute of real play (see
# local/SceneFlowSoak-doc.md §3), and an hour of clean-link match is still the
# longest peer-compared exposure available.
LONG = [("soak 60 min", ["--loss", "1", "--delay", "50", "--jitter", "--minutes", "60"]),
        ("soak 60 min no delay", ["--loss", "1", "--jitter", "--minutes", "60"])]
COLS = ["case", "inst", "result", "min", "frame", "rollbacks", "max depth", "lost", "stalls",
        "worst ms", "ping ms", "jitter ms", "loss %", "quality", "fails"]


def build_id(exe):
    """First 8 hex of the binary's md5: the matrix is run in pieces over hours
    while the tree is being built by other hands, and a table whose rows came
    from two different binaries has to say so."""
    try:
        with open(exe, "rb") as f:
            h = hashlib.md5()
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        return h.hexdigest()[:8]
    except OSError:
        return "?"


def load(path):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--quick", action="store_true", help="4 cases, 1 minute each")
    ap.add_argument("--long", action="store_true", help="add the 60-minute soak")
    ap.add_argument("--minutes", type=float, default=2, help="per case (not the soak)")
    ap.add_argument("--only", help="comma-separated case names to run")
    ap.add_argument("--table", action="store_true", help="print the stored table, run nothing")
    ap.add_argument("--exe")
    ap.add_argument("--disc")
    ap.add_argument("--port", type=int, default=42050)
    ap.add_argument("--work", default="/tmp/net_acceptance")
    args = ap.parse_args()

    every = MATRIX + LONG
    cases = list(every) if args.long else list(MATRIX)
    if args.only:
        want = [s.strip() for s in args.only.split(",")]
        cases = [c for c in every if c[0] in want]
        missing = [w for w in want if w not in [c[0] for c in cases]]
        if missing:
            sys.exit(f"no such case: {', '.join(missing)}")
    elif args.quick:
        cases = [c for c in cases if c[0] in QUICK]
    if args.table:
        cases = []
    minutes = 1 if args.quick else args.minutes
    passthrough = []
    for k in ("exe", "disc"):
        if getattr(args, k):
            passthrough += [f"--{k}", getattr(args, k)]
    os.makedirs(args.work, exist_ok=True)
    store_path = os.path.join(args.work, "rows.json")
    store = load(store_path)
    exe = args.exe or net_test.parse_args([]).exe
    build = build_id(exe)
    t0 = time.time()
    for name, flags in cases:
        argv = flags + passthrough + ["--port", str(args.port), "--work",
                                      os.path.join(args.work, name.replace(" ", "_").replace("%", ""))]
        if "--minutes" not in flags:
            argv += ["--minutes", str(minutes)]
        print(f"\n=== {name}: net_test.py {' '.join(argv)}", flush=True)
        t1 = time.time()
        case_ok, results = net_test.run(net_test.parse_args(argv))
        took = (time.time() - t1) / 60
        store[name] = {
            "ok": case_ok, "took": round(took, 1), "build": build,
            "when": time.strftime("%Y-%m-%d %H:%M"),
            "rows": [{"inst": inst, "fails": fails, "stats": st} for inst, fails, _, st in results],
        }
        with open(store_path, "w") as f:
            json.dump(store, f, indent=1)
        print(f"=== {name}: {'PASS' if case_ok else 'FAIL'} in {took:.1f} min", flush=True)

    rows = []
    ok = True
    builds = set()
    for name, _ in every:
        got = store.get(name)
        if got is None:
            rows.append([name, "-", "NOT RUN"] + ["-"] * (len(COLS) - 4) + ["never run"])
            continue
        ok = ok and got["ok"]
        builds.add(got.get("build", "?"))
        for r in got["rows"]:
            st = r["stats"]
            rows.append([name, r["inst"], "PASS" if got["ok"] else "FAIL", got["took"],
                         st.get("frame", "-"), st.get("rollbacks", "-"), st.get("max_depth", "-"),
                         st.get("lost", "-"), st.get("stalls", "-"), st.get("worst_ms", "-"),
                         st.get("ping", "-"), st.get("jitter", "-"), st.get("loss", "-"),
                         st.get("quality", "-"), "; ".join(r["fails"]) or "-"])

    table = ["| " + " | ".join(COLS) + " |", "|" + "---|" * len(COLS)]
    table += ["| " + " | ".join(str(c) for c in r) + " |" for r in rows]
    text = "\n".join(table) + "\n"
    ran = [n for n, _ in every if n in store]
    print(f"\nnet_acceptance: {len(cases)} cases this run in {(time.time() - t0) / 60:.1f} min, "
          f"{len(ran)}/{len(every)} of the matrix on record, build {'+'.join(sorted(builds))}, "
          f"{'PASS' if ok else 'FAIL'}\n")
    print(text)
    with open(os.path.join(args.work, "acceptance.md"), "w") as f:
        f.write(f"# Netplay acceptance ({time.strftime('%Y-%m-%d %H:%M')})\n\n" + text)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
