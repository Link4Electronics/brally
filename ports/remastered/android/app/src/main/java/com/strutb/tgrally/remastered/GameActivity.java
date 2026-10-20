package com.strutb.tgrally.remastered;

// GameActivity.java: Top Gear Rally Remastered on Android -- the SurfaceView
// Vulkan draws into, the touch overlay over it (TouchOverlay.java), the
// controller (Gamepad.java), and the first run's copy of the game's files out
// of the APK. Landscape; the game runs on its own native thread
// (host_android.c: Native.start), the activity merely hands it the surface,
// the focus and the pad, as App.swift does on the iPhone.

import android.app.Activity;
import android.content.SharedPreferences;
import android.content.pm.ActivityInfo;
import android.content.res.AssetManager;
import android.os.Build;
import android.os.Bundle;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

public final class GameActivity extends Activity implements SurfaceHolder.Callback {
    private static GameActivity sThis;

    private SurfaceView screen;
    private TouchOverlay overlay;
    private boolean surfaceReady, dataReady, started;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        sThis = this;
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        hideSystemUi();

        Native.init(getFilesDir().getAbsolutePath());

        screen = new SurfaceView(this);
        screen.getHolder().addCallback(this);
        overlay = new TouchOverlay(this);
        overlay.setFocusable(true);

        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(0xFF000000);
        root.addView(screen, new FrameLayout.LayoutParams(-1, -1));
        root.addView(overlay, new FrameLayout.LayoutParams(-1, -1));
        setContentView(root);

        extractAssets();
    }

    // ---- the game's files: from the APK into filesDir/data, once per build ----
    private void extractAssets() {
        new Thread(() -> {
            boolean ok = true;
            try {
                AssetManager am = getAssets();
                SharedPreferences p = getSharedPreferences("remastered", MODE_PRIVATE);
                String version = read(am, "data/version.txt");
                File data = new File(getFilesDir(), "data");
                File saves = new File(getFilesDir(), "saves");
                if (!saves.isDirectory() && !saves.mkdirs())
                    throw new IOException("no " + saves);
                if (!version.equals(p.getString("assetsVersion", "")) || !new File(data, "disc").isDirectory()) {
                    delete(data);
                    if (!data.mkdirs())
                        throw new IOException("no " + data);
                    copy(am, "data", data);
                    if (!version.isEmpty())
                        p.edit().putString("assetsVersion", version).apply();
                }
            } catch (Exception e) {
                ok = false;
                post("Preparing the game's files failed: " + e.getMessage());
            }
            final boolean good = ok;
            runOnUiThread(() -> {
                dataReady = good;
                maybeStart();
            });
        }, "assets").start();
    }

    private static String read(AssetManager am, String path) throws IOException {
        try (InputStream in = am.open(path)) {
            byte[] b = new byte[256];
            int n = in.read(b);
            StringBuilder s = new StringBuilder();
            for (int i = 0; i < n; i++)
                if (b[i] != '\n' && b[i] != '\r')
                    s.append((char)b[i]);
            return s.toString();
        } catch (IOException e) {
            return "";
        }
    }

    private static void copy(AssetManager am, String path, File to) throws IOException {
        String[] kids = am.list(path);
        if (kids == null || kids.length == 0) {
            File dir = to.getParentFile();
            if (dir != null && !dir.isDirectory())
                dir.mkdirs();
            try (InputStream in = am.open(path); OutputStream out = new FileOutputStream(to)) {
                byte[] b = new byte[1 << 16];
                int n;
                while ((n = in.read(b)) > 0)
                    out.write(b, 0, n);
            }
            return;
        }
        if (!to.isDirectory() && !to.mkdirs())
            throw new IOException("no " + to);
        for (String k : kids)
            copy(am, path + "/" + k, new File(to, k));
    }

    private static void delete(File f) {
        File[] kids = f.listFiles();
        if (kids != null)
            for (File k : kids)
                delete(k);
        //noinspection ResultOfMethodCallIgnored
        f.delete();
    }

    // ---- the surface: the game starts when it and the files are ready ----
    @Override
    public void surfaceCreated(SurfaceHolder h) {
        Native.surface(h.getSurface());
        surfaceReady = true;
        maybeStart();
    }

    @Override
    public void surfaceChanged(SurfaceHolder h, int format, int w, int hgt) {
        // (the renderer reads the window's size itself; the surface's object is the same)
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder h) {
        surfaceReady = false;
        Native.surface(null);                        // the game is told the window is gone
    }

    private void maybeStart() {
        if (surfaceReady && dataReady && !started) {
            started = true;
            Native.start();
        }
    }

    // ---- the process' life: the background pauses it, as on the iPhone ----
    @Override
    protected void onResume() {
        super.onResume();
        hideSystemUi();
        if (started)
            Native.focus(1);
    }

    @Override
    protected void onPause() {
        if (started)
            Native.focus(0);
        Gamepad.reset();
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        Native.close();
        sThis = null;
        super.onDestroy();
    }

    private void hideSystemUi() {
        getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus)
            hideSystemUi();
    }

    // ---- the controller and the back button ----
    @Override
    public boolean dispatchKeyEvent(KeyEvent ev) {
        if (ev.getKeyCode() == KeyEvent.KEYCODE_BACK) {
            if (ev.getAction() == KeyEvent.ACTION_UP)
                Native.close();
            return true;
        }
        if ((ev.getSource() & InputDevice.SOURCE_GAMEPAD) != 0) {
            if (ev.getAction() == KeyEvent.ACTION_DOWN || ev.getAction() == KeyEvent.ACTION_UP)
                if (Gamepad.onKey(ev.getKeyCode(), ev.getAction() == KeyEvent.ACTION_DOWN))
                    return true;
        }
        return super.dispatchKeyEvent(ev);
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent ev) {
        Gamepad.onMotion(ev);
        return super.dispatchGenericMotionEvent(ev);
    }

    // ---- what the native side shows the player (host_message_box, a toast) ----
    public static void postMessage(final String text) {
        post(text);
    }

    private static void post(final String text) {
        final GameActivity a = sThis;
        if (a == null)
            return;
        a.runOnUiThread(() -> Toast.makeText(a, text, Toast.LENGTH_LONG).show());
    }
}
