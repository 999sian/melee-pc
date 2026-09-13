#!/usr/bin/env python3
"""Ordinary VS widescreen smoke runs using the real scene/GDB harness and private saves."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import signal
p = argparse.ArgumentParser(description=__doc__)
p.add_argument("disc", type=Path)
p.add_argument("--aspect", type=int, choices=(0,1,2), default=1)
p.add_argument("--stage", type=int, default=31)
p.add_argument("--width", type=int, default=1280)
p.add_argument("--height", type=int, default=720)
p.add_argument("--frames", type=int, default=1200)
p.add_argument("--output", type=Path, default=Path("/tmp/melee-widescreen"))
a = p.parse_args()
root = Path(__file__).resolve().parent.parent
out = a.output.resolve(); out.mkdir(parents=True, exist_ok=True)
profile = out / "profile" / "melee-pc"; profile.mkdir(parents=True, exist_ok=True)
shutil.copytree(Path.home()/".local/share/melee-pc/USA", profile/"USA", dirs_exist_ok=True)
(profile/"launcher.cfg").write_text(f"widescreen {a.aspect}\n")
env = dict(os.environ, SDL_VIDEO_DRIVER="x11", MELEE_WINDOW_TITLE="melee-wide-test",
    MELEE_VSYNC="0", MELEE_HEAP_CHECK="1", MELEE_TEST_SINGLE_SHOT="1", MELEE_TEST_WIDE_INFO="1", MELEE_TEST_MODE="2",
    MELEE_TEST_STAGE=str(a.stage), MELEE_TEST_FRAMES=str(a.frames), MELEE_TEST_OUTPUT=str(out),
    MELEE_TEST_WIDTH=str(a.width), MELEE_TEST_HEIGHT=str(a.height),
    XDG_DATA_HOME=str(out/"profile"), XDG_CACHE_HOME=str(out/"cache"))
cmd = ["gdb","-q","-batch","-ex","set pagination off","-ex","set confirm off",
       "-ex","set debuginfod enabled off","-ex","handle SIGUSR1 nostop noprint",
       "-x",str(root/"tools/special_smash_gdb.py"),"--args",str(root/"build/melee"),str(a.disc.resolve())]
with (out/"gdb.log").open("w") as log:
    proc = subprocess.Popen(cmd,cwd=root,env=env,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
    try: proc.wait(timeout=180)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid,signal.SIGINT)
        try: proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid,signal.SIGKILL); proc.wait()
s = (out/"gdb.log").read_text()
ok = "SPECIAL_TEST PASS mode=2 " in s and "exited normally" in s and "received signal" not in s and "Python Exception" not in s
print(("PASS" if ok else "FAIL") + f": aspect={a.aspect} stage={a.stage}; {out/'gdb.log'}")
raise SystemExit(0 if ok else 1)
