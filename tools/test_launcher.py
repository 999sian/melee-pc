#!/usr/bin/env python3
"""Exercise real launcher startup, settings, keyboard navigation and close.

Run on the desktop X11/Xwayland display, with DISPLAY/XAUTHORITY set.
Uses private preferences and --no-card. Optional --disc tests UI game handoff.
"""
import argparse
import os
from pathlib import Path
import subprocess
import shlex
import tempfile
import time
from Xlib import X, XK, display, protocol
from Xlib.ext import xtest

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--disc", type=Path)
parser.add_argument("--binary", type=Path, help="Path to executable or AppImage to test (defaults to build/melee)")
parser.add_argument("--screenshots", type=Path)
parser.add_argument("--case", action="append", help="Run only this named case (repeatable)")
parser.add_argument("--native-picker", action="store_true", help="Also test the desktop's Zenity file picker (requires working GTK/X11)")
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent
dpy = display.Display()


def key(win, name, repeat=1):
    for _ in range(repeat):
        if dpy.get_input_focus().focus != win:
            win.set_input_focus(X.RevertToParent, X.CurrentTime)
        code = dpy.keysym_to_keycode(XK.string_to_keysym(name))
        xtest.fake_input(dpy, X.KeyPress, code)
        dpy.sync()
        time.sleep(0.05)
        xtest.fake_input(dpy, X.KeyRelease, code)
        dpy.sync()
        time.sleep(0.35)


def close(win, proc):
    win.send_event(protocol.event.ClientMessage(
        window=win, client_type=dpy.intern_atom("WM_PROTOCOLS"),
        data=(32, [dpy.intern_atom("WM_DELETE_WINDOW"), 0, 0, 0, 0])))
    dpy.sync()
    assert proc.wait(timeout=20) == 0, "launcher shutdown failed"


def screenshot(win, name):
    if args.screenshots:
        args.screenshots.mkdir(parents=True, exist_ok=True)
        subprocess.run(["import", "-window", hex(win.id), str(args.screenshots / f"{name}.png")], check=True)


def resize(win, width, height):
    root_window = dpy.screen().root
    for states in [("_NET_WM_STATE_MAXIMIZED_VERT", "_NET_WM_STATE_MAXIMIZED_HORZ"), ("_NET_WM_STATE_FULLSCREEN", "_NET_WM_STATE_FULLSCREEN")]:
        root_window.send_event(protocol.event.ClientMessage(window=win,
            client_type=dpy.intern_atom("_NET_WM_STATE"),
            data=(32, [0, dpy.intern_atom(states[0]), dpy.intern_atom(states[1]), 1, 0])),
            event_mask=X.SubstructureRedirectMask | X.SubstructureNotifyMask)
    dpy.sync()
    time.sleep(0.3)
    win.configure(width=width, height=height)
    dpy.sync()
    time.sleep(0.5)
    size = win.get_geometry()
    assert (size.width, size.height) == (width, height), "window manager did not apply requested test size"


