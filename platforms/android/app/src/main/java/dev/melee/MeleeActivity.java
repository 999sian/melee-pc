package dev.melee;

import org.libsdl.app.SDLActivity;

public class MeleeActivity extends SDLActivity {

    @Override
    protected String[] getLibraries() {
        /* libpng is linked statically since the aurora sync to 77326d45
         * (extern/aurora/extern/CMakeLists.txt, _USE_SHARED is off on
         * Android), so a clean build ships no libpng16.so and loading it
         * failed before the activity had a window. */
        return new String[] {
            "melee"
        };
    }

    @Override
    protected String getMainFunction() {
        return "SDL_main";
    }

    @Override
    public org.libsdl.app.SDLSurface createSDLSurface(android.content.Context context) {
        return new dev.encounter.aurora.AuroraSurface(context);
    }

    private TouchOverlayView mTouchOverlay;

    @Override
    protected void onCreate(android.os.Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (mBrokenLibraries) {
            return;
        }
        getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        applyImmersiveMode();

        mTouchOverlay = new TouchOverlayView(this);
        if (mLayout != null) {
            mLayout.addView(mTouchOverlay, new android.view.ViewGroup.LayoutParams(
                android.view.ViewGroup.LayoutParams.MATCH_PARENT,
                android.view.ViewGroup.LayoutParams.MATCH_PARENT
            ));
        }

        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.N) {
            android.os.PowerManager pm = (android.os.PowerManager) getSystemService(android.content.Context.POWER_SERVICE);
            if (pm != null && pm.isSustainedPerformanceModeSupported()) {
                getWindow().setSustainedPerformanceMode(true);
            }
        }
        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.M) {
            android.view.WindowManager.LayoutParams lp = getWindow().getAttributes();
            lp.preferredRefreshRate = 60.0f;
            getWindow().setAttributes(lp);
        }
        /* Only pre-Android 13 can be granted READ/WRITE_EXTERNAL_STORAGE at
         * all; from API 33 they are not grantable and the request resolves
         * instantly, but the dialog still pauses and recreates the activity
         * while the graphics device is coming up, which killed the process at
         * startup on a Pixel 8. The disc is read from the app's own external
         * files dir, which needs no permission on any version. */
        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.M
                && android.os.Build.VERSION.SDK_INT < android.os.Build.VERSION_CODES.TIRAMISU) {
            if (checkSelfPermission(android.Manifest.permission.READ_EXTERNAL_STORAGE)
                    != android.content.pm.PackageManager.PERMISSION_GRANTED) {
                requestPermissions(new String[] {
                    android.Manifest.permission.READ_EXTERNAL_STORAGE,
                    android.Manifest.permission.WRITE_EXTERNAL_STORAGE
                }, 100);
            }
        }
    }

    /* Called from native (src/pc/android_compat.cpp) around pc_launcher_run:
     * the launcher needs every touch while it is on screen. */
    public void setLauncherActive(final boolean active) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (mTouchOverlay != null) {
                    mTouchOverlay.setLauncherActive(active);
                }
            }
        });
    }

    public static native void nativeDisconnect();

    @Override
    protected void onStop() {
        if (isFinishing()) {
            try {
                nativeDisconnect();
            } catch (Throwable ignored) {}
        }
        super.onStop();
    }

    @Override
    protected void onDestroy() {
        try {
            nativeDisconnect();
        } catch (Throwable ignored) {}
        super.onDestroy();
    }

    @Override
    protected void onResume() {
        super.onResume();
        applyImmersiveMode();
        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.M) {
            android.view.WindowManager.LayoutParams lp = getWindow().getAttributes();
            lp.preferredRefreshRate = 60.0f;
            getWindow().setAttributes(lp);
        }
        if (mTouchOverlay != null) {
            mTouchOverlay.updateControllerState();
        }
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            applyImmersiveMode();
        }
    }

    private void applyImmersiveMode() {
        if (mBrokenLibraries) {
            return;
        }
        android.view.Window window = getWindow();
        if (window == null) {
            return;
        }
        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.R) {
            window.setDecorFitsSystemWindows(false);
            android.view.WindowInsetsController controller = window.getInsetsController();
            if (controller != null) {
                controller.hide(android.view.WindowInsets.Type.systemBars());
                controller.setSystemBarsBehavior(
                    android.view.WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
            if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.P) {
                android.view.WindowManager.LayoutParams lp = window.getAttributes();
                lp.layoutInDisplayCutoutMode =
                    android.view.WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS;
                window.setAttributes(lp);
            }
        } else {
            int flags = android.view.View.SYSTEM_UI_FLAG_FULLSCREEN
                | android.view.View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | android.view.View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                | android.view.View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                | android.view.View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                | android.view.View.SYSTEM_UI_FLAG_LAYOUT_STABLE;
            window.getDecorView().setSystemUiVisibility(flags);
        }
    }

    @Override
    protected String[] getArguments() {
        android.content.Intent intent = getIntent();
        if (intent != null) {
            String[] args = intent.getStringArrayExtra("args");
            if (args != null && args.length > 0) {
                return args;
            }
            String disc = intent.getStringExtra("disc");
            if (disc != null && !disc.isEmpty()) {
                return new String[] { "--dvd", disc };
            }
            android.net.Uri data = intent.getData();
            if (data != null) {
                try {
                    getContentResolver().takePersistableUriPermission(
                        data, android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION);
                } catch (Exception ignored) {
                }
                return new String[] { "--dvd", data.toString() };
            }
        }
        return new String[0];
    }

    @Override
    public boolean dispatchKeyEvent(android.view.KeyEvent event) {
        if (mSurface != null) {
            handleKeyEvent(mSurface, event.getKeyCode(), event, null);
            return true;
        }
        return super.dispatchKeyEvent(event);
    }
}
