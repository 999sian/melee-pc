package dev.melee;

import android.app.AlertDialog;
import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.hardware.input.InputManager;
import android.os.Build;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.util.AttributeSet;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.View;

import java.util.Arrays;

public class TouchOverlayView extends View {

    private static final String PREFS_NAME = "melee_touch_controls";
    private static final String KEY_USER_ENABLED = "user_enabled";
    private static final String KEY_OPACITY = "opacity";
    private static final String KEY_SCALE = "scale";
    private static final String KEY_HAPTICS = "haptics";
    private static final String KEY_FLOATING = "floating_stick";

    private SharedPreferences mPrefs;
    private Vibrator mVibrator;
    private InputManager mInputManager;
    private InputManager.InputDeviceListener mInputDeviceListener;

    private boolean mUserEnabled = true;
    private boolean mPhysicalControllerConnected = false;
    // Set while the RmlUi launcher owns the screen (pushed from
    // src/pc/android_compat.cpp). The overlay must not draw over it or consume
    // any of its touches, or the disc picker is unreachable on a phone.
    private boolean mLauncherActive = false;
    private float mOpacity = 0.55f;
    private float mScale = 1.0f;
    private boolean mHapticsEnabled = true;
    private boolean mFloatingStick = true;

    // Paints
    private final Paint mPaintFill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint mPaintStroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint mPaintText = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint mPaintAccent = new Paint(Paint.ANTI_ALIAS_FLAG);

    // Geometry / layout
    private float mDp = 1.0f;
    private int mViewWidth = 0;
    private int mViewHeight = 0;

    // Main Stick
    private float mStickDefX, mStickDefY;
    private float mStickBaseX, mStickBaseY;
    private float mStickKnobX, mStickKnobY;
    private float mStickBaseRadius, mStickKnobRadius;
    private int mStickPointerId = -1;
    private int mStickValX = 0, mStickValY = 0;

    // C-Stick
    private float mCStickBaseX, mCStickBaseY;
    private float mCStickKnobX, mCStickKnobY;
    private float mCStickBaseRadius, mCStickKnobRadius;
    private int mCStickPointerId = -1;
    private int mCStickValX = 0, mCStickValY = 0;

    // Buttons
    private static class TouchBtn {
        int mask;
        String label;
        float x, y, radius;
        RectF rect; // For rounded rects (L, R, Start)
        int color;
        boolean pressed;
        int pointerId = -1;

        TouchBtn(int mask, String label, int color) {
            this.mask = mask;
            this.label = label;
            this.color = color;
        }
    }

    private TouchBtn mBtnA = new TouchBtn(TouchControls.PAD_BUTTON_A, "A", Color.rgb(46, 204, 113));
    private TouchBtn mBtnB = new TouchBtn(TouchControls.PAD_BUTTON_B, "B", Color.rgb(231, 76, 60));
    private TouchBtn mBtnX = new TouchBtn(TouchControls.PAD_BUTTON_X, "X", Color.rgb(189, 195, 199));
    private TouchBtn mBtnY = new TouchBtn(TouchControls.PAD_BUTTON_Y, "Y", Color.rgb(189, 195, 199));
    private TouchBtn mBtnZ = new TouchBtn(TouchControls.PAD_TRIGGER_Z, "Z", Color.rgb(142, 68, 173));
    private TouchBtn mBtnStart = new TouchBtn(TouchControls.PAD_BUTTON_START, "START", Color.rgb(231, 76, 60));
    private TouchBtn mBtnL = new TouchBtn(TouchControls.PAD_TRIGGER_L, "L", Color.rgb(120, 144, 156));
    private TouchBtn mBtnR = new TouchBtn(TouchControls.PAD_TRIGGER_R, "R", Color.rgb(120, 144, 156));

    // D-Pad
    private TouchBtn mBtnDUp = new TouchBtn(TouchControls.PAD_BUTTON_UP, "▲", Color.rgb(90, 110, 125));
    private TouchBtn mBtnDDown = new TouchBtn(TouchControls.PAD_BUTTON_DOWN, "▼", Color.rgb(90, 110, 125));
    private TouchBtn mBtnDLeft = new TouchBtn(TouchControls.PAD_BUTTON_LEFT, "◀", Color.rgb(90, 110, 125));
    private TouchBtn mBtnDRight = new TouchBtn(TouchControls.PAD_BUTTON_RIGHT, "▶", Color.rgb(90, 110, 125));

    private TouchBtn[] mAllButtons;

