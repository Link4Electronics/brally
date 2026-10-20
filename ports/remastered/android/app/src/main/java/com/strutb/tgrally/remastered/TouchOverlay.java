package com.strutb.tgrally.remastered;

// TouchOverlay.java: the N64 pad from fingers on the glass, as TouchPad.swift
// does it on the iPhone.
//
// In a race (Native.driving()): the right of the screen is the wheel -- a
// finger there holds the accelerator and, moved sideways, steers (full lock
// at 90 dp either side, the neutral sliding along so the thumb never runs out
// of room); the left 30% brakes and, once stopped, reverses. Elsewhere a tap
// chooses (the menu's targets) and a swipe moves. A two-finger tap is START
// (pause), a three-finger tap steps the sound's low-pass filter. The pause
// button at the top centre pauses a race, as Escape does on a keyboard.

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.os.SystemClock;
import android.view.MotionEvent;
import android.view.View;

final class TouchOverlay extends View {
    private static final int NONE = 0, WHEEL = 1, BRAKE = 2, MENU = 3;

    private static final float STEER_DP = 90;
    private static final float BRAKE_ZONE = 0.3f;
    private static final float TAP_DP = 12;
    private static final float TAP_MS = 250;
    private static final float SWIPE_DP = 30;
    private static final float GESTURE_MS = 400;
    private static final float REVERSE_BELOW_KPH = 3;

    private static final int[] FILTERS = { 0, 8000, 7000, 6000, 5000 };
    private static final String FILTER_KEY = "audioLowpassHz";

    private static final class Finger {
        int role;
        float startX, startY;
        long began;
    }

    private final float dp;
    private final float steerRange, tapSlop, swipeMin;
    private final java.util.HashMap<Integer, Finger> fingers = new java.util.HashMap<>();

    private final Paint gold = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint red = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint white = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint shade = new Paint(Paint.ANTI_ALIAS_FLAG);

    private float wheelX, wheelY, steer;
    private boolean gas, brake, reversing;
    private boolean wheelShown, pedalShown;

    private long gestureBegan;
    private int gestureFingers;
    private boolean gestureMoved;

    private int front = -1;
    private boolean driving;

    private String note;
    private long noteUntil;
    private final SharedPreferences prefs;
    private int filterHz;

