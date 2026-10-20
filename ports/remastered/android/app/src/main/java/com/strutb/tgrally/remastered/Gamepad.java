package com.strutb.tgrally.remastered;

// Gamepad.java: a game controller as the game's second pad (host_pad, the
// DirectInput/XInput layout: host.h). The activity hands it every key and
// motion event a controller sends; the sticks and triggers are read here and
// the buttons held kept as host.h's bits.
final class Gamepad {
    private static final int A = 0, B = 1, X = 2, Y = 3, LB = 4, RB = 5, VIEW = 6, MENU = 7,
            L3 = 8, R3 = 9, LT = 10, RT = 11, UP = 12, RIGHT = 13, DOWN = 14, LEFT = 15;

    private static int held;                 // the button bits held (keys)
    private static float x, y, rx, ry, z;

    static boolean onKey(int keyCode, boolean down) {
        int bit = bitFor(keyCode);
        if (bit < 0)
            return false;
        if (down)
            held |= 1 << bit;
        else
            held &= ~(1 << bit);
        push();
        return true;
    }

    // MOTION, not key: the sticks and the triggers
    static void onMotion(android.view.MotionEvent ev) {
        if ((ev.getSource() & android.view.InputDevice.SOURCE_JOYSTICK) == 0)
            return;
        x = ev.getAxisValue(android.view.MotionEvent.AXIS_X);
        y = ev.getAxisValue(android.view.MotionEvent.AXIS_Y);
        rx = ev.getAxisValue(android.view.MotionEvent.AXIS_Z);
        ry = ev.getAxisValue(android.view.MotionEvent.AXIS_RZ);
        // a pad may report its triggers as LTRIGGER/RTRIGGER or BRAKE/GAS
        float lt = Math.max(ev.getAxisValue(android.view.MotionEvent.AXIS_LTRIGGER),
                            ev.getAxisValue(android.view.MotionEvent.AXIS_BRAKE));
        float rt = Math.max(ev.getAxisValue(android.view.MotionEvent.AXIS_RTRIGGER),
                            ev.getAxisValue(android.view.MotionEvent.AXIS_GAS));
        z = rt - lt;
        int h = ev.getAxisValue(android.view.MotionEvent.AXIS_HAT_Y) > 0.5f ? 1 << DOWN
                : ev.getAxisValue(android.view.MotionEvent.AXIS_HAT_Y) < -0.5f ? 1 << UP : 0;
        h |= ev.getAxisValue(android.view.MotionEvent.AXIS_HAT_X) > 0.5f ? 1 << RIGHT
                : ev.getAxisValue(android.view.MotionEvent.AXIS_HAT_X) < -0.5f ? 1 << LEFT : 0;
        int b = held & ~((1 << UP) | (1 << DOWN) | (1 << LEFT) | (1 << RIGHT));
        b |= h;
        if (lt > 0.5f) b |= 1 << LT;
        if (rt > 0.5f) b |= 1 << RT;
        push(x, y, rx, ry, z, b);
    }

    static void reset() {
        x = y = rx = ry = z = 0;
        held = 0;
        Native.gamepad(0, 0, 0, 0, 0, 0, 0);      // none connected, the state centred
    }

    private static void push() { push(x, y, rx, ry, z, held); }

    private static void push(float lx, float ly, float lrx, float lry, float lz, int buttons) {
        Native.gamepad(lx, ly, lrx, lry, lz, buttons, 1);
    }

    // host.h's bits for the standard Android controller keys
    private static int bitFor(int keyCode) {
        switch (keyCode) {
            case android.view.KeyEvent.KEYCODE_BUTTON_A: return A;
            case android.view.KeyEvent.KEYCODE_BUTTON_B: return B;
            case android.view.KeyEvent.KEYCODE_BUTTON_X: return X;
            case android.view.KeyEvent.KEYCODE_BUTTON_Y: return Y;
            case android.view.KeyEvent.KEYCODE_BUTTON_L1: return LB;
            case android.view.KeyEvent.KEYCODE_BUTTON_R1: return RB;
            case android.view.KeyEvent.KEYCODE_BUTTON_SELECT: return VIEW;
            case android.view.KeyEvent.KEYCODE_BUTTON_START: return MENU;
            case android.view.KeyEvent.KEYCODE_BUTTON_THUMBL: return L3;
            case android.view.KeyEvent.KEYCODE_BUTTON_THUMBR: return R3;
            case android.view.KeyEvent.KEYCODE_DPAD_UP: return UP;
            case android.view.KeyEvent.KEYCODE_DPAD_RIGHT: return RIGHT;
            case android.view.KeyEvent.KEYCODE_DPAD_DOWN: return DOWN;
            case android.view.KeyEvent.KEYCODE_DPAD_LEFT: return LEFT;
            default: return -1;
        }
    }

    private Gamepad() {}
}