    // Quick Settings Button (top-center)
    private final RectF mSettingsRect = new RectF();

    private int mCurrentMask = 0;

    public TouchOverlayView(Context context) {
        super(context);
        init(context);
    }

    public TouchOverlayView(Context context, AttributeSet attrs) {
        super(context, attrs);
        init(context);
    }

    private void init(Context context) {
        mPrefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE);
        mUserEnabled = mPrefs.getBoolean(KEY_USER_ENABLED, true);
        mOpacity = mPrefs.getFloat(KEY_OPACITY, 0.55f);
        mScale = mPrefs.getFloat(KEY_SCALE, 1.0f);
        mHapticsEnabled = mPrefs.getBoolean(KEY_HAPTICS, true);
        mFloatingStick = mPrefs.getBoolean(KEY_FLOATING, true);

        mAllButtons = new TouchBtn[] {
            mBtnA, mBtnB, mBtnX, mBtnY, mBtnZ, mBtnStart, mBtnL, mBtnR,
            mBtnDUp, mBtnDDown, mBtnDLeft, mBtnDRight
        };

        mVibrator = (Vibrator) context.getSystemService(Context.VIBRATOR_SERVICE);
        mInputManager = (InputManager) context.getSystemService(Context.INPUT_SERVICE);

        mPaintFill.setStyle(Paint.Style.FILL);
        mPaintStroke.setStyle(Paint.Style.STROKE);
        mPaintText.setTextAlign(Paint.Align.CENTER);
        mPaintAccent.setStyle(Paint.Style.STROKE);

