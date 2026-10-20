package com.strutb.tgrally.remastered;

// Native.java: the game library's C side (ports/remastered/android/host_android.c).
// The host under the games (host.h): these are what the Java activity and the
// touch overlay need of it; everything else the game keeps to itself.
final class Native {
    static {
        System.loadLibrary("remastered");
    }

    // the files' folder (the game extracts its data there); before start()
    static native void init(String root);
    // the process (rm_main) on a thread of its own; once the surface and the data are ready
    static native void start();
    // the SurfaceView's Surface (null when it goes): Vulkan draws into it
    static native void surface(android.view.Surface s);
    // the session ends, as closing the window does
    static native void close();
    // the app's focus: 1 in front, 0 in the background (the game pauses)
    static native void focus(int on);

    // pad 0, the touch screen: x (-1..1), y (positive down), buttons (host.h's bits)
    static native void pad(float x, float y, int buttons);
    // pad 1, a game controller; connected 0: none
    static native void gamepad(float x, float y, float rx, float ry, float z, int buttons, int connected);

    // which game is in front: 0 Top Gear Rally, 1 Boss Rally (mux.h)
    static native int front();
    // 1 while the game in front is a race being driven
    static native int driving();
    // the player's speed, km/h (top speed is read to tell a walk from a stop)
    static native float speed();

    // a tap at nx, ny: fractions of the screen; a swipe's direction; START/pause
    static native void tap(float nx, float ny);
    static native void swipe(float dx, float dy);
    static native void pause();
    // the sound's low-pass cut, Hz (0: off)
    static native void setFilter(int hz);

    private Native() {}
}
