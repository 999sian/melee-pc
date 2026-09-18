#!/usr/bin/env python3
"""Netplay acceptance matrix (docs/netcode-plan.md §12): tools/net_test.py once
per case, one markdown table at the end.

    tools/net_acceptance.py [--quick] [--long] [--only "name,name"] [--minutes N]
                            [--exe build/melee] [--disc ../melee.ciso] [--port 42050]
                            [--work /tmp/net_acceptance]

Link conditions: loss 0/1/5/20 %, one-way delay 50/100/200 ms, burst, reorder,
jitter, dup, asymmetric rx delay 100 ms. Then the three cases that are about
the flow rather than the link: "scene flow" walks a whole set (CSS -> SSS ->
match -> results -> rematch) through the LAN lobby and asserts both instances
crossed every stage together; "snapshot oom" fails A's first in-match snapshot
allocation and asserts the session falls back to lockstep and still finishes;
"disconnect" SIGKILLs B mid-match and asserts A times the peer out and keeps
running. --long adds a 60-minute soak (loss 1 % + delay 50 ms + jitter).
--quick: four representative link cases at one minute each. --only runs the
named cases (comma-separated, matching the table's case names).
The table (one row per instance) is printed and written to <work>/acceptance.md;
per-case logs are in <work>/<case>/. Exit 1 if any case fails.
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import net_test  # noqa: E402

# name -> net_test.py flags. The three flow cases pin their own --minutes: the
# scene drive needs room for two matches and results, and --quick's one minute
# would cut the set in half.
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
    ("scene flow", ["--scenes", "--minutes", "3"]),
    ("snapshot oom", ["--oom", "2400", "--minutes", "2"]),
    ("disconnect", ["--disconnect", "--minutes", "2"]),
]
QUICK = ["clean", "loss 5%", "delay 100 ms", "jitter"]
LONG = ("soak 60 min", ["--loss", "1", "--delay", "50", "--jitter", "--minutes", "60"])
COLS = ["case", "inst", "result", "frame", "rollbacks", "max depth", "lost", "stalls", "worst ms",
        "ping ms", "jitter ms", "loss %", "quality", "fails"]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--quick", action="store_true", help="4 cases, 1 minute each")
    ap.add_argument("--long", action="store_true", help="add the 60-minute soak")
    ap.add_argument("--minutes", type=float, default=2, help="per case (not the soak)")
    ap.add_argument("--only", help="comma-separated case names to run")
    ap.add_argument("--exe")
    ap.add_argument("--disc")
    ap.add_argument("--port", type=int, default=42050)
    ap.add_argument("--work", default="/tmp/net_acceptance")
    args = ap.parse_args()

    cases = MATRIX + [LONG] if args.long else list(MATRIX)
    if args.only:
        want = [s.strip() for s in args.only.split(",")]
        cases = [c for c in cases if c[0] in want]
        missing = [w for w in want if w not in [c[0] for c in cases]]
        if missing:
            sys.exit(f"no such case: {', '.join(missing)}")
    elif args.quick:
        cases = [c for c in cases if c[0] in QUICK]
    minutes = 1 if args.quick else args.minutes
    passthrough = []
    for k in ("exe", "disc"):
        if getattr(args, k):
            passthrough += [f"--{k}", getattr(args, k)]
    os.makedirs(args.work, exist_ok=True)
    rows = []
    ok = True
    t0 = time.time()
    for name, flags in cases:
        argv = flags + passthrough + ["--port", str(args.port), "--work",
                                      os.path.join(args.work, name.replace(" ", "_").replace("%", ""))]
        if "--minutes" not in flags:
            argv += ["--minutes", str(minutes)]
        print(f"\n=== {name}: net_test.py {' '.join(argv)}", flush=True)
        case_ok, results = net_test.run(net_test.parse_args(argv))
        ok = ok and case_ok
        for inst, fails, _, st in results:
            rows.append([name, inst, "PASS" if case_ok else "FAIL", st.get("frame", "-"),
                         st.get("rollbacks", "-"), st.get("max_depth", "-"), st.get("lost", "-"),
                         st.get("stalls", "-"), st.get("worst_ms", "-"), st.get("ping", "-"),
                         st.get("jitter", "-"), st.get("loss", "-"), st.get("quality", "-"),
                         "; ".join(fails) or "-"])
        print(f"=== {name}: {'PASS' if case_ok else 'FAIL'}", flush=True)

    table = ["| " + " | ".join(COLS) + " |", "|" + "---|" * len(COLS)]
    table += ["| " + " | ".join(str(c) for c in r) + " |" for r in rows]
    text = "\n".join(table) + "\n"
    print(f"\nnet_acceptance: {len(cases)} cases in {(time.time() - t0) / 60:.1f} min, "
          f"{'PASS' if ok else 'FAIL'}\n")
    print(text)
    with open(os.path.join(args.work, "acceptance.md"), "w") as f:
        f.write(f"# Netplay acceptance ({time.strftime('%Y-%m-%d %H:%M')})\n\n" + text)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
