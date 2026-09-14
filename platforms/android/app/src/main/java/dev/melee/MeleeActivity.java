package dev.melee;

import org.libsdl.app.SDLActivity;

public class MeleeActivity extends SDLActivity {

    @Override
    protected String[] getLibraries() {
        return new String[] {
            "png16",
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