def run_case(name, arguments, saved_disc=None, interact=None, saved_preferences=""):
    if args.case and name not in args.case:
        return
    with tempfile.TemporaryDirectory(prefix="melee-launcher-") as directory:
        config = Path(directory) / "melee-pc" / "launcher.cfg"
        if saved_disc:
            config.parent.mkdir()
            # C++ std::quoted uses backslash escapes.
            escaped = str(saved_disc).replace("\\", "\\\\").replace('"', '\\"')
            config.write_text(f'disc "{escaped}"\n')
        if saved_preferences:
            config.parent.mkdir(exist_ok=True)
            with config.open("a") as saved:
                saved.write(saved_preferences)
        title = f"launcher-test-{name}-{os.getpid()}"
        env = dict(os.environ, SDL_VIDEO_DRIVER="x11", MELEE_WINDOW_TITLE=title,
                   XDG_DATA_HOME=directory, XDG_CACHE_HOME=directory, MELEE_VSYNC="0")
        if name == "settings-live":
            env.pop("MELEE_VSYNC", None)
        with tempfile.TemporaryFile(mode="w+") as log:
            if name == "relative-cli":
                arguments = [os.path.relpath(args.disc.resolve(), directory)]
            target_bin = str(args.binary.resolve()) if args.binary else str(root / "build/melee")
            proc = subprocess.Popen([target_bin, "--no-card", *arguments], env=env,
                                    cwd=directory, stdout=log, stderr=log)
            try:
                win = None
                for _ in range(200):
                    assert proc.poll() is None, f"{name}: exited before launcher was usable"
                    clients = dpy.screen().root.get_full_property(dpy.intern_atom("_NET_CLIENT_LIST"), X.AnyPropertyType)
                    for wid in clients.value if clients else []:
                        candidate = dpy.create_resource_object("window", wid)
                        if candidate.get_wm_name() == title:
                            win = candidate
                            break
                    if win:
                        break
                    time.sleep(0.1)
                assert win, f"{name}: no window"
                time.sleep(2)
                assert proc.poll() is None, f"{name}: launcher did not stay open"
                if not name.startswith("f1-"):
                    resize(win, 1280, 960)
                screenshot(win, name)
                if interact:
                    interact(win, proc, config)
                else:
                    close(win, proc)
            except BaseException:
                if proc.poll() is None:
                    proc.kill()
                    proc.wait()
                log.seek(0)
                print(log.read())
                raise
    print(f"PASS: {name}")


def settings(win, proc, config):
    # Initial focus: Choose Disc. Verify is disabled, so Down reaches Settings.
    key(win, "Down")
    key(win, "Return")
    time.sleep(0.5)
    key(win, "Right")  # tab strip: Graphics -> Audio & interface
    for _ in range(6):
        key(win, "Down")  # volume -> music -> sfx -> mute -> fps -> scale
    time.sleep(0.5)
    screenshot(win, "settings")
    key(win, "Return")
    time.sleep(0.5)
    assert config.exists() and "scale 1.25" in config.read_text(), "UI scale was not saved"
    screenshot(win, "settings-scaled")
    resize(win, 720, 760)
    time.sleep(0.5)
    screenshot(win, "settings-small")
    key(win, "Escape")
    key(win, "Down", 2)  # discord -> Quit
    key(win, "Return")
    assert proc.wait(timeout=20) == 0, "keyboard Quit failed"


def settings_live(win, proc, config):
    key(win, "Down")
    key(win, "Return")  # open settings, focus lands on the tab strip
    key(win, "Down")    # Display mode
    key(win, "Return")  # Fullscreen
    assert "fullscreen 1" in config.read_text(), "fullscreen setting was not saved"
    key(win, "Return")  # Windowed
    assert "fullscreen 0" in config.read_text(), "windowed setting was not saved"
    key(win, "Down")
    key(win, "Return")  # Vsync off
    assert "vsync 0" in config.read_text(), "VSync toggle was not saved"
    close(win, proc)


def picker_cancel(win, proc, config):
    clients = dpy.screen().root.get_full_property(dpy.intern_atom("_NET_CLIENT_LIST"), X.AnyPropertyType)
    before = set(clients.value if clients else [])
    key(win, "Return")
    popup = None
    for _ in range(150):
        clients = dpy.screen().root.get_full_property(dpy.intern_atom("_NET_CLIENT_LIST"), X.AnyPropertyType)
        for wid in set(clients.value if clients else []) - before:
            candidate = dpy.create_resource_object("window", wid)
            parent = candidate.get_wm_transient_for()
            pid = candidate.get_full_property(dpy.intern_atom("_NET_WM_PID"), X.AnyPropertyType)
            attached = False
            if pid:
                try:
                    command = Path(f"/proc/{pid.value[0]}/cmdline").read_bytes().split(b"\0")
                    attached = b"--attach" in command and hex(win.id).encode() in command
                except OSError:
                    pass
            if (parent and parent.id == win.id) or attached:
                popup = candidate
                break
        if popup:
            break
        time.sleep(0.1)
    assert popup is not None, "native file chooser did not appear"
    screenshot(popup, "native-picker")
    key(popup, "Escape")
    time.sleep(1)
    settings(win, proc, config)