        setupControllerListener();
        updateControllerState();
    }

    private void setupControllerListener() {
        if (mInputManager != null) {
            mInputDeviceListener = new InputManager.InputDeviceListener() {
                @Override public void onInputDeviceAdded(int deviceId) { updateControllerState(); }
                @Override public void onInputDeviceRemoved(int deviceId) { updateControllerState(); }
                @Override public void onInputDeviceChanged(int deviceId) { updateControllerState(); }
            };
            mInputManager.registerInputDeviceListener(mInputDeviceListener, null);
        }
    }

    public static boolean hasPhysicalGamepad() {
        int[] ids = InputDevice.getDeviceIds();
        for (int id : ids) {
            InputDevice dev = InputDevice.getDevice(id);
            if (dev == null || dev.isVirtual()) continue;
            int sources = dev.getSources();
            if (((sources & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD) ||
                ((sources & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK)) {
                return true;
            }
        }
        return false;
    }

    /* Called from native while the launcher is up and again once the game
     * starts. While it is up the overlay is fully inert: the launcher draws
     * its own UI on the same surface and needs every tap. */
    public void setLauncherActive(boolean active) {
        if (mLauncherActive == active) {
            return;
        }
        mLauncherActive = active;
        if (active) {
            resetInputState();
            TouchControls.nativeSetTouchActive(false);
        } else {
            updateControllerState();
        }
        invalidate();
    }

    public void updateControllerState() {
        final boolean hasGamepad = hasPhysicalGamepad();
        mPhysicalControllerConnected = hasGamepad;
        post(new Runnable() {
            @Override
            public void run() {
                if (mPhysicalControllerConnected) {
                    // Physical gamepad detected: disable and hide touch controls
                    setVisibility(View.GONE);
                    TouchControls.nativeSetTouchActive(false);
                } else {
                    // No physical gamepad: stay on screen even when the
                    // controls are hidden, so the settings pill that un-hides
                    // them is still reachable.
                    setVisibility(View.VISIBLE);
                    TouchControls.nativeSetTouchActive(mUserEnabled);
                }
            }
        });
    }

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        updateControllerState();
    }

    @Override
    protected void onDetachedFromWindow() {
        super.onDetachedFromWindow();
        if (mInputManager != null && mInputDeviceListener != null) {
            mInputManager.unregisterInputDeviceListener(mInputDeviceListener);
        }
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        mViewWidth = w;
        mViewHeight = h;
        mDp = getResources().getDisplayMetrics().density;
        layoutControls();
    }

    private void layoutControls() {
        if (mViewWidth <= 0 || mViewHeight <= 0) return;

        final float dp = mDp;
        final float s = mScale;

        // Main Stick (bottom-left)
        mStickBaseRadius = 68.0f * dp * s;
        mStickKnobRadius = 32.0f * dp * s;
        mStickDefX = 115.0f * dp * s;
        mStickDefY = mViewHeight - 120.0f * dp * s;
        mStickBaseX = mStickDefX;
        mStickBaseY = mStickDefY;
        mStickKnobX = mStickDefX;
        mStickKnobY = mStickDefY;

        // D-Pad (cross above the main stick)
        float dpadCenterX = 90.0f * dp * s;
        float dpadCenterY = mViewHeight - 275.0f * dp * s;
        float dpadArm = 28.0f * dp * s;
        float dpadR = 18.0f * dp * s;
        mBtnDUp.x = dpadCenterX; mBtnDUp.y = dpadCenterY - dpadArm; mBtnDUp.radius = dpadR;
        mBtnDDown.x = dpadCenterX; mBtnDDown.y = dpadCenterY + dpadArm; mBtnDDown.radius = dpadR;
        mBtnDLeft.x = dpadCenterX - dpadArm; mBtnDLeft.y = dpadCenterY; mBtnDLeft.radius = dpadR;
        mBtnDRight.x = dpadCenterX + dpadArm; mBtnDRight.y = dpadCenterY; mBtnDRight.radius = dpadR;

        // C-Stick (yellow mini-stick placed left of the ABXY cluster)
        mCStickBaseRadius = 46.0f * dp * s;
        mCStickKnobRadius = 22.0f * dp * s;
        mCStickBaseX = mViewWidth - 235.0f * dp * s;
        mCStickBaseY = mViewHeight - 95.0f * dp * s;
        mCStickKnobX = mCStickBaseX;
        mCStickKnobY = mCStickBaseY;

        // GameCube ABXY Cluster (bottom-right)
        float aCenterX = mViewWidth - 105.0f * dp * s;
        float aCenterY = mViewHeight - 120.0f * dp * s;

        // A (center green, large)
        mBtnA.x = aCenterX;
        mBtnA.y = aCenterY;
        mBtnA.radius = 36.0f * dp * s;

        // B (left-bottom red circle)
        mBtnB.x = aCenterX - 52.0f * dp * s;
        mBtnB.y = aCenterY + 16.0f * dp * s;
        mBtnB.radius = 24.0f * dp * s;

        // X (right silver/grey)
        mBtnX.x = aCenterX + 52.0f * dp * s;
        mBtnX.y = aCenterY - 12.0f * dp * s;
        mBtnX.radius = 23.0f * dp * s;

        // Y (top silver/grey)
        mBtnY.x = aCenterX - 8.0f * dp * s;
        mBtnY.y = aCenterY - 54.0f * dp * s;
        mBtnY.radius = 23.0f * dp * s;

        // Z (purple shoulder/action button above cluster)
        mBtnZ.x = aCenterX + 40.0f * dp * s;
        mBtnZ.y = aCenterY - 68.0f * dp * s;
        mBtnZ.radius = 22.0f * dp * s;

        // Triggers: L (top-left) & R (top-right)
        float triggerW = 88.0f * dp * s;
        float triggerH = 44.0f * dp * s;
        mBtnL.rect = new RectF(16.0f * dp, 16.0f * dp, 16.0f * dp + triggerW, 16.0f * dp + triggerH);
        mBtnL.x = mBtnL.rect.centerX(); mBtnL.y = mBtnL.rect.centerY();

        mBtnR.rect = new RectF(mViewWidth - 16.0f * dp - triggerW, 16.0f * dp, mViewWidth - 16.0f * dp, 16.0f * dp + triggerH);
        mBtnR.x = mBtnR.rect.centerX(); mBtnR.y = mBtnR.rect.centerY();

        // Start / Pause (top-center right)
        float startW = 60.0f * dp * s;
        float startH = 28.0f * dp * s;
        float startX = (mViewWidth / 2.0f) + 65.0f * dp * s;
        float startY = 18.0f * dp;
        mBtnStart.rect = new RectF(startX, startY, startX + startW, startY + startH);
        mBtnStart.x = mBtnStart.rect.centerX(); mBtnStart.y = mBtnStart.rect.centerY();

        // Quick Settings Button (top-center)
        float setW = 44.0f * dp * s;
        float setH = 28.0f * dp * s;
        mSettingsRect.set((mViewWidth - setW) / 2.0f, 18.0f * dp, (mViewWidth + setW) / 2.0f, 18.0f * dp + setH);
    }

    private void vibrateTick() {
        if (!mHapticsEnabled || mVibrator == null) return;
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                mVibrator.vibrate(VibrationEffect.createPredefined(VibrationEffect.EFFECT_CLICK));
            } else if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                mVibrator.vibrate(VibrationEffect.createOneShot(12, VibrationEffect.DEFAULT_AMPLITUDE));
            } else {
                mVibrator.vibrate(12);
            }
        } catch (Exception ignored) {}
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        // The launcher owns every touch while it is up.
        if (mLauncherActive || mPhysicalControllerConnected) {
            return false;
        }
        // Hidden controls still answer on the settings pill: it is the only
        // way back, since the hide toggle lives in its dialog.
        if (!mUserEnabled) {
            if (event.getActionMasked() == MotionEvent.ACTION_DOWN
                    && mSettingsRect.contains(event.getX(), event.getY())) {
                vibrateTick();
                showSettingsDialog();
                return true;
            }
            return false;
        }

        final int action = event.getActionMasked();
        final int actionIndex = event.getActionIndex();
        final int pointerCount = event.getPointerCount();

        // Pass through touches outside controls (e.g. clicking launcher menu items)
        if (action == MotionEvent.ACTION_DOWN) {
            float downX = event.getX();
            float downY = event.getY();
            if (!isTouchOnAnyControl(downX, downY)) {
                return false;
            }
        }

        // Check settings click on pointer down
        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
            float downX = event.getX(actionIndex);
            float downY = event.getY(actionIndex);
            if (mSettingsRect.contains(downX, downY)) {
                vibrateTick();
                showSettingsDialog();
                return true;
            }
        }

        // Reset pointer associations on release
        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) {
            resetInputState();
            sendPadState();
            invalidate();
            return true;
        }

        if (action == MotionEvent.ACTION_POINTER_UP) {
            int liftedId = event.getPointerId(actionIndex);
            if (mStickPointerId == liftedId) {
                mStickPointerId = -1;
                mStickKnobX = mStickDefX;
                mStickKnobY = mStickDefY;
                mStickBaseX = mStickDefX;
                mStickBaseY = mStickDefY;
                mStickValX = 0;
                mStickValY = 0;
            }
            if (mCStickPointerId == liftedId) {
                mCStickPointerId = -1;
                mCStickKnobX = mCStickBaseX;
                mCStickKnobY = mCStickBaseY;
                mCStickValX = 0;
                mCStickValY = 0;
            }
            for (TouchBtn btn : mAllButtons) {
                if (btn.pointerId == liftedId) {
                    btn.pressed = false;
                    btn.pointerId = -1;
                }
            }
        }

        // Process active pointers
        boolean hadNewPress = false;

        // Clear button pressed state before re-evaluating pointers
        for (TouchBtn btn : mAllButtons) {
            btn.pressed = false;
        }

        for (int i = 0; i < pointerCount; i++) {
            if (action == MotionEvent.ACTION_POINTER_UP && i == actionIndex) {
                continue;
            }
            int pId = event.getPointerId(i);
            float px = event.getX(i);
            float py = event.getY(i);

            // Handle main stick
            if (mStickPointerId == pId) {
                updateStick(px, py);
                continue;
            }

            // Handle c-stick
            if (mCStickPointerId == pId) {
                updateCStick(px, py);
                continue;
            }

            // If pointer is unassigned, check if it initiates Main Stick or C-Stick
            if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
                if (i == actionIndex) {
                    // Check C-Stick zone
                    float distC = (float) Math.hypot(px - mCStickBaseX, py - mCStickBaseY);
                    if (distC <= mCStickBaseRadius * 1.5f) {
                        mCStickPointerId = pId;
                        updateCStick(px, py);
                        continue;
                    }

                    // Check Main Stick zone (left 45% of screen, lower 75%)
                    if (px < mViewWidth * 0.45f && py > mViewHeight * 0.25f && !isOverDpad(px, py) && !isOverL(px, py)) {
                        mStickPointerId = pId;
                        if (mFloatingStick) {
                            mStickBaseX = px;
                            mStickBaseY = py;
                            mStickKnobX = px;
                            mStickKnobY = py;
                            mStickValX = 0;
                            mStickValY = 0;
                        } else {
                            updateStick(px, py);
                        }
                        continue;
                    }
                }
            }

            // Check buttons: assign pointer to the nearest matching button
            TouchBtn bestBtn = null;
            float bestDist = Float.MAX_VALUE;
            for (TouchBtn btn : mAllButtons) {
                if (btn.rect != null) {
                    if (isPointerOverBtn(btn, px, py)) {
                        bestBtn = btn;
                        break;
                    }
                } else {
                    float dist = (float) Math.hypot(px - btn.x, py - btn.y);
                    if (dist <= btn.radius * 1.25f && dist < bestDist) {
                        bestDist = dist;
                        bestBtn = btn;
                    }
                }
            }
            if (bestBtn != null) {
                if (!bestBtn.pressed && bestBtn.pointerId != pId) {
                    hadNewPress = true;
                }
                bestBtn.pressed = true;
                bestBtn.pointerId = pId;
            }
        }

        if (hadNewPress) {
            vibrateTick();
        }

        sendPadState();
        invalidate();
        return true;
    }

    private boolean isOverDpad(float px, float py) {
        return isPointerOverBtn(mBtnDUp, px, py) || isPointerOverBtn(mBtnDDown, px, py)
            || isPointerOverBtn(mBtnDLeft, px, py) || isPointerOverBtn(mBtnDRight, px, py);
    }

    private boolean isOverL(float px, float py) {
        return mBtnL.rect != null && mBtnL.rect.contains(px, py);
    }

    private boolean isTouchOnAnyControl(float px, float py) {
        if (mSettingsRect.contains(px, py)) return true;
        for (TouchBtn btn : mAllButtons) {
            if (isPointerOverBtn(btn, px, py)) return true;
        }
        float distC = (float) Math.hypot(px - mCStickBaseX, py - mCStickBaseY);
        if (distC <= mCStickBaseRadius * 1.5f) return true;
        if (px < mViewWidth * 0.40f && py > mViewHeight * 0.35f) return true;
        return false;
    }

    private boolean isPointerOverBtn(TouchBtn btn, float px, float py) {
        if (btn.rect != null) {
            // Expand touch target for rectangular triggers / buttons
            RectF hit = new RectF(btn.rect);
            hit.inset(-8.0f * mDp, -8.0f * mDp);
            return hit.contains(px, py);
        } else {
            float dist = (float) Math.hypot(px - btn.x, py - btn.y);
            return dist <= btn.radius * 1.25f;
        }
    }

    private void updateStick(float px, float py) {
        float dx = px - mStickBaseX;
        float dy = py - mStickBaseY;
        float dist = (float) Math.hypot(dx, dy);
        float maxR = mStickBaseRadius;

        if (dist > maxR && dist > 0.001f) {
            mStickKnobX = mStickBaseX + dx * (maxR / dist);
            mStickKnobY = mStickBaseY + dy * (maxR / dist);
        } else {
            mStickKnobX = px;
            mStickKnobY = py;
        }

        // Deadzone & deflection scaling
        float norm = Math.min(1.0f, dist / maxR);
        float deadzone = 0.12f;
        if (norm < deadzone) {
            mStickValX = 0;
            mStickValY = 0;
        } else {
            float scaled = (norm - deadzone) / (1.0f - deadzone);
            // Melee triggers dash/smash at +-80 deflection; max out at +-85
            float angleX = (dist > 0.001f) ? (dx / dist) : 0;
            float angleY = (dist > 0.001f) ? (dy / dist) : 0;
            mStickValX = Math.round(angleX * scaled * 85.0f);
            // Invert Y: screen Y goes down, GameCube Y goes up
            mStickValY = Math.round(-angleY * scaled * 85.0f);
            mStickValX = Math.max(-127, Math.min(127, mStickValX));
            mStickValY = Math.max(-127, Math.min(127, mStickValY));
        }
    }

    private void updateCStick(float px, float py) {
        float dx = px - mCStickBaseX;
        float dy = py - mCStickBaseY;
        float dist = (float) Math.hypot(dx, dy);
        float maxR = mCStickBaseRadius;

        if (dist > maxR && dist > 0.001f) {
            mCStickKnobX = mCStickBaseX + dx * (maxR / dist);
            mCStickKnobY = mCStickBaseY + dy * (maxR / dist);
        } else {
            mCStickKnobX = px;
            mCStickKnobY = py;
        }

        float norm = Math.min(1.0f, dist / maxR);
        float deadzone = 0.15f;
        if (norm < deadzone) {
            mCStickValX = 0;
            mCStickValY = 0;
        } else {
            float scaled = (norm - deadzone) / (1.0f - deadzone);
            float angleX = (dist > 0.001f) ? (dx / dist) : 0;
            float angleY = (dist > 0.001f) ? (dy / dist) : 0;
            mCStickValX = Math.round(angleX * scaled * 85.0f);
            mCStickValY = Math.round(-angleY * scaled * 85.0f);
            mCStickValX = Math.max(-127, Math.min(127, mCStickValX));
            mCStickValY = Math.max(-127, Math.min(127, mCStickValY));
        }
    }

    private void resetInputState() {
        mStickPointerId = -1;
        mCStickPointerId = -1;
        mStickKnobX = mStickDefX;
        mStickKnobY = mStickDefY;
        mStickBaseX = mStickDefX;
        mStickBaseY = mStickDefY;
        mStickValX = 0;
        mStickValY = 0;
        mCStickKnobX = mCStickBaseX;
        mCStickKnobY = mCStickBaseY;
        mCStickValX = 0;
        mCStickValY = 0;
        for (TouchBtn btn : mAllButtons) {
            btn.pressed = false;
            btn.pointerId = -1;
        }
    }

    private void sendPadState() {
        int mask = 0;
        int triggerL = 0;
        int triggerR = 0;

        for (TouchBtn btn : mAllButtons) {
            if (btn.pressed) {
                mask |= btn.mask;
                if (btn.mask == TouchControls.PAD_TRIGGER_L) triggerL = 255;
                if (btn.mask == TouchControls.PAD_TRIGGER_R) triggerR = 255;
            }
        }

        mCurrentMask = mask;
        TouchControls.nativeSetTouchPad(
            mask,
            mStickValX, mStickValY,
            mCStickValX, mCStickValY,
            triggerL, triggerR
        );
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        if (mLauncherActive || mPhysicalControllerConnected) return;

        final int baseAlpha = Math.round(mOpacity * 255.0f);
        if (!mUserEnabled) {
            // Controls hidden, but the settings pill stays so the hide can be
            // undone.
            drawSettingsPill(canvas, baseAlpha);
            return;
        }

        final int pressedAlpha = Math.min(255, Math.round((mOpacity + 0.35f) * 255.0f));

        // 1. Draw Main Stick
        drawControlStick(canvas, baseAlpha, pressedAlpha);

        // 2. Draw D-Pad
        drawDpad(canvas, baseAlpha, pressedAlpha);

        // 3. Draw C-Stick
        drawCStick(canvas, baseAlpha, pressedAlpha);

        // 4. Draw Action Buttons (A, B, X, Y, Z, Start, L, R)
        for (TouchBtn btn : mAllButtons) {
            if (btn == mBtnDUp || btn == mBtnDDown || btn == mBtnDLeft || btn == mBtnDRight) {
                continue; // Dpad drawn separately
            }
            drawButton(canvas, btn, baseAlpha, pressedAlpha);
        }

        // 5. Draw Settings Button
        drawSettingsPill(canvas, baseAlpha);
    }

    private void drawControlStick(Canvas canvas, int baseAlpha, int pressedAlpha) {
        // Outer GameCube gate with 8 notch indicators
        mPaintStroke.setColor(Color.WHITE);
        mPaintStroke.setAlpha(baseAlpha / 2);
        mPaintStroke.setStrokeWidth(2.5f * mDp);
        canvas.drawCircle(mStickBaseX, mStickBaseY, mStickBaseRadius, mPaintStroke);

        mPaintFill.setColor(Color.BLACK);
        mPaintFill.setAlpha(baseAlpha / 4);
        canvas.drawCircle(mStickBaseX, mStickBaseY, mStickBaseRadius, mPaintFill);

        // 8 gate notches
        mPaintStroke.setAlpha(baseAlpha / 3);
        mPaintStroke.setStrokeWidth(2.0f * mDp);
        for (int i = 0; i < 8; i++) {
            double angle = i * (Math.PI / 4.0);
            float x1 = mStickBaseX + (float) Math.cos(angle) * (mStickBaseRadius - 6.0f * mDp);
            float y1 = mStickBaseY + (float) Math.sin(angle) * (mStickBaseRadius - 6.0f * mDp);
            float x2 = mStickBaseX + (float) Math.cos(angle) * (mStickBaseRadius + 2.0f * mDp);
            float y2 = mStickBaseY + (float) Math.sin(angle) * (mStickBaseRadius + 2.0f * mDp);
            canvas.drawLine(x1, y1, x2, y2, mPaintStroke);
        }

        // Inner Stick Knob
        int knobAlpha = (mStickPointerId != -1) ? pressedAlpha : baseAlpha;
        mPaintFill.setColor(Color.rgb(176, 190, 197));
        mPaintFill.setAlpha(knobAlpha);
        canvas.drawCircle(mStickKnobX, mStickKnobY, mStickKnobRadius, mPaintFill);

        // Concentric rubber rings on knob
        mPaintStroke.setColor(Color.rgb(55, 71, 79));
        mPaintStroke.setAlpha(knobAlpha);
        mPaintStroke.setStrokeWidth(1.5f * mDp);
        canvas.drawCircle(mStickKnobX, mStickKnobY, mStickKnobRadius * 0.70f, mPaintStroke);
        canvas.drawCircle(mStickKnobX, mStickKnobY, mStickKnobRadius * 0.40f, mPaintStroke);
    }

    private void drawCStick(Canvas canvas, int baseAlpha, int pressedAlpha) {
        // Outer gate
        mPaintStroke.setColor(Color.WHITE);
        mPaintStroke.setAlpha(baseAlpha / 2);
        mPaintStroke.setStrokeWidth(2.5f * mDp);
        canvas.drawCircle(mCStickBaseX, mCStickBaseY, mCStickBaseRadius, mPaintStroke);

        mPaintFill.setColor(Color.BLACK);
        mPaintFill.setAlpha(baseAlpha / 4);
        canvas.drawCircle(mCStickBaseX, mCStickBaseY, mCStickBaseRadius, mPaintFill);

        // Yellow Knob
        int knobAlpha = (mCStickPointerId != -1) ? pressedAlpha : baseAlpha;
        mPaintFill.setColor(Color.rgb(241, 196, 15)); // GameCube Yellow
        mPaintFill.setAlpha(knobAlpha);
        canvas.drawCircle(mCStickKnobX, mCStickKnobY, mCStickKnobRadius, mPaintFill);

        // Directional cross markings on C-stick
        mPaintStroke.setColor(Color.rgb(183, 149, 11));
        mPaintStroke.setAlpha(knobAlpha);
        mPaintStroke.setStrokeWidth(2.5f * mDp);
        float arm = mCStickKnobRadius * 0.55f;
        canvas.drawLine(mCStickKnobX - arm, mCStickKnobY, mCStickKnobX + arm, mCStickKnobY, mPaintStroke);
        canvas.drawLine(mCStickKnobX, mCStickKnobY - arm, mCStickKnobX, mCStickKnobY + arm, mPaintStroke);
    }

    private void drawDpad(Canvas canvas, int baseAlpha, int pressedAlpha) {
        for (TouchBtn btn : new TouchBtn[] { mBtnDUp, mBtnDDown, mBtnDLeft, mBtnDRight }) {
            int alpha = btn.pressed ? pressedAlpha : baseAlpha;
            mPaintFill.setColor(btn.color);
            mPaintFill.setAlpha(alpha);
            canvas.drawCircle(btn.x, btn.y, btn.radius, mPaintFill);

            mPaintStroke.setColor(Color.WHITE);
            mPaintStroke.setAlpha(alpha);
            mPaintStroke.setStrokeWidth(1.5f * mDp);
            canvas.drawCircle(btn.x, btn.y, btn.radius, mPaintStroke);

            mPaintText.setColor(Color.WHITE);
            mPaintText.setAlpha(alpha);
            mPaintText.setTextSize(btn.radius * 0.9f);
            mPaintText.setFakeBoldText(true);
            float textY = btn.y - ((mPaintText.descent() + mPaintText.ascent()) / 2.0f);
            canvas.drawText(btn.label, btn.x, textY, mPaintText);
        }
    }

    private void drawButton(Canvas canvas, TouchBtn btn, int baseAlpha, int pressedAlpha) {
        int alpha = btn.pressed ? pressedAlpha : baseAlpha;
        float rCorner = 8.0f * mDp;

        if (btn.rect != null) {
            // Rounded rectangle button (L, R, Start)
            mPaintFill.setColor(btn.color);
            mPaintFill.setAlpha(alpha);
            canvas.drawRoundRect(btn.rect, rCorner, rCorner, mPaintFill);

            mPaintStroke.setColor(Color.WHITE);
            mPaintStroke.setAlpha(alpha);
            mPaintStroke.setStrokeWidth(2.0f * mDp);
            canvas.drawRoundRect(btn.rect, rCorner, rCorner, mPaintStroke);

            mPaintText.setColor(Color.WHITE);
            mPaintText.setAlpha(alpha);
            mPaintText.setTextSize(btn.rect.height() * 0.45f);
            mPaintText.setFakeBoldText(true);
            float textY = btn.y - ((mPaintText.descent() + mPaintText.ascent()) / 2.0f);
            canvas.drawText(btn.label, btn.x, textY, mPaintText);
        } else {
            // Circular button (A, B, X, Y, Z)
            float drawRadius = btn.pressed ? (btn.radius * 1.08f) : btn.radius;

            mPaintFill.setColor(btn.color);
            mPaintFill.setAlpha(alpha);
            canvas.drawCircle(btn.x, btn.y, drawRadius, mPaintFill);

            mPaintStroke.setColor(Color.WHITE);
            mPaintStroke.setAlpha(alpha);
            mPaintStroke.setStrokeWidth(2.0f * mDp);
            canvas.drawCircle(btn.x, btn.y, drawRadius, mPaintStroke);

            mPaintText.setColor(Color.WHITE);
            mPaintText.setAlpha(alpha);
            mPaintText.setTextSize(btn.radius * 0.85f);
            mPaintText.setFakeBoldText(true);
            float textY = btn.y - ((mPaintText.descent() + mPaintText.ascent()) / 2.0f);
            canvas.drawText(btn.label, btn.x, textY, mPaintText);
        }
    }

    private void drawSettingsPill(Canvas canvas, int baseAlpha) {
        mPaintFill.setColor(Color.rgb(55, 71, 79));
        mPaintFill.setAlpha(baseAlpha);
        float rCorner = 6.0f * mDp;
        canvas.drawRoundRect(mSettingsRect, rCorner, rCorner, mPaintFill);

        mPaintStroke.setColor(Color.WHITE);
        mPaintStroke.setAlpha(baseAlpha);
        mPaintStroke.setStrokeWidth(1.5f * mDp);
        canvas.drawRoundRect(mSettingsRect, rCorner, rCorner, mPaintStroke);

        mPaintText.setColor(Color.WHITE);
        mPaintText.setAlpha(baseAlpha);
        mPaintText.setTextSize(mSettingsRect.height() * 0.45f);
        mPaintText.setFakeBoldText(true);
        float textY = mSettingsRect.centerY() - ((mPaintText.descent() + mPaintText.ascent()) / 2.0f);
        canvas.drawText("⚙", mSettingsRect.centerX(), textY, mPaintText);
    }

    private void showSettingsDialog() {
        Context ctx = getContext();
        AlertDialog.Builder builder = new AlertDialog.Builder(ctx);
        builder.setTitle("Touch Controls Settings");

        String[] options = new String[] {
            "Opacity: " + Math.round(mOpacity * 100) + "%",
            "Button Size: " + (mScale <= 0.85f ? "Small (80%)" : (mScale >= 1.15f ? "Large (120%)" : "Normal (100%)")),
            "Haptic Feedback: " + (mHapticsEnabled ? "ON" : "OFF"),
            "Floating Stick: " + (mFloatingStick ? "ON" : "OFF"),
            mUserEnabled ? "Hide Touch Controls" : "Show Touch Controls"
        };

        builder.setItems(options, (dialog, which) -> {
            switch (which) {
                case 0: // Cycle Opacity (25% -> 40% -> 60% -> 80% -> 100%)
                    if (mOpacity < 0.35f) mOpacity = 0.45f;
                    else if (mOpacity < 0.55f) mOpacity = 0.70f;
                    else if (mOpacity < 0.85f) mOpacity = 1.0f;
                    else mOpacity = 0.25f;
                    mPrefs.edit().putFloat(KEY_OPACITY, mOpacity).apply();
                    break;
                case 1: // Cycle Scale (0.8 -> 1.0 -> 1.2)
                    if (mScale < 0.9f) mScale = 1.0f;
                    else if (mScale < 1.1f) mScale = 1.2f;
                    else mScale = 0.8f;
                    mPrefs.edit().putFloat(KEY_SCALE, mScale).apply();
                    layoutControls();
                    break;
                case 2: // Toggle Haptics
                    mHapticsEnabled = !mHapticsEnabled;
                    mPrefs.edit().putBoolean(KEY_HAPTICS, mHapticsEnabled).apply();
                    break;
                case 3: // Toggle Floating Stick
                    mFloatingStick = !mFloatingStick;
                    mPrefs.edit().putBoolean(KEY_FLOATING, mFloatingStick).apply();
                    break;
                case 4: // Hide / show the on-screen controls
                    mUserEnabled = !mUserEnabled;
                    mPrefs.edit().putBoolean(KEY_USER_ENABLED, mUserEnabled).apply();
                    if (!mUserEnabled) {
                        resetInputState();
                    }
                    TouchControls.nativeSetTouchActive(mUserEnabled);
                    break;
            }
            invalidate();
        });

        builder.setPositiveButton("Close", null);
        builder.show();
    }
}
