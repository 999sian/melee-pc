#!/usr/bin/env python3
"""Two-process signed pairing/ranked regression with isolated profiles and UDP."""
import os
import socket
import subprocess
import tempfile
from pathlib import Path

from net_test_support import sdl_includes

ROOT = Path(__file__).resolve().parents[1]


def run():
    with tempfile.TemporaryDirectory(prefix="net-match-") as work:
        executable = Path(work) / "match"
        includes = [ROOT / "extern/aurora/include", ROOT / "src", ROOT / "src/sdk_include",
                    *sdl_includes(ROOT), ROOT / "extern/monocypher"]
        sources = [ROOT / "tools/test_net_match.c"]
        sources += [ROOT / f"src/pc/{name}.c" for name in (
            "net_identity", "net_rank", "net_rank_store", "net_rank_session",
            "libm/pc_rank_exp", "libm/pc_rank_sqrt")]
        sources += [ROOT / name for name in (
            "extern/dht/sha1.c", "extern/monocypher/monocypher.c",
            "extern/monocypher/monocypher-ed25519.c")]
        subprocess.run([
            "cc", "-std=gnu11", "-DTARGET_PC=1", "-DMELEE_PC=1", "-DAURORA",
            *[f"-I{path}" for path in includes], *map(str, sources),
            "-lm", "-o", str(executable)], check=True)

        sockets = [socket.socket(socket.AF_INET, socket.SOCK_DGRAM) for _ in range(2)]
        try:
            for sock in sockets:
                sock.bind(("127.0.0.1", 0))
            ports = [sock.getsockname()[1] for sock in sockets]
        finally:
            for sock in sockets:
                sock.close()
        codes = []
        for player in range(2):
            profile = Path(work) / f"p{player}"
            profile.mkdir()
            codes.append(subprocess.run(
                [executable, "code"], check=True, text=True, capture_output=True,
                env=os.environ | {"MATCH_NAME": f"P{player}", "MATCH_DIR": str(profile)}
            ).stdout.strip())
        processes = []
        try:
            for player in range(2):
                profile = Path(work) / f"p{player}"
                env = os.environ | {"MATCH_NAME": f"P{player}", "MATCH_DIR": str(profile)}
                if os.getenv("MATCH_DIRECT"):
                    # Both players dial each other -- the habit Slippi
                    # teaches -- and player 1 gets the name wrong: only the
                    # key suffix identifies a player now.
                    theirs = codes[1 - player]
                    env["MATCH_DIAL"] = theirs if player else "WRONG" + theirs[theirs.index("#"):]
                processes.append(subprocess.Popen(
                    [executable, str(ports[player]), str(ports[1-player])],
                    env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE))
            output = [process.communicate(timeout=20) for process in processes]
            assert all(process.returncode == 0 for process in processes), output
        finally:
            for process in processes:
                if process.poll() is None:
                    process.kill()
                process.wait()

        if not any(os.getenv(name) for name in (
                "MATCH_PROOF_TIMEOUT", "MATCH_PROOF_MISMATCH", "MATCH_RECOVER")):
            rows = [item[0].strip().split() for item in output]
            assert {row[1] for row in rows} == {"0", "1"}, output
            assert len({row[2] for row in rows}) == 1, output
        profile = Path(work) / "cancel"
        profile.mkdir()
        subprocess.run([executable, "cancel"], check=True, timeout=20,
                       env=os.environ | {"MATCH_NAME": "CANCEL", "MATCH_DIR": str(profile)})
        subprocess.run([executable, "parse"], check=True, timeout=20,
                       env=os.environ | {"MATCH_NAME": "CANCEL", "MATCH_DIR": str(profile)})

    if os.getenv("MATCH_RECOVER"):
        detail = "restart republishes durable immutable history and stale mutable state"
    elif os.getenv("MATCH_COMPLETE"):
        detail = "dual-signed durable set, immutable publication and mutable retry"
    elif os.getenv("MATCH_PROOF_TIMEOUT") or os.getenv("MATCH_PROOF_MISMATCH"):
        detail = "unverified peer state refused before socket handoff"
    elif os.getenv("MATCH_DIRECT"):
        detail = "mutual direct dial by key suffix (one name wrong), contact saved"
    elif os.getenv("MATCH_RANKED"):
        detail = "BEP44 genesis proofs, signed ranked session and READY barrier"
    else:
        detail = "signed pairing, socket handoff, role election and READY barrier"
    print(f"PASS: {detail}; cancellation and same-key name refresh")


if __name__ == "__main__":
    run()
