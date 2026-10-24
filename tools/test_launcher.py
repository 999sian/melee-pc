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
import shutil
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
parser.add_argument("--send-event", action="store_true",
                    help="Deliver keys with XSendEvent instead of XTest (XTest keys never reach the window under KWin's Xwayland)")
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent
dpy = display.Display()


def key(win, name, repeat=1):
    if args.send_event:
        code = dpy.keysym_to_keycode(XK.string_to_keysym(name))
        for _ in range(repeat):
            for kind in (protocol.event.KeyPress, protocol.event.KeyRelease):
                win.send_event(kind(time=X.CurrentTime, root=dpy.screen().root, window=win, same_screen=1,
                                    child=X.NONE, root_x=0, root_y=0, event_x=0, event_y=0, state=0, detail=code),
                               propagate=True)
                dpy.sync()
                time.sleep(0.05)
            time.sleep(0.35)
        return
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
        portable_binary = None
        if name == "portable-settings":
            install = Path(directory) / "portable game"
            install.mkdir()
            portable_binary = install / "melee"
            shutil.copy2(args.binary or root / "build/melee", portable_binary)
            (install / "resources").symlink_to(root / "resources", target_is_directory=True)
            (install / "portable.txt").touch()
            config = install / "user" / "launcher.cfg"
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
        if name == "f1-keyboard-bindings":
            fifo = Path(directory) / "input.fifo"
            os.mkfifo(fifo)
            env.update(MELEE_INPUT_TRACE="1", MELEE_LOG_FILE=str(Path(directory) / "input.log"),
                       MELEE_KEY_FIFO=str(fifo))
        if name in ("settings-live", "portable-settings"):
            env.pop("MELEE_VSYNC", None)
        with tempfile.TemporaryFile(mode="w+") as log:
            if name == "relative-cli":
                arguments = [os.path.relpath(args.disc.resolve(), directory)]
            target_bin = str(portable_binary) if portable_binary else (str(args.binary.resolve()) if args.binary else str(root / "build/melee"))
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
                if name == "portable-settings":
                    assert config.exists(), "portable settings were not saved beside the executable"
                    assert not (Path(directory) / "melee-pc").exists(), "portable mode wrote to the normal user profile"
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
    key(win, "Down", 5)  # categories: Graphics -> ... -> Launcher, which opens its page
    key(win, "Right")    # into the rows: Interface size
    time.sleep(0.5)
    screenshot(win, "settings")
    key(win, "Return")
    time.sleep(0.5)
    assert config.exists() and "scale 1.25" in config.read_text(), "UI scale was not saved"
    screenshot(win, "settings-scaled")
    resize(win, 720, 760)
    time.sleep(0.5)
    screenshot(win, "settings-small")
    key(win, "Escape")  # rows -> categories
    key(win, "Escape")  # categories -> main menu, on Settings
    key(win, "Down", 2)  # Discord -> Quit
    key(win, "Return")
    assert proc.wait(timeout=20) == 0, "keyboard Quit failed"


