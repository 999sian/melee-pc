package dev.encounter.aurora;

import android.content.Context;
import android.os.Build;
import android.util.Log;
import android.view.Surface;
import android.view.SurfaceHolder;

import org.libsdl.app.SDLSurface;

public class AuroraSurface extends SDLSurface {
    private static native void nativeSetSurfaceReady(boolean ready);

    public AuroraSurface(Context context) {
        super(context);
        getHolder().setKeepScreenOn(true);
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        nativeSetSurfaceReady(false);
        super.surfaceCreated(holder);
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        nativeSetSurfaceReady(false);
        super.surfaceDestroyed(holder);
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        nativeSetSurfaceReady(false);
        super.surfaceChanged(holder, format, width, height);
        nativeSetSurfaceReady(mIsSurfaceReady);
        requestGameFrameRate(holder);
    }

    /* Melee simulates and presents at 60 Hz, and the tablets and phones it
     * runs on default to 90-144 Hz panels. MeleeActivity's window-level
     * preferredRefreshRate is only a hint; on API 30+ SurfaceFlinger picks
     * the refresh rate from per-layer votes, so the game surface votes for
     * 60 itself (Dusklight's BorealisSurface does the same). A recreated
     * surface starts without a vote, so this repeats on every
     * surfaceChanged. */
    private void requestGameFrameRate(SurfaceHolder holder) {
        if (!mIsSurfaceReady || Build.VERSION.SDK_INT < Build.VERSION_CODES.R) {
            return;
        }
        Surface surface = holder.getSurface();
        if (surface == null || !surface.isValid()) {
            return;
        }
        try {
            surface.setFrameRate(60.0f, Surface.FRAME_RATE_COMPATIBILITY_DEFAULT);
        } catch (RuntimeException e) {
            Log.w("AuroraSurface", "setFrameRate(60) failed", e);
        }
    }
}
