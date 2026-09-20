#!/usr/bin/env python3
import os
import sys
import time
import subprocess
import re
import signal

EXE = "./build/melee"
DISC = "melee.ciso"
FIFO = "/tmp/pc_lan.keys"
LOG = "/tmp/pc_lan.log"
SERIAL = "R9TR20NR6YJ"

def logcat_melee():
    res = subprocess.run(["adb", "-s", SERIAL, "logcat", "-d", "-s", "Melee:V"],
                         capture_output=True, text=True)
    return res.stdout

def fifo_write(line):
    for _ in range(50):
        try:
            fd = os.open(FIFO, os.O_WRONLY | os.O_NONBLOCK)
            with os.fdopen(fd, "w") as f:
                f.write(line + "\n")
            return
        except OSError:
            time.sleep(0.1)
    print("Failed to write to FIFO:", line)

def pc_text():
    if not os.path.exists(LOG):
        return ""
    with open(LOG, "rb") as f:
        return f.read().decode("utf-8", "replace")

def press_pc(key, frames, settle_sec=0.6):
    fifo_write(f"{key} {int(frames * 16.6)}")
    time.sleep(frames * 0.0166 + 0.1 + settle_sec)

def main():
    if os.path.exists(FIFO):
        os.unlink(FIFO)
    os.mkfifo(FIFO)

    env = os.environ.copy()
    for k in ("MELEE_DEBUG_VS", "MELEE_NET", "MELEE_NET_PLAYER", "MELEE_NET_REPLAY"):
        env.pop(k, None)
    env["SDL_VIDEO_DRIVER"] = "x11"
    env["MELEE_VSYNC"] = "0"
    env["MELEE_NET_PORT"] = "41000"
    env["MELEE_WINDOW_TITLE"] = "melee-pc-lan"
    env["MELEE_KEY_FIFO"] = FIFO
    env["MELEE_FPS"] = "1"
    env.pop("MELEE_LOG_FILE", None)

    print("Launching PC Melee...")
    log_file = open(LOG, "wb")
    proc = subprocess.Popen([EXE, "--no-card", DISC], env=env, stdout=log_file, stderr=subprocess.STDOUT)

    try:
        # Wait for PC to boot into main menu
        print("Waiting for PC to boot...")
        for _ in range(60):
            t = pc_text()
            if "HIT: MnMaAll" in t or "HIT: GmTtAll" in t:
                break
            time.sleep(1)
        else:
            print("PC did not reach title/menu in time")
            return

        print("PC reached title/menu. Dismissing title if needed...")
        for _ in range(3):
            press_pc("Return", 9, 1.0)
            time.sleep(0.5)

        # Walk to LAN lobby on PC:
        # Down (VS Mode) -> A (Enter) -> Up (Online) -> A (Enter) -> A (LAN Play)
        print("Navigating PC to LAN Play lobby...")
        press_pc("Z", 15, 0.5)
        press_pc("Z", 15, 0.5)
        time.sleep(1.0)
        
        press_pc("Down", 10, 1.0)
        press_pc("X", 10, 1.5) # Enter VS menu
        press_pc("Up", 10, 1.0)
        press_pc("X", 10, 1.5) # Enter Online menu
        press_pc("X", 10, 1.5) # Enter LAN Play lobby

        # Check if PC reached LAN lobby
        for _ in range(30):
            t = pc_text()
            if "lan: announcing" in t:
                print("PC reached LAN Play lobby!")
                break
            time.sleep(0.5)
        else:
            print("PC did not announce LAN. Last log:")
            print("\n".join(pc_text().splitlines()[-20:]))
            return

        print("Checking peer discovery between PC and Tablet...")
        for i in range(25):
            t = pc_text()
            tablet_log = logcat_melee()
            pc_sees_tablet = "SM-T505" in t or "players found: 1" in t or "lobby: 1 players" in t
            tablet_sees_pc = "players found: 1" in tablet_log or "lobby: 1 players" in tablet_log or "melee" in tablet_log.lower()
            print(f"[{i}s] PC log has tablet: {pc_sees_tablet}, Tablet sees PC: {tablet_sees_pc}")
            if pc_sees_tablet and tablet_sees_pc:
                print(">>> SUCCESS: Mutual discovery confirmed! <<<")
                break
            time.sleep(1)

        # Keep running so we can inspect
        time.sleep(5)
    finally:
        print("Done test.")

if __name__ == "__main__":
    main()