    TouchOverlay(Context c) {
        super(c);
        dp = getResources().getDisplayMetrics().density;
        steerRange = STEER_DP * dp;
        tapSlop = TAP_DP * dp;
        swipeMin = SWIPE_DP * dp;
        gold.setStyle(Paint.Style.STROKE);
        gold.setStrokeWidth(6 * dp);
        gold.setColor(0xFFF5D34A);
        red.setStyle(Paint.Style.STROKE);
        red.setStrokeWidth(4 * dp);
        red.setColor(0xFFE84A3C);
        white.setColor(Color.WHITE);
        white.setTextSize(15 * dp);
        white.setTextAlign(Paint.Align.CENTER);
        shade.setColor(0x99000000);
        setBackgroundColor(Color.TRANSPARENT);
        prefs = c.getSharedPreferences("remastered", Context.MODE_PRIVATE);
        filterHz = prefs.getInt(FILTER_KEY, 5000);
        Native.setFilter(filterHz);
    }

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        postInvalidateOnAnimation();
    }

    // ---- each screen refresh: the game in front, the pads, the drawing ----
    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        int f = Native.front();
        if (f != front) {
            front = f;
            reset();
        }
        driving = Native.driving() != 0;
        sample();
        if (driving != pedalShown) {
            pedalShown = driving;
            wheelShown = wheelShown && driving;
        }
        if (driving) {
            drawPedal(canvas);
            if (wheelShown)
                drawWheel(canvas);
            drawPause(canvas);
        }
        if (note != null) {
            long now = SystemClock.uptimeMillis();
            if (now < noteUntil) {
                float tw = white.measureText(note) + 16 * dp;
                canvas.drawRoundRect(new RectF((getWidth() - tw) / 2, 8 * dp,
                                (getWidth() + tw) / 2, 8 * dp + 26 * dp),
                        6 * dp, 6 * dp, shade);
                canvas.drawText(note, getWidth() / 2f, 26 * dp, white);
            } else {
                note = null;
            }
        }
        postInvalidateOnAnimation();
    }

    // ---- the pad the game reads -------------------------------------------
    private void sample() {
        float sx = gas ? steer : 0;
        int buttons;
        float sy = 0;
        if (!brake) {
            reversing = false;
            buttons = gas ? 1 : 0;
        } else if (front == 1) {                     // Boss Rally's reverse: brakes, then backs up
            buttons = 1 << 2;
        } else {
            if (!reversing && Native.speed() < REVERSE_BELOW_KPH)
                reversing = true;
            if (reversing) {
                buttons = 1;
                sy = 1;
            } else {
                buttons = 1 << 1;                   // B
            }
        }
        Native.pad(sx, sy, buttons);
    }

    // ---- touches -------------------------------------------------------------
    private int roleFor(float x) {
        if (!driving)
            return hasRole(MENU) ? NONE : MENU;
        if (x < getWidth() * BRAKE_ZONE)
            return BRAKE;
        return hasRole(WHEEL) ? NONE : WHEEL;
    }

    private boolean hasRole(int role) {
        for (Finger f : fingers.values())
            if (f.role == role)
                return true;
        return false;
    }

    private void beginWheel(float x, float y) {
        wheelX = x;
        wheelY = y;
        steer = 0;
        gas = true;
        wheelShown = true;
    }

    private void setBrake(boolean on) {
        brake = on;
        if (!on)
            reversing = false;
    }

    private void pauseHit(float x, float y) {
        Native.pause();
    }

    @Override
    public boolean onTouchEvent(MotionEvent ev) {
        long now = SystemClock.uptimeMillis();
        driving = Native.driving() != 0;             // (a touch can come before a draw)
        switch (ev.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                if (fingers.isEmpty()) {
                    gestureBegan = now;
                    gestureFingers = 0;
                    gestureMoved = false;
                }
                int i = ev.getActionIndex();
                int id = ev.getPointerId(i);
                float x = ev.getX(i), y = ev.getY(i);
                if (driving && insidePause(x, y)) {
                    pauseHit(x, y);
                    break;
                }
                Finger f = new Finger();
                f.startX = x;
                f.startY = y;
                f.began = now;
                f.role = roleFor(x);
                fingers.put(id, f);
                gestureFingers++;
                if (f.role == WHEEL)
                    beginWheel(x, y);
                else if (f.role == BRAKE)
                    setBrake(true);
                break;
            }
            case MotionEvent.ACTION_MOVE: {
                for (int i = 0; i < ev.getPointerCount(); i++) {
                    int id = ev.getPointerId(i);
                    Finger f = fingers.get(id);
                    if (f == null)
                        continue;
                    float x = ev.getX(i), y = ev.getY(i);
                    if (Math.hypot(x - f.startX, y - f.startY) > tapSlop)
                        gestureMoved = true;
                    if (f.role != WHEEL)
                        continue;
                    float raw = x - wheelX;
                    if (raw > steerRange) {
                        wheelX = x - steerRange;
                        raw = steerRange;
                    } else if (raw < -steerRange) {
                        wheelX = x + steerRange;
                        raw = -steerRange;
                    }
                    steer = raw / steerRange;
                    gas = true;
                }
                break;
            }
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP: {
                int i = ev.getActionIndex();
                end(ev.getPointerId(i), ev.getX(i), ev.getY(i), now, false);
                break;
            }
            case MotionEvent.ACTION_CANCEL: {
                for (int i = 0; i < ev.getPointerCount(); i++)
                    end(ev.getPointerId(i), ev.getX(i), ev.getY(i), now, true);
                break;
            }
            default:
                break;
        }
        if (fingers.isEmpty() && !gestureMoved && gestureFingers > 1
                && now - gestureBegan <= GESTURE_MS + 150) {
            if (gestureFingers == 2)
                Native.pause();
            else if (gestureFingers == 3)
                stepFilter();
        }
        return true;
    }

    private void end(int id, float x, float y, long now, boolean cancelled) {
        Finger f = fingers.remove(id);
        if (f == null)
            return;
        switch (f.role) {
            case WHEEL:
                gas = false;
                steer = 0;
                wheelShown = false;
                break;
            case BRAKE:
                if (!hasRole(BRAKE))
                    setBrake(false);
                break;
            case MENU: {
                if (cancelled || gestureFingers != 1)
                    break;
                float dx = x - f.startX, dy = y - f.startY;
                float dist = (float)Math.hypot(dx, dy);
                if (dist <= tapSlop && now - f.began <= TAP_MS) {
                    // the renderer letterboxes the game's 4:3 frame in the window
                    float fw = getWidth(), fh = getHeight();
                    if (fw * 3 > fh * 4)
                        fw = fh * 4 / 3f;
                    else
                        fh = fw * 3 / 4f;
                    float left = (getWidth() - fw) / 2, top = (getHeight() - fh) / 2;
                    Native.tap((x - left) / fw, (y - top) / fh);
                } else if (dist >= swipeMin) {
                    Native.swipe(dx, dy);
                }
                break;
            }
            default:
                break;
        }
    }

    // ---- the filter: 0 off, then 8, 7, 6, 5 kHz (the mix's fizz above 8 kHz) ----
    private void stepFilter() {
        int i = 0;
        for (int k = 0; k < FILTERS.length; k++)
            if (FILTERS[k] == filterHz)
                i = k;
        filterHz = FILTERS[(i + 1) % FILTERS.length];
        Native.setFilter(filterHz);
        prefs.edit().putInt(FILTER_KEY, filterHz).apply();
        note = filterHz == 0 ? "Sound filter: off" : "Sound filter: " + (filterHz / 1000) + " kHz";
        noteUntil = SystemClock.uptimeMillis() + 2000;
    }

    void reset() {
        fingers.clear();
        gas = false;
        steer = 0;
        setBrake(false);
        wheelShown = false;
        Native.pad(0, 0, 0);
    }

    // ---- drawing -------------------------------------------------------------
    private boolean insidePause(float x, float y) {
        float r = 22 * dp;
        return x >= getWidth() / 2f - r && x <= getWidth() / 2f + r
                && y >= 6 * dp && y <= 6 * dp + 2 * r;
    }

    private void drawPause(Canvas c) {
        float cx = getWidth() / 2f, top = 10 * dp;
        Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        p.setColor(0xB0000000);
        c.drawCircle(cx, top + 16 * dp, 20 * dp, p);
        p.setColor(Color.WHITE);
        c.drawRoundRect(new RectF(cx - 7 * dp, top + 8 * dp, cx - 2 * dp, top + 24 * dp), dp, dp, p);
        c.drawRoundRect(new RectF(cx + 2 * dp, top + 8 * dp, cx + 7 * dp, top + 24 * dp), dp, dp, p);
    }

    private void drawWheel(Canvas c) {
        Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
        fill.setStyle(Paint.Style.FILL);
        fill.setColor(0x33F5D34A);
        c.drawCircle(wheelX, wheelY, 52 * dp, fill);
        gold.setAlpha(230);
        c.drawCircle(wheelX, wheelY, 52 * dp, gold);
        c.save();
        c.rotate(steer * 120, wheelX, wheelY);
        for (int k = 0; k < 3; k++) {
            double a = Math.toRadians(-90 + k * 120);
            c.drawLine(wheelX, wheelY,
                    wheelX + (float)Math.cos(a) * 40 * dp, wheelY + (float)Math.sin(a) * 40 * dp, gold);
        }
        c.restore();
        c.drawCircle(wheelX, wheelY, 11 * dp, gold);
        gold.setAlpha(255);
    }

    private void drawPedal(Canvas c) {
        float w = getWidth() * BRAKE_ZONE * 0.55f;
        float h = 110 * dp;
        float cx = getWidth() * BRAKE_ZONE * 0.5f;
        float cy = getHeight() * 0.62f;
        RectF r = new RectF(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2);
        Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        p.setColor(brake ? 0xCC101010 : 0x80101010);
        c.drawRoundRect(r, 14 * dp, 14 * dp, p);
        red.setAlpha(brake ? 255 : 140);
        c.drawRoundRect(r, 14 * dp, 14 * dp, red);
        for (int k = 0; k < 4; k++) {
            float yy = r.top + 22 * dp + k * 16 * dp;
            c.drawLine(r.left + 16 * dp, yy, r.right - 16 * dp, yy, red);
        }
        Paint t = new Paint(red);
        t.setStyle(Paint.Style.FILL);
        t.setTextSize(12 * dp);
        t.setTextAlign(Paint.Align.CENTER);
        t.setFakeBoldText(true);
        c.drawText("BRAKE", cx, r.bottom - 10 * dp, t);
        red.setAlpha(255);
    }
}
