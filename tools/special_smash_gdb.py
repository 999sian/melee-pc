"""GDB-side Special Smash smoke test. Loaded by test_special_smash.py.

Uses real boot, CSS/SSS lifecycle and mode callbacks. Debugger writes replace
controller selection only; no production hooks or match callbacks are bypassed.
"""
import gdb
import os
import subprocess

mode = int(os.environ["MELEE_TEST_MODE"])
limit = int(os.environ.get("MELEE_TEST_FRAMES", "1800"))
out = os.environ["MELEE_TEST_OUTPUT"]
match_frames = 0
complete = False
shots = []

def setvar(name, value):
    gdb.execute(f"set var {name} = {value}", to_string=True)

def mark(message):
    print("SPECIAL_TEST " + message, flush=True)

def shot(name):
    # Capturing while GDB has all threads stopped prevents Xwayland's resize
    # refresh from rendering. Let the game continue while the helper captures.
    command = (["import", "-window", "root", out + "/" + name + ".png"]
               if os.environ.get("MELEE_TEST_ROOT_SHOT") == "1" else
               ["python3", "tools/devctl.py", "shot", out + "/" + name + ".png"])
    shots.append(subprocess.Popen(command, stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL))

class Boot(gdb.Breakpoint):
    def stop(self):
        setvar("leave_data.mode_id", mode)
        self.enabled = False
        mark(f"mode={mode} boot complete")
        return False

class CameraIntro(gdb.Breakpoint):
    count = 0
    def stop(self):
        self.count += 1
        if self.count == 60:
            setvar("*gmCamera_VsCameraTextLayout.x0", 0)
            setvar("gm_80479D58.unk_C", 1)
            self.enabled = False
            mark("Camera intro completed")
        return False

class Css(gdb.Breakpoint):
    count = 0
    def stop(self):
        self.count += 1
        if self.count == 40:
            shot("css")  # menus are not widescreen-eligible; proves the fallback
        if self.count < 60:
            return False
        # Set the same selection result that a human token drop produces.
        css = "mnCharSel_804D6CB0->vs.start"
        for i in range(6):
            p = f"{css}.players[{i}]"
            setvar(p + ".slot_type", 1 if i < 2 else 3)
            if i < 2:
                setvar(p + ".ckind", 8 if i == 0 else 2)  # Mario / Fox
                setvar(p + ".cpu_level", 9)
                setvar(p + ".cpu_kind", 4)
                setvar(p + ".stocks", 4)
                setvar(p + ".color", 0)
        setvar(css + ".rules.time_limit", 2)
        setvar("mnCharSel_804D6CF6", 0)
        setvar("gm_80479D58.unk_C", 1)
        self.enabled = False
        mark("CSS completed")
        return False

class Sss(gdb.Breakpoint):
    count = 0
    def stop(self):
        self.count += 1
        if self.count < 60:
            return False
        setvar("sss_data->vs.start.rules.stkind", int(os.environ.get("MELEE_TEST_STAGE", "4")))  # Kongo Jungle, original crash stage
        setvar("sss_data->force_stage_id", int(os.environ.get("MELEE_TEST_STAGE", "-1")))
        setvar("mnStageSel_804D6CAF", 2)
        setvar("gm_80479D58.unk_C", 1)
        self.enabled = False
        mark("SSS completed: selected=" + os.environ.get("MELEE_TEST_STAGE", "4") + " forced=-1")
        return False

class Match(gdb.Breakpoint):
    count = 0
    def stop(self):
        global match_frames, complete
        self.count += 1
        if self.count == 1:
            actual = int(gdb.parse_and_eval("state_machine.routing.curr_mode"))
            if actual != mode:
                raise RuntimeError(f"Wrong mode: expected {mode}, got {actual}")
            mark("MATCH entered")
        match_frames += 1
        if match_frames == 300:
            shot("match")
        if match_frames == limit - 120 and not os.environ.get("MELEE_TEST_SINGLE_SHOT"):
            shot("final")
        if match_frames >= limit:
            mark(f"PASS mode={mode} match_frames={match_frames} completed smoke interval")
            complete = True
            setvar("pc_exit_requested", 1)
            self.enabled = False
        return False

class Scene(gdb.Breakpoint):
    def stop(self):
        global complete
        scene = int(gdb.parse_and_eval("gm_804D6720->scene_kind"))
        if match_frames and not complete and scene in (5, 8):
            outcome = int(gdb.parse_and_eval("controller.match_result"))
            if outcome not in (1, 2, 3):
                mark(f"FAIL unexpected match exit: outcome={outcome} scene={scene}")
            else:
                mark(f"PASS mode={mode} match_frames={match_frames} natural match end, outcome={outcome} scene={scene}")
            complete = True
            setvar("pc_exit_requested", 1)
        return False

class WideCamera(gdb.Breakpoint):
    count = 0
    def stop(self):
        if not bool(gdb.parse_and_eval("s_supported")): return False
        self.count += 1
        if self.count <= 8:
            mark("WIDE " + str(gdb.parse_and_eval("s_mode")) +
                 " camera=" + str(gdb.parse_and_eval("camera->projection_type")) +
                 " aspect=" + str(gdb.parse_and_eval("camera->projection_param.perspective.aspect")))
        if self.count == 8: self.enabled = False
        return False

if os.environ.get("MELEE_TEST_WIDE_INFO"):
    WideCamera("pc_widescreen_apply_camera")
    class WideViewport(gdb.Breakpoint):
        count = 0
        def stop(self):
            if not bool(gdb.parse_and_eval("'widescreen.c'::s_supported")): return False
            self.count += 1
            mark("VIEWPORT " + ",".join(str(gdb.parse_and_eval(v)) for v in ("left","top","wd","ht")))
            if self.count == 8: self.enabled = False
            return False
    WideViewport("GXSetViewportRender")

class WindowSize(gdb.Breakpoint):
    def stop(self):
        setvar("config->windowWidth", int(os.environ["MELEE_TEST_WIDTH"]))
        setvar("config->windowHeight", int(os.environ.get("MELEE_TEST_HEIGHT", "720")))
        self.enabled = False
        return False

if "MELEE_TEST_WIDTH" in os.environ:
    WindowSize("aurora_initialize")

class SoftwareRenderer(gdb.Breakpoint):
    def stop(self):
        setvar("config->allowCpuAdapter", 1)
        self.enabled = False
        return False

if os.environ.get("MELEE_TEST_SOFTWARE") == "1":
    SoftwareRenderer("aurora_initialize")
Boot("bootOnLeave")
CameraIntro("gm_Scene_CameraVs_OnFrame")
Css("mnCharSel_Scene_OnFrame")
Sss("mnStageSel_Scene_OnFrame")
Match("gm_Scene_Vs_OnFrame")
Scene("gm_801A4D34")
gdb.execute("run")
gdb.execute("thread apply all bt 15")
for job in shots:
    job.wait(timeout=15)
