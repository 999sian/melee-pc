"""Stadium completion tests: real scene lifecycle with debugger setup.

Home-Run sends a real smash after setting position/damage. Other scenarios
inject a final-target, final-wave, timer or blast-zone boundary and require
normal mode completion. These are completion-path tests, not full playthroughs.
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
    print("STADIUM_TEST " + message, flush=True)

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

class Css(gdb.Breakpoint):
    count = 0
    def stop(self):
        self.count += 1
        if self.count < 60:
            return False
        for i in range(6):
            setvar(f"mnCharSel_804D6CB0->vs.start.players[{i}].ckind", 8)
        setvar("mnCharSel_804D6CB0->pending_scene_change", 0)
        setvar("mnCharSel_804D6CF6", 0)
        setvar("gm_80479D58.unk_C", 1)
        self.enabled = False
        mark("CSS completed")
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
        if mode == 32 and os.environ.get("MELEE_TEST_HIT") == "1" and match_frames == 100:
            setvar("((Fighter*)player_slots[0].player_entity[0]->user_data)->cur_pos.x", -7)
            setvar("((Fighter*)player_slots[1].player_entity[0]->user_data)->dmg.x1830_percent", 150)
            shots.append(subprocess.Popen(["python3", "tools/stadium_input.py"], stdout=subprocess.DEVNULL))
        if mode != 32 and match_frames == 300:
            if os.environ.get("MELEE_TEST_SCENARIO") == "loss" or mode in (37, 38):
                setvar("((Fighter*)player_slots[0].player_entity[0]->user_data)->cur_pos.y", -1000)
                mark("Injected player below blast zone; awaiting real loss handling")
            elif mode == 15:
                setvar("stage_info.x6D4", 0)
                setvar("stage_info.flags", int(gdb.parse_and_eval("stage_info.flags")) | 0x20)
                mark("Injected final-target cleared state; awaiting completion handling")
            elif mode in (35, 36):
                setvar("controller.timer_seconds", 1)
                setvar("controller.unk_2C", 0)
                mark("Timer advanced to final second")
            elif mode in (33, 34):
                count = 10 if mode == 33 else 100
                for i in range(count):
                    setvar(f"lbl_80472ED8.x54[{i}].x0", -2)
                for i in range(1, 6):
                    setvar(f"player_slots[{i}].falls[0]", 1)
                mark("Final wave defeated; awaiting real completion callback")
        if match_frames % 300 == 0:
            expressions = ["controller.timer_seconds", "controller.match_result", "stage_info.flags"]
            if mode == 32:
                expressions += ["lbl_80472E48", "lbl_80472EC8", "((Fighter*)player_slots[1].player_entity[0]->user_data)->dmg.x1830_percent"]
            for expr in expressions:
                mark(f"{expr}={gdb.parse_and_eval(expr)}")
        if match_frames == 600:
            shot("result")
        if match_frames == 300:
            shot("match")
        if match_frames == limit - 120:
            shot("final")
        if match_frames >= limit:
            mark(f"FAIL mode={mode} match_frames={match_frames} did not finish")
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
            if mode == 32 and os.environ.get("MELEE_TEST_HIT") == "1":
                assert int(gdb.parse_and_eval("lbl_80472EC8[0]")) > 0, "Hit did not produce a distance"
                assert int(gdb.parse_and_eval("lbl_80472E48.b32")) == 1, "Distance did not settle"
            loss = os.environ.get("MELEE_TEST_SCENARIO") == "loss" or mode in (37, 38)
            expected = ({9} if mode in (37, 38) else {4}) if loss else ({6} if mode == 15 else {9})
            if loss:
                assert int(gdb.parse_and_eval("player_slots[0].falls[0]")) > 0, "Player loss was not recorded"
            if not loss and mode in (33, 34, 35, 36):
                assert int(gdb.parse_and_eval("lbl_80472ED8.x0")) == 1, "Stadium victory was not recorded"
            if outcome not in expected:
                mark(f"FAIL unexpected match exit: outcome={outcome} scene={scene}")
            else:
                mark(f"PASS mode={mode} match_frames={match_frames} completed via game callbacks, outcome={outcome} scene={scene}")
            complete = True
            setvar("pc_exit_requested", 1)
        return False

class SoftwareRenderer(gdb.Breakpoint):
    def stop(self):
        setvar("config->allowCpuAdapter", 1)
        self.enabled = False
        return False

if os.environ.get("MELEE_TEST_SOFTWARE") == "1":
    SoftwareRenderer("aurora_initialize")
Boot("bootOnLeave")
Css("mnCharSel_Scene_OnFrame")
Match("gm_Scene_Vs_OnFrame")
Scene("gm_801A4D34")
gdb.execute("run")
gdb.execute("thread apply all bt 15")
for job in shots:
    job.wait(timeout=15)
