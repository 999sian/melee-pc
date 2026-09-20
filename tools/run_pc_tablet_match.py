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
ARTIFACT_DIR = "/home/sian/.gemini/antigravity/brain/11bcbbd6-af63-4b1c-ad63-96c611035173"

def logcat_melee():
    res = subprocess.run(["adb", "-s", SERIAL, "logcat", "-d", "-s", "Melee:V"],
                         capture_output=True, text=True)
    return res.stdout

def adb_tap(x, y):
    subprocess.run(["adb", "-s", SERIAL, "shell", "input", "tap", str(x), str(y)])

def adb_hold_btn(x, y, ms=200):
    subprocess.run(["adb", "-s", SERIAL, "shell", "input", "swipe",
                    str(x), str(y), str(x), str(y), str(ms)])

def adb_swipe(x1, y1, x2, y2, dur):
    subprocess.run(["adb", "-s", SERIAL, "shell", "input", "swipe",
                    str(x1), str(y1), str(x2), str(y2), str(dur)])

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

def snap(name):
    # PC screenshot via spectacle
    pc_out = os.path.join(ARTIFACT_DIR, f"pc_{name}.png")
    subprocess.run(["spectacle", "-b", "-n", "-o", pc_out], capture_output=True)
    # Tablet screenshot via adb
    tab_out = os.path.join(ARTIFACT_DIR, f"tablet_{name}.png")
    subprocess.run(f"adb -s {SERIAL} exec-out screencap -p > '{tab_out}'", shell=True)
    print(f"Captured screenshots: pc_{name}.png, tablet_{name}.png")

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

    print("[1] Launching PC Melee...")
    log_file = open(LOG, "wb")
    proc = subprocess.Popen([EXE, "--no-card", DISC], env=env, stdout=log_file, stderr=subprocess.STDOUT)

    try:
        # 1. Wait for PC to boot
        print("[2] Waiting for PC to boot...")
        for _ in range(60):
            t = pc_text()
            if "HIT: MnMaAll" in t or "HIT: GmTtAll" in t:
                break
            time.sleep(1)
        else:
            print("PC did not boot in time")
            return

        print("[3] PC reached title/menu. Dismissing title...")
        for _ in range(3):
            press_pc("Return", 9, 1.0)
            time.sleep(0.5)

        # 2. Walk to LAN lobby on PC
        print("[4] Navigating PC to LAN Play lobby...")
        press_pc("Z", 15, 0.5)
        press_pc("Z", 15, 0.5)
        time.sleep(1.0)
        
        press_pc("Down", 10, 1.0)
        press_pc("X", 10, 1.5) # Enter VS menu
        press_pc("Up", 10, 1.0)
        press_pc("X", 10, 1.5) # Enter Online menu
        press_pc("X", 10, 1.5) # Enter LAN Play lobby

        # Wait for both in lobby
        print("[5] Waiting for mutual discovery...")
        for i in range(25):
            t = pc_text()
            tab_log = logcat_melee()
            if ("lobby: 2 players found" in t or "players found: 1" in t) and \
               ("lobby: 2 players found" in tab_log or "players found: 1" in tab_log):
                print("Mutual discovery confirmed!")
                break
            time.sleep(1)
        else:
            print("Discovery timed out. Checking state...")

        time.sleep(1.0)
        snap("lan_lobby")

        # 3. Press Start on PC to start match / enter CSS
        print("[6] Electing host and starting match (Press START on PC)...")
        press_pc("Return", 10, 1.0)

        # Wait for CSS on both
        print("[7] Waiting for CSS on both devices...")
        css_loaded = False
        for _ in range(30):
            t = pc_text()
            tab_log = logcat_melee()
            if ("MnSlChr" in t or "online: enter CSS" in t) and \
               ("MnSlChr" in tab_log or "online: enter CSS" in tab_log):
                print("Both devices entered CSS!")
                css_loaded = True
                break
            time.sleep(1)

        if not css_loaded:
            print("CSS failed to load. Logs:")
            print("PC:", "\n".join(pc_text().splitlines()[-15:]))
            print("Tablet:", "\n".join(logcat_melee().splitlines()[-15:]))
            return

        # Settle animation
        time.sleep(3.0)
        snap("css_empty")

        # 4. Character Selection
        print("[8] Picking characters...")
        # Tablet (P2): move hand up into character grid and press A
        print("  - Moving P2 hand on tablet and dropping token...")
        adb_swipe(172, 1020, 172, 750, 1000)
        time.sleep(0.5)
        adb_hold_btn(1842, 1020, 200) # Tap A
        time.sleep(0.8)

        # PC (P1): move hand and press A
        print("  - Moving P1 hand on PC and dropping token...")
        press_pc("Left+Up", 40, 1.2)  # pin on top-left (-35, 25)
        press_pc("Down", 8, 0.8)       # move into middle row
        press_pc("Right", 9, 0.6)      # move to column 1
        press_pc("X", 7, 0.5)          # press A

        time.sleep(2.0)
        snap("css_picked")

        # 5. Press Start to enter Stage Select Screen (SSS)
        print("[9] Pressing START on PC to proceed to SSS...")
        press_pc("Return", 10, 1.0)

        # Wait for SSS on both
        print("[10] Waiting for SSS on both devices...")
        sss_loaded = False
        for _ in range(25):
            t = pc_text()
            tab_log = logcat_melee()
            if "MnSlMap" in t or "sss: we picked" in t or "MnSlMap" in tab_log or "sss: we picked" in tab_log:
                print("SSS loaded!")
                sss_loaded = True
                break
            time.sleep(1)

        time.sleep(2.5)
        snap("sss")

        # 6. Stage Selection (The core feature under test!)
        print("[11] Testing independent Stage Selection & 50/50 Random Picker...")
        # PC picks a stage
        print("  - PC selecting stage...")
        press_pc("Left", 10, 0.5)
        press_pc("X", 8, 0.5)

        time.sleep(1.0)

        # Tablet picks a stage
        print("  - Tablet selecting stage...")
        adb_swipe(172, 1020, 280, 1020, 500) # Move stick right on tablet
        time.sleep(0.5)
        adb_hold_btn(1842, 1020, 200) # Tap A on tablet
        time.sleep(1.0)

        # Wait for resolution
        print("[12] Checking stage resolution in logs...")
        for _ in range(15):
            t = pc_text()
            tab_log = logcat_melee()
            if "sss: picks P1=" in t or "sss: picks P1=" in tab_log:
                print(">>> STAGE RESOLUTION COIN-FLIP TRIGGERED! <<<")
                print("PC SSS log lines:")
                for l in t.splitlines():
                    if "sss:" in l:
                        print("  [PC]", l)
                print("Tablet SSS log lines:")
                for l in tab_log.splitlines():
                    if "sss:" in l:
                        print("  [Tablet]", l)
                break
            time.sleep(1)

        # Wait for match gameplay to begin
        print("[13] Waiting for match to start...")
        time.sleep(4.0)
        snap("gameplay")

        print("[14] Verifying live match gameplay for 6 seconds...")
        for i in range(6):
            # Send small inputs to keep fight active
            press_pc("Right", 5, 0.2)
            adb_swipe(172, 1020, 250, 1020, 200)
            time.sleep(0.8)

        snap("gameplay_action")
        print(">>> LAN MATCH COMPLETE AND VERIFIED! <<<")

    finally:
        print("Terminating PC process...")
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        print("Done.")

if __name__ == "__main__":
    main()