try:
    run_case("first-run", [], interact=settings)
    run_case("invalid-cli", ["/nonexistent/disc.iso"])
    run_case("missing-saved", [], saved_disc="/nonexistent/disc with spaces.iso")
    run_case("settings-live", [], interact=settings_live)
    if args.disc:
        def port_menu(win, proc, config):
            time.sleep(12)
            key(win, "F1")
            time.sleep(1)
            # Focus opens on the tab strip; Down walks the Graphics page.
            key(win, "Down", 3)  # display -> sync -> resolution
            key(win, "Return")
            assert "render_scale 1\n" in config.read_text(), "F1 resolution change was not saved"
            key(win, "Down")  # Aspect ratio
            key(win, "Return")
            assert "widescreen 1\n" in config.read_text(), "widescreen choice was not saved"
            key(win, "Down")  # Anti-aliasing
            key(win, "Return")
            assert "msaa 4\n" in config.read_text(), "MSAA preference was not saved"
            key(win, "Up", 5)  # back up to the tab strip
            key(win, "Right")  # Audio & interface
            key(win, "Down")   # Master volume
            key(win, "Return")
            assert "volume 0\n" in config.read_text(), "master volume was not saved"
            key(win, "Down", 2)  # Mute -> FPS counter
            key(win, "Return")
            assert "fps 1\n" in config.read_text(), "FPS preference was not saved"
            key(win, "Up", 3)  # back to the tab strip
            key(win, "Right")  # Controls
            time.sleep(0.5)
            screenshot(win, "f1-controls")
            key(win, "Left")
            screenshot(win, "f1-enhancements")
            key(win, "Escape")
            key(win, "F1")
            key(win, "F1")
            time.sleep(2)
            assert proc.poll() is None, "game failed after closing F1 menu"
            close(win, proc)
        run_case("f1-menu", [str(args.disc.resolve())], interact=port_menu)
        def restart_menu(win, proc, config):
            time.sleep(12)
            key(win, "F1")
            time.sleep(1)
            key(win, "Down", 3)  # display -> sync -> resolution
            key(win, "Return")
            assert "render_scale 3\n" in config.read_text(), "F1 menu failed after loading saved renderer settings: " + config.read_text()
            screenshot(win, "f1-msaa-restart")
            assert proc.poll() is None, "game failed with saved 4x MSAA settings"
            close(win, proc)
        run_case("f1-restart", [str(args.disc.resolve())], interact=restart_menu,
                 saved_preferences="msaa 4\nanisotropy 8\nrender_scale 2\nvolume 0.4\nfps 1\n")
        def launch(win, proc, config):
            key(win, "Return")
            time.sleep(12)
            assert proc.poll() is None, "game failed after launcher handoff"
            screenshot(win, "gameplay-handoff")
            close(win, proc)
        run_case("ui-handoff", [], saved_disc=args.disc.resolve(), interact=launch)
        def relative_cli(win, proc, config):
            saved = Path(shlex.split(config.read_text().splitlines()[0])[1])
            assert saved.is_absolute() and saved.resolve() == args.disc.resolve(), "relative CLI disc path was not saved as absolute"
            close(win, proc)
        run_case("relative-cli", [], interact=relative_cli)
        def verify_close(win, proc, config):
            key(win, "Down")
            key(win, "Down")
            key(win, "Return")
            close(win, proc)
        run_case("verify-close", [], saved_disc=args.disc.resolve(), interact=verify_close)
    if args.native_picker or (args.case and "picker-cancel" in args.case):
        os.environ.update(SDL_FILE_DIALOG_DRIVER="zenity", GDK_BACKEND="x11", GSK_RENDERER="cairo")
        run_case("picker-cancel", [], interact=picker_cancel)
finally:
    dpy.close()
