#!/usr/bin/env python3
"""Netplay acceptance matrix (docs/netcode-plan.md §12): tools/net_test.py once
per link condition, one markdown table at the end.

    tools/net_acceptance.py [--quick] [--long] [--minutes N] [--exe build/melee]
                            [--disc ../melee.ciso] [--port 42050]
                            [--work /tmp/net_acceptance]

Full matrix: loss 0/1/5/20 %, one-way delay 50/100/200 ms, burst, reorder,
jitter, dup, asymmetric rx delay 100 ms; --long adds a 60-minute soak (loss 1 %
+ delay 50 ms + jitter). --quick: four representative cases at one minute each.
The table (one row per instance) is printed and written to <work>/acceptance.md;
per-case logs are in <work>/<case>/. Exit 1 if any case fails.
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import net_test  # noqa: E402

# name -> net_test.py flags
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
    ap.add_argument("--exe")
    ap.add_argument("--disc")
    ap.add_argument("--port", type=int, default=42050)
    ap.add_argument("--work", default="/tmp/net_acceptance")
    args = ap.parse_args()

    cases = [c for c in MATRIX if not args.quick or c[0] in QUICK]
    if args.long:
        cases.append(LONG)
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
