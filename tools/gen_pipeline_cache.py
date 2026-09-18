#!/usr/bin/env python3
"""Regenerate tools/initial_pipeline_cache.db.gz, the pipeline seed every
build ships (tools/package_*.sh, CMake `pipeline_cache` target).

The seed is a list of pipeline *configs* (GX register state, blend, depth,
MSAA...), not compiled blobs, so one recorded on any GPU or backend seeds
every other: aurora queues every row for background compilation at startup
and Dawn's own dawn_cache.db holds the per-GPU binaries. Rows are keyed by
(type, xxh3(config)); loading keeps only the current config version, so
re-record after bumping GXPipelineConfigVersion in
extern/aurora/lib/gx/pipeline.hpp.

  tools/gen_pipeline_cache.py                 merge this machine's pipeline_cache.db
  tools/gen_pipeline_cache.py --db PATH       merge a specific pipeline_cache.db
  tools/gen_pipeline_cache.py --dry-run       report counts, write nothing
  tools/gen_pipeline_cache.py --run DISC [--seeds 1,2,3] [--secs 150]
                                              record attract demos first, then merge

Recording needs a display. The game runs from build/ with SDL_VIDEO_DRIVER=x11
and MELEE_VSYNC=0; tools/devctl.py skips the opening movie, the title screen
then plays the attract demo on its own (MELEE_SEED picks fighters and stage),
and the process is stopped after --secs. Everything it compiled is already
committed to pipeline_cache.db (WAL), which is then merged. Coverage of menus,
Classic, and specific characters comes from playing them by hand and running
this tool afterwards without --run.
"""
import argparse
import gzip
import os
import shutil
import signal
import sqlite3
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SEED = os.path.join(ROOT, "tools", "initial_pipeline_cache.db.gz")
SCHEMA = 1  # PipelineCacheSchema in extern/aurora/lib/gfx/pipeline_cache.cpp


def local_db():
    """SDL_GetPrefPath(NULL, "melee-pc")/pipeline_cache.db for this OS."""
    if sys.platform == "win32":
        base = os.path.join(os.environ["APPDATA"], "melee-pc")
    elif sys.platform == "darwin":
        base = os.path.expanduser("~/Library/Application Support/melee-pc")
    else:
        base = os.path.join(os.environ.get("XDG_DATA_HOME", os.path.expanduser("~/.local/share")), "melee-pc")
    return os.path.join(base, "pipeline_cache.db")


def counts(conn, table="pipeline_cache"):
    return {(t, v): n for t, v, n in conn.execute(
        f"SELECT type, config_version, COUNT(*) FROM {table} GROUP BY 1, 2 ORDER BY 1, 2")}


def record(disc, seeds, secs):
    exe = os.path.join(ROOT, "build", "melee")
    env = dict(os.environ, SDL_VIDEO_DRIVER="x11", MELEE_VSYNC="0", MELEE_WINDOW_TITLE="melee-pc-gen")
    for seed in seeds:
        env["MELEE_SEED"] = str(seed)
        print(f"recording attract demo, seed {seed}, {secs}s")
        proc = subprocess.Popen([exe, disc], env=env, cwd=os.path.join(ROOT, "build"),
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            time.sleep(12)
            # Skip the opening movie; the title screen idles into the demo. Synthetic
            # keys are best effort (portal-gated on some desktops) -- the movie ends
            # on its own, it just costs a minute more.
            subprocess.run([sys.executable, os.path.join(ROOT, "tools", "devctl.py"), "key", "Return"],
                           env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            deadline = time.time() + secs
            while time.time() < deadline and proc.poll() is None:
                time.sleep(1)
            if proc.poll() is not None:
                sys.exit(f"melee exited early with {proc.returncode} (seed {seed})")
        finally:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()


def merge(src, dry_run):
    if not os.path.exists(src):
        sys.exit(f"no pipeline cache at {src}")
    work = tempfile.NamedTemporaryFile(suffix=".db", delete=False).name
    try:
        with gzip.open(SEED, "rb") as f, open(work, "wb") as out:
            shutil.copyfileobj(f, out)
        conn = sqlite3.connect(work)
        if conn.execute("SELECT value FROM aurora_schema").fetchone() != (SCHEMA,):
            sys.exit(f"{SEED}: unexpected schema")
        conn.execute("ATTACH DATABASE ? AS src", (src,))
        if conn.execute("SELECT value FROM src.aurora_schema").fetchone() != (SCHEMA,):
            sys.exit(f"{src}: unexpected schema")
        before = counts(conn)
        print("seed before:", before)
        print("source:     ", counts(conn, "src.pipeline_cache"))
        conn.execute(
            "INSERT INTO main.pipeline_cache (type, hash, config_version, config_size, config, first_frame_used) "
            "SELECT type, hash, config_version, config_size, config, first_frame_used FROM src.pipeline_cache WHERE true "
            "ON CONFLICT(type, hash) DO UPDATE SET "
            "config_version = excluded.config_version, config_size = excluded.config_size, config = excluded.config, "
            "first_frame_used = MIN(pipeline_cache.first_frame_used, excluded.first_frame_used)")
        # Rows from an older config version are pruned by the game anyway; do not ship them.
        conn.execute(
            "DELETE FROM main.pipeline_cache WHERE config_version < "
            "(SELECT MAX(config_version) FROM main.pipeline_cache p WHERE p.type = pipeline_cache.type)")
        conn.commit()
        after = counts(conn)
        print("seed after: ", after)
        print(f"rows: {sum(before.values())} -> {sum(after.values())}")
        conn.execute("DETACH DATABASE src")
        conn.execute("VACUUM")
        conn.close()
        if dry_run:
            return
        with open(work, "rb") as f, gzip.GzipFile(SEED, "wb", compresslevel=9, mtime=0) as out:
            shutil.copyfileobj(f, out)
        print(f"wrote {SEED} ({os.path.getsize(SEED)} bytes)")
    finally:
        os.unlink(work)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--db", default=local_db(), help="pipeline_cache.db to merge (default: this machine's)")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--run", metavar="DISC", help="record attract demos from this disc image first")
    ap.add_argument("--seeds", default="1", help="comma-separated MELEE_SEED values for --run")
    ap.add_argument("--secs", type=int, default=150, help="seconds per seed for --run")
    args = ap.parse_args()
    if args.run:
        record(os.path.abspath(args.run), [int(s) for s in args.seeds.split(",")], args.secs)
    merge(args.db, args.dry_run)


if __name__ == "__main__":
    main()
