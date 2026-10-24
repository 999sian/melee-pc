#!/usr/bin/env python3
"""Exercise the real overlay lifecycle methods with host Java platform doubles."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
JAVA = ROOT / "platforms/android/app/src/main/java/dev/melee"
JDK = Path(os.environ.get("JAVA_HOME", "")) / "bin"
JAVAC = str(JDK / "javac") if (JDK / "javac").is_file() else shutil.which("javac")
JAVA_RUNTIME = str(JDK / "java") if (JDK / "java").is_file() else shutil.which("java")


def method(source, signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class AndroidInputTest(unittest.TestCase):
    def test_surface_disconnects_before_recreation(self):
        source = (ROOT / "extern/aurora/lib/webgpu/gpu.cpp").read_text()
        release = method(source, "static void release_surface_locked()")
        with tempfile.TemporaryDirectory(prefix="melee-surface-") as directory:
            path = Path(directory) / "surface.cpp"
            path.write_text(r"""
#include <cassert>
struct Surface {
    bool live = true, configured = true;
    explicit operator bool() const { return live; }
    void Unconfigure() { assert(live); configured = false; }
    void operator=(int) { assert(!configured); live = false; }
} g_surface;
""" + release.replace("g_surface = {};", "g_surface = 0;") + r"""
int main() {
    release_surface_locked();
    assert(!g_surface.live && !g_surface.configured);
    release_surface_locked(); // Repeated background frames are harmless.
}
""")
            binary = Path(directory) / "surface"
            subprocess.run(["c++", "-std=c++17", str(path), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_activity_uses_sdl_key_dispatch(self):
        # SDL's dispatcher reserves volume keys for Android and dispatches to
        # the focused view; SDLSurface already forwards gamepad keys to SDL.
        self.assertFalse("dispatchKeyEvent(" in (JAVA / "MeleeActivity.java").read_text(),
                         "Melee must preserve SDL's key dispatch")

    @unittest.skipUnless(JAVAC and JAVA_RUNTIME, "Fullscreen runtime check needs a JDK")
    def test_fullscreen_system_bars_and_resume(self):
        source = (JAVA / "MeleeActivity.java").read_text()
        methods = "\n".join(method(source, signature) for signature in (
            "public void setFullscreen(final boolean fullscreen)",
            "private void applyImmersiveMode()",
        ))
        harness = r"""
class android {
    static class os { static class Build {
        static class VERSION { static int SDK_INT = 30; }
        static class VERSION_CODES { static final int R = 30, P = 28; }
    } }
    static class view {
        static class View {
            static final int SYSTEM_UI_FLAG_FULLSCREEN = 1, SYSTEM_UI_FLAG_HIDE_NAVIGATION = 2,
                SYSTEM_UI_FLAG_IMMERSIVE_STICKY = 4, SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN = 8,
                SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION = 16, SYSTEM_UI_FLAG_LAYOUT_STABLE = 32;
            int flags;
            void setSystemUiVisibility(int value) { flags = value; }
        }
        static class WindowInsets { static class Type { static int systemBars() { return 3; } } }
        static class WindowInsetsController {
            static final int BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE = 2, BEHAVIOR_DEFAULT = 1;
            boolean hidden;
            void hide(int bars) { hidden = true; }
            void show(int bars) { hidden = false; }
            void setSystemBarsBehavior(int value) {}
        }
        static class WindowManager { static class LayoutParams {
            static final int FLAG_FULLSCREEN = 1024;
            static final int LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS = 3, LAYOUT_IN_DISPLAY_CUTOUT_MODE_DEFAULT = 0;
            int layoutInDisplayCutoutMode;
        } }
        static class Window {
            void setFlags(int flags, int mask) {}
            View decor = new View();
            WindowInsetsController controller = new WindowInsetsController();
            boolean fits;
            void setDecorFitsSystemWindows(boolean value) { fits = value; }
            WindowInsetsController getInsetsController() { return controller; }
            WindowManager.LayoutParams getAttributes() { return new WindowManager.LayoutParams(); }
            void setAttributes(WindowManager.LayoutParams value) {}
            View getDecorView() { return decor; }
        }
    }
}
public class FullscreenCheck {
    boolean mBrokenLibraries, mFullscreen = true;
    android.view.Window window = new android.view.Window();
    android.view.Window getWindow() { return window; }
    void runOnUiThread(Runnable action) { action.run(); }
    static void check(boolean ok) { if (!ok) throw new AssertionError(); }
    METHODS
    public static void main(String[] args) {
        FullscreenCheck activity = new FullscreenCheck();
        activity.setFullscreen(true);
        check(activity.window.controller.hidden && !activity.window.fits);
        activity.setFullscreen(false);
        check(!activity.window.controller.hidden && activity.window.fits);
        activity.applyImmersiveMode(); // Focus/resume must respect the saved choice.
        check(!activity.window.controller.hidden && activity.window.fits);
        android.os.Build.VERSION.SDK_INT = 26;
        activity.setFullscreen(true);
        check((activity.window.decor.flags & android.view.View.SYSTEM_UI_FLAG_FULLSCREEN) != 0);
        activity.setFullscreen(false);
        check((activity.window.decor.flags & android.view.View.SYSTEM_UI_FLAG_FULLSCREEN) == 0);
    }
}
""".replace("METHODS", methods)
        with tempfile.TemporaryDirectory(prefix="melee-fullscreen-") as directory:
            path = Path(directory) / "FullscreenCheck.java"
            path.write_text(harness)
            subprocess.run([JAVAC, str(path)], check=True)
            subprocess.run([JAVA_RUNTIME, "-cp", directory, "FullscreenCheck"], check=True)

    @unittest.skipUnless(JAVAC and JAVA_RUNTIME, "Overlay runtime check needs a JDK (set JAVA_HOME)")
    def test_overlay_launcher_and_reattach(self):
        source = (JAVA / "TouchOverlayView.java").read_text()
        methods = "\n".join(method(source, signature) for signature in (
            "private void setupControllerListener()",
            "public void setLauncherActive(boolean active)",
            "public void updateControllerState()",
            "protected void onAttachedToWindow()",
            "protected void onDetachedFromWindow()",
        ))
        harness = r"""
