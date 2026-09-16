package dev.melee;

public final class TouchControls {
    // GameCube PAD button flags matching dolphin/pad.h
    public static final int PAD_BUTTON_LEFT  = 1 << 0;  // 0x0001
    public static final int PAD_BUTTON_RIGHT = 1 << 1;  // 0x0002
    public static final int PAD_BUTTON_DOWN  = 1 << 2;  // 0x0004
    public static final int PAD_BUTTON_UP    = 1 << 3;  // 0x0008
    public static final int PAD_TRIGGER_Z    = 1 << 4;  // 0x0010
    public static final int PAD_TRIGGER_R    = 1 << 5;  // 0x0020
    public static final int PAD_TRIGGER_L    = 1 << 6;  // 0x0040
    public static final int PAD_BUTTON_A     = 1 << 8;  // 0x0100
    public static final int PAD_BUTTON_B     = 1 << 9;  // 0x0200
    public static final int PAD_BUTTON_X     = 1 << 10; // 0x0400
    public static final int PAD_BUTTON_Y     = 1 << 11; // 0x0800
    public static final int PAD_BUTTON_START = 1 << 12; // 0x1000

    public static native void nativeSetTouchPad(
        int buttons,
        int stickX, int stickY,
        int cstickX, int cstickY,
        int triggerL, int triggerR
    );

    public static native void nativeSetTouchActive(boolean active);

    private TouchControls() {}
}
