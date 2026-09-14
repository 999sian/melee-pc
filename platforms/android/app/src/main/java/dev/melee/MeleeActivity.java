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
}