import java.util.ArrayDeque;
class View {
    static final int GONE = 8, VISIBLE = 0;
    protected void onAttachedToWindow() {}
    protected void onDetachedFromWindow() {}
    void invalidate() {}
    int visibility;
    void setVisibility(int value) { visibility = value; }
}
class InputManager {
    interface InputDeviceListener {
        void onInputDeviceAdded(int id);
        void onInputDeviceRemoved(int id);
        void onInputDeviceChanged(int id);
    }
    int registered;
    void registerInputDeviceListener(InputDeviceListener listener, Object handler) { registered++; }
    void unregisterInputDeviceListener(InputDeviceListener listener) { registered--; }
}
class TouchControls {
    static boolean active;
    static void nativeSetTouchActive(boolean value) { active = value; }
}
public class OverlayCheck extends View {
    InputManager mInputManager = new InputManager();
    InputManager.InputDeviceListener mInputDeviceListener;
    boolean mLauncherActive, mPhysicalControllerConnected, gamepad;
    boolean mUserEnabled = true;
    ArrayDeque<Runnable> queue = new ArrayDeque<>();
    OverlayCheck() { INIT }
    boolean hasPhysicalGamepad() { return gamepad; }
    void resetInputState() {}
    void post(Runnable action) { queue.add(action); }
    void drain() { while (!queue.isEmpty()) queue.remove().run(); }
    static void check(boolean ok, String message) { if (!ok) throw new AssertionError(message); }
    METHODS
    public static void main(String[] args) {
        OverlayCheck view = new OverlayCheck();
        view.onAttachedToWindow();
        view.drain();
        check(view.mInputManager.registered == 1, "attach must register controller listener");
        check(TouchControls.active, "touch enabled without controller");
        view.updateControllerState(); // Callback was queued before entering launcher.
        view.setLauncherActive(true);
        view.drain();
        check(!TouchControls.active, "queued controller update must not enable launcher touch");
        view.updateControllerState(); // Resume and hotplug also update controller state.
        view.drain();
        check(!TouchControls.active, "launcher must retain input ownership");
        view.setLauncherActive(false);
        view.drain();
        check(TouchControls.active, "leaving launcher restores touch controls");
        view.gamepad = true;
        view.updateControllerState();
        view.drain();
        check(!TouchControls.active, "physical controller disables touch");
        check(view.visibility == View.VISIBLE, "settings must remain accessible with controller");
        view.gamepad = false;
        view.mUserEnabled = false;
        view.updateControllerState();
        view.drain();
        check(!TouchControls.active, "hidden controls stay disabled");
        view.onDetachedFromWindow();
        check(view.mInputManager.registered == 0, "detach must release listener");
        view.onAttachedToWindow();
        check(view.mInputManager.registered == 1, "reattach must restore listener");
    }
}
""".replace("METHODS", methods).replace("INIT", "setupControllerListener();" if
            "setupControllerListener();" in method(source, "private void init(Context context)") else "")
        with tempfile.TemporaryDirectory(prefix="melee-android-input-") as directory:
            path = Path(directory) / "OverlayCheck.java"
            path.write_text(harness)
            subprocess.run([JAVAC, str(path)], check=True)
            subprocess.run([JAVA_RUNTIME, "-cp", directory, "OverlayCheck"], check=True)


if __name__ == "__main__":
    unittest.main()