def settings_live(win, proc, config):
    key(win, "Down")
    key(win, "Return")  # open settings, focus lands on Graphics
    key(win, "Right")   # Display mode
    key(win, "Return")  # Fullscreen
    assert "fullscreen 1" in config.read_text(), "fullscreen setting was not saved"
    key(win, "Return")  # Windowed
    assert "fullscreen 0" in config.read_text(), "windowed setting was not saved"
    key(win, "Down")
    key(win, "Return")  # Vsync off
    assert "vsync 0" in config.read_text(), "VSync toggle was not saved"
    key(win, "Down", 2)  # Internal resolution -> Aspect ratio
    key(win, "Left")     # steps back: Original -> Auto (window aspect)
    assert "widescreen 2" in config.read_text(), "Left did not step the aspect ratio back"
    key(win, "Right")    # and forward again
    assert "widescreen 0" in config.read_text(), "Right did not step the aspect ratio"
    key(win, "Down", 7)  # Aspect ratio -> Graphics backend
    if "MELEE_BACKEND" not in os.environ:
        key(win, "Left")
        assert "backend 2\n" in config.read_text(), "Left did not wrap Auto to Vulkan"
        key(win, "Right")
        assert "backend 0\n" in config.read_text(), "Right did not wrap Vulkan to Auto"
        key(win, "Return")
        assert "backend 2\n" in config.read_text(), "Click did not advance the backend"
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
    run_case("portable-settings", [], interact=settings_live)
    if args.disc:
        def keyboard_bindings(win, proc, config):
            time.sleep(12)
            key(win, "F1")
            key(win, "Down", 3)
            key(win, "Right")
            key(win, "Left")  # Port 1 -> Keyboard
            key(win, "Down")  # A
            key(win, "Return")
            key(win, "p")
            assert "keys 19 " in config.read_text(), "keyboard A binding was not saved"
            screenshot(win, "f1-keyboard-bound")
            key(win, "F1")
            trace = config.parent.parent / "input.log"
            before = trace.read_text()
            # Feed the same scancode through the existing test input source;
            # XSendEvent navigates RmlUi but cannot grant SDL keyboard focus.
            descriptor = os.open(config.parent.parent / "input.fifo", os.O_WRONLY | os.O_NONBLOCK)
            try:
                os.write(descriptor, b"P 200\n")
            finally:
                os.close(descriptor)
            time.sleep(0.5)
            assert "pad: btn 0100 " in trace.read_text()[len(before):], "remapped key did not publish GameCube A"
            key(win, "F1")
            key(win, "Right")
            key(win, "Down")
            key(win, "Return")
            key(win, "Delete")
            assert "keys 0 " in config.read_text(), "keyboard A could not be unbound"
            key(win, "Down", 22)  # remaining buttons + axes -> Restore defaults
            key(win, "Return")
            assert "keys 27 29 6 25 " in config.read_text(), "keyboard defaults were not restored"
            close(win, proc)
        run_case("f1-keyboard-bindings", [str(args.disc.resolve())], interact=keyboard_bindings)

        def quit_menu(win, proc, config):
            time.sleep(12)
            key(win, "F1")
            time.sleep(1)
            key(win, "Up")  # last sidebar action: Quit game
            key(win, "Return")
            assert proc.wait(timeout=12) == 0, "F1 Quit did not shut down cleanly"
        run_case("f1-quit", [str(args.disc.resolve())], interact=quit_menu)

        def category_navigation(win, proc, config):
            time.sleep(12)
            key(win, "F1")
            time.sleep(1)
            key(win, "Down")  # Graphics -> Audio in the vertical category list
            key(win, "Right")  # enter Master volume
            key(win, "Left")
            assert "volume 0.95\n" in config.read_text(), "Down did not select Audio and Right enter its settings"
            screenshot(win, "f1-keyboard-audio")
            key(win, "Escape")  # rows -> Audio category
            key(win, "Down")    # Gameplay
            key(win, "Return")  # enter Unlock everything
            key(win, "Return")
            assert "unlock_all 1\n" in config.read_text(), "Down did not select Gameplay and Enter enter its settings"
            screenshot(win, "f1-keyboard-gameplay")
            key(win, "Escape")
            key(win, "Down")  # Controls
            key(win, "Right")
            screenshot(win, "f1-keyboard-controls")
            key(win, "Escape")
            key(win, "Up", 2)  # Controls -> Gameplay -> Audio
            key(win, "Right")
            key(win, "Right")
            assert "volume 1\n" in config.read_text(), "Up did not return to Audio"
            key(win, "Escape")
            key(win, "Escape")  # category -> game
            close(win, proc)
        run_case("f1-category-navigation", [str(args.disc.resolve())], interact=category_navigation)

        def tab_wrap(win, proc, config):
            time.sleep(12)
            key(win, "F1")
            time.sleep(1)
            for page in ("audio", "gameplay", "controls", "online", "resume", "quit-game", "graphics"):
                key(win, "Down")
                time.sleep(0.5)
                screenshot(win, "f1-" + page)
            key(win, "Right")  # Graphics: enter Display mode
            key(win, "Return")
            assert "fullscreen 1\n" in config.read_text(), "F1 did not save the requested fullscreen mode"
            key(win, "Return")
            assert "fullscreen 0\n" in config.read_text(), "F1 did not save the requested windowed mode"
            key(win, "Down", 2)  # sync -> resolution
            key(win, "Return")
            assert "render_scale 1\n" in config.read_text(), "F1 did not wrap its five pages to Graphics"
            key(win, "F1")
            key(win, "F1")
            resize(win, 720, 760)
            screenshot(win, "f1-small")
            key(win, "Up", 2)  # wrap through Quit game to Resume
            screenshot(win, "f1-resume")
            key(win, "Return")
            assert proc.poll() is None, "game failed after resuming from F1"
            close(win, proc)
        run_case("f1-tab-wrap", [str(args.disc.resolve())], interact=tab_wrap)

        def port_menu(win, proc, config):
            time.sleep(12)
            key(win, "F1")
            time.sleep(1)
            key(win, "Right")  # enter Graphics: Display mode
            key(win, "Down", 2)  # sync -> resolution
            key(win, "Return")
            assert "render_scale 1\n" in config.read_text(), "F1 resolution change was not saved"
            key(win, "Down")  # Aspect ratio
            key(win, "Return")
            assert "widescreen 1\n" in config.read_text(), "widescreen choice was not saved"
            key(win, "Down", 2)  # HUD layout -> Anti-aliasing
            key(win, "Return")
            assert "msaa 4\n" in config.read_text(), "MSAA preference was not saved"
            key(win, "Escape")  # Graphics category
            key(win, "Down")    # Audio
            key(win, "Right")   # Master volume
            key(win, "Return")
            assert "volume 0\n" in config.read_text(), "master volume was not saved"
            key(win, "Down", 5)  # music, SFX, mute, reverb -> FPS counter
            key(win, "Return")
            assert "fps 1\n" in config.read_text(), "FPS preference was not saved"
            key(win, "Escape")
            key(win, "Down", 2)  # Gameplay -> Controls
            time.sleep(0.5)
            screenshot(win, "f1-controls")
            key(win, "Up")
            screenshot(win, "f1-enhancements")
            key(win, "F1")
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
            key(win, "Right")
            key(win, "Down", 2)  # display -> sync -> resolution
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
