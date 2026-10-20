/* host_linux.c: the Linux host -- a Wayland window that shows the frames, the
 * keyboard, mouse and game controllers, ALSA out and decoded music.
 * host/posix under it supplies time, threads, files, network and processes.
 *
 * The game keeps its own loop on the main thread, so this host never runs a
 * Wayland event loop of its own: host_poll_event dispatches the display's
 * events with the wait the game asked for. Frames arrive as ARGB pixels
 * through host_present (the software renderer), copied into a shared-memory
 * buffer and shown scaled to the window, or are drawn by the Vulkan renderer
 * into the window's wl_surface (host_wayland_display / host_wayland_surface,
 * render/vulkan) or through an EGL context on it (render/gl).
 *
 *   window    xdg-shell; the compositor decides the size (the game's aspect
 *             is not forced: the Remastered racing fills any shape)
 *   keys      the xkb keymap the compositor sends, through xkbcommon; the
 *             events carry the Win32 virtual key and the DirectInput scan
 *             code the games read
 *   pointer   the seat's pointer, in the size the game asked for
 *   sound     ALSA ("default", a plug device: any rate and format), 32-bit
 *             float stereo, on a thread of its own
 *   music     the CD's FLAC tracks through FFmpeg (libavformat/libavcodec/
 *             libswresample) when the build found it; without it the game
 *             plays silently and host_stream_open answers NULL
 *   pads      evdev gamepads (/dev/input/event*), Xbox/PlayStation/Switch
 *             Pro layouts: two, the second player's (controls.h)
 *
 * Directories (environment, else the defaults):
 *   BR_CDROOT    the CD's files     (default disc/ beside the executable, then reference/brally/data/disc)
 *   BR_GAMEDIR   the install        (default: the CD root)
 *   BR_SAVEDIR   saves and settings (default $XDG_DATA_HOME/Top Gear Rally Remastered)
 *   BR_MUSICDIR  the CD's tracks    (default music/cd beside the executable)
 *   BR_RESDIR    the program's files (default: the executable's folder)
 *
 * The xdg-shell and xdg-decoration client code beside this file
 * (xdg-shell-client-protocol.h, xdg-shell-protocol.c,
 * xdg-decoration-client-protocol.h, xdg-decoration-protocol.c) is
 * wayland-scanner's output from wayland-protocols' stable xdg-shell.xml
 * (version 6) and unstable xdg-decoration-unstable-v1.xml. It is checked in
 * so a build needs only libwayland-client, not the protocol scanner and its
 * XML.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include <alsa/asoundlib.h>

#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-client-protocol.h"
#include "host.h"

#if defined(BR_HAVE_FFMPEG)
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#endif

/* the second player's pad: a host/linux extension, declared here (Remastered
 * patches host.h with it; the standalone ports' other hosts do not have it) */
int host_pad_read_n(int i, host_pad *p);

static char s_cd[1024], s_game[1024], s_save[1024], s_music[1024], s_res[1024];
static const char *s_app_dir = "Boss Rally 64", *s_app_title = "Boss Rally";

void host_set_app_name(const char *dir, const char *title)
{
    if (dir)
        s_app_dir = dir;
    if (title)
        s_app_title = title;
}

static int is_dir(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

void host_init(int argc, char **argv)
{
    const char *e;
    char exe[1024], home[1024];
    ssize_t n;
    char *slash;
    (void)argc;
    (void)argv;

    e = getenv("BR_RESDIR");
    if (e) {
        snprintf(s_res, sizeof s_res, "%s", e);
    } else {
        n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        exe[n > 0 ? n : 0] = 0;
        slash = strrchr(exe, '/');
        if (slash)
            *slash = 0;
        snprintf(s_res, sizeof s_res, "%s", n > 0 ? exe : ".");
    }
    e = getenv("BR_CDROOT");
    if (e)
        snprintf(s_cd, sizeof s_cd, "%s", e);
    else if (is_dir("reference/brally/data/disc"))
        snprintf(s_cd, sizeof s_cd, "reference/brally/data/disc");
    else
        snprintf(s_cd, sizeof s_cd, "%s/disc", s_res);
    e = getenv("BR_GAMEDIR");
    snprintf(s_game, sizeof s_game, "%s", e ? e : s_cd);
    e = getenv("BR_SAVEDIR");
    if (e) {
        snprintf(s_save, sizeof s_save, "%s", e);
    } else {
        e = getenv("XDG_DATA_HOME");
        if (!e || !e[0]) {
            e = getenv("HOME");
            snprintf(home, sizeof home, "%s/.local/share", e && e[0] ? e : ".");
            e = home;
        }
        snprintf(s_save, sizeof s_save, "%s/%s", e, s_app_dir);
    }
    host_mkdir(s_save);
    e = getenv("BR_MUSICDIR");
    snprintf(s_music, sizeof s_music, "%s", e ? e : "");
    if (!e)
        snprintf(s_music, sizeof s_music, "%s/music/cd", s_res);
}

void host_shutdown(void)
{
    host_audio_close();
    host_window_close();
}

const char *host_resource_dir(void)
{
    const char *e = getenv("BR_RESDIR");
    return e ? e : s_res;
}
const char *host_game_dir(void) { return s_game; }
const char *host_cd_dir(void)   { return s_cd; }
const char *host_save_dir(void) { return s_save; }
const char *host_music_dir(void) { return is_dir(s_music) ? s_music : NULL; }

/* ---- keys: evdev code -> DirectInput scan code and Win32 virtual key ------------- */
static const struct { uint16_t ev; uint8_t dik; uint8_t vk; } k_keys[] = {
    { KEY_ESC, 0x01, 0x1B }, { KEY_ENTER, 0x1C, 0x0D }, { KEY_KPENTER, 0x9C, 0x0D },
    { KEY_SPACE, 0x39, 0x20 }, { KEY_TAB, 0x0F, 0x09 }, { KEY_BACKSPACE, 0x0E, 0x08 },
    { KEY_UP, 0xC8, 0x26 }, { KEY_DOWN, 0xD0, 0x28 }, { KEY_LEFT, 0xCB, 0x25 }, { KEY_RIGHT, 0xCD, 0x27 },
    { KEY_LEFTSHIFT, 0x2A, 0x10 }, { KEY_RIGHTSHIFT, 0x36, 0x10 },
    { KEY_LEFTCTRL, 0x1D, 0x11 }, { KEY_RIGHTCTRL, 0x9D, 0x11 },
    { KEY_LEFTALT, 0x38, 0x12 }, { KEY_RIGHTALT, 0xB8, 0x12 },
    { KEY_HOME, 0xC7, 0x24 }, { KEY_END, 0xCF, 0x23 }, { KEY_PAGEUP, 0xC9, 0x21 }, { KEY_PAGEDOWN, 0xD1, 0x22 },
    { KEY_INSERT, 0xD2, 0x2D }, { KEY_DELETE, 0xD3, 0x2E },
    { KEY_F1, 0x3B, 0x70 }, { KEY_F2, 0x3C, 0x71 }, { KEY_F3, 0x3D, 0x72 }, { KEY_F4, 0x3E, 0x73 },
    { KEY_F5, 0x3F, 0x74 }, { KEY_F6, 0x40, 0x75 }, { KEY_F7, 0x41, 0x76 }, { KEY_F8, 0x42, 0x77 },
    { KEY_F9, 0x43, 0x78 }, { KEY_F10, 0x44, 0x79 }, { KEY_F11, 0x57, 0x7A }, { KEY_F12, 0x58, 0x7B },
    { KEY_MINUS, 0x0C, 0xBD }, { KEY_EQUAL, 0x0D, 0xBB }, { KEY_LEFTBRACE, 0x1A, 0xDB },
    { KEY_RIGHTBRACE, 0x1B, 0xDD }, { KEY_BACKSLASH, 0x2B, 0xDC }, { KEY_SEMICOLON, 0x27, 0xBA },
    { KEY_APOSTROPHE, 0x28, 0xDE }, { KEY_GRAVE, 0x29, 0xC0 }, { KEY_COMMA, 0x33, 0xBC },
    { KEY_DOT, 0x34, 0xBE }, { KEY_SLASH, 0x35, 0xBF },
};

static void key_map(uint32_t ev, uint8_t *dik, uint8_t *vk)
{
    size_t i;
    /* the letter and digit rows: the evdev code is the DirectInput scan code */
    if (ev >= KEY_Q && ev <= KEY_P) {
        *dik = (uint8_t)ev;
        *vk = (uint8_t)('Q' + ev - KEY_Q);
        return;
    }
    if (ev >= KEY_A && ev <= KEY_L) {
        *dik = (uint8_t)ev;
        *vk = (uint8_t)('A' + ev - KEY_A);
        return;
    }
    if (ev >= KEY_Z && ev <= KEY_M) {           /* Z, X, C, V, B, N, M */
        static const char tab[] = "ZXCVBNM";
        *dik = (uint8_t)ev;
        *vk = (uint8_t)tab[ev - KEY_Z];
        return;
    }
    if (ev >= KEY_1 && ev <= KEY_0) {
        *dik = (uint8_t)ev;
        *vk = (uint8_t)(ev == KEY_0 ? '0' : '1' + ev - KEY_1);
        return;
    }
    for (i = 0; i < sizeof k_keys / sizeof k_keys[0]; i++)
        if (k_keys[i].ev == ev) {
            *dik = k_keys[i].dik;
            *vk = k_keys[i].vk;
            return;
        }
    *dik = 0;
    *vk = 0;
}

/* ---- the event queue the host fills and host_poll_event drains ------------------ */
#define QMAX 256
static host_event s_q[QMAX];
static int s_qh, s_qt;
static uint8_t s_down[KEY_MAX + 1];          /* by evdev code: held, for focus changes */

static void qpush(const host_event *e)
{
    int n = (s_qt + 1) % QMAX;
    if (n != s_qh) {
        s_q[s_qt] = *e;
        s_qt = n;
    }
}

static void key_event(uint32_t ev, int down, uint32_t ch)
{
    host_event e;
    uint8_t dik, vk;
    if (ev <= KEY_MAX) {
        if (s_down[ev] == down)
            return;                          /* auto-repeat and modifier duplicates */
        s_down[ev] = (uint8_t)down;
    }
    key_map(ev, &dik, &vk);
    if (!vk && !dik && ch == 0)
        return;
    memset(&e, 0, sizeof e);
    e.type = HOST_EV_KEY;
    e.vk = vk;
    e.scan = dik;
    e.down = down;
    qpush(&e);
}

static void char_event(uint32_t ch)
{
    host_event e;
    if (ch == 0 || ch >= 0x80 || ch == 0x7F || (ch < 0x20 && ch != 0x0D && ch != 0x08 && ch != 0x1B))
        return;
    memset(&e, 0, sizeof e);
    e.type = HOST_EV_CHAR;
    e.ch = (int)ch;
    qpush(&e);
}

/* ---- the Wayland window ---------------------------------------------------------- */
static struct wl_display *s_dpy;
static struct wl_registry *s_reg;
static struct wl_compositor *s_comp;
static struct wl_shm *s_shm;
static struct wl_seat *s_seat;
static struct wl_keyboard *s_kb;
static struct wl_pointer *s_ptr;
static struct wl_surface *s_surf;
static struct xdg_wm_base *s_wm;
static struct xdg_surface *s_xsurf;
static struct xdg_toplevel *s_top;
static struct zxdg_decoration_manager_v1 *s_deco_mgr;
static struct zxdg_toplevel_decoration_v1 *s_deco;
static struct xkb_context *s_xkb;
static struct xkb_keymap *s_keymap;
static struct xkb_state *s_xstate;
static int s_open, s_configured, s_focus, s_visible = 1;
static int s_lost;                      /* the display connection went: one close event */
static int s_win_w = 640, s_win_h = 480;     /* what the game asked for (pointer units) */
static int s_px_w, s_px_h;                   /* the window's pixels (a configure) */

/* the renderers' window resources (their swapchains), released when the
 * window passes from one to another (Remastered's two engines; see host.h) */
#define RELEASE_MAX 4
struct release_entry { host_window_release_fn fn; void *user; };
static struct release_entry s_release[RELEASE_MAX];
static int s_nrelease;
static pthread_mutex_t s_release_m = PTHREAD_MUTEX_INITIALIZER;

/* a shared-memory buffer for the software renderer's frames */
struct shmbuf { struct wl_buffer *buf; void *data; size_t size; int w, h; };
static struct shmbuf s_shmbuf;

/* ---- keyboard and pointer --------------------------------------------------------- */
static void kb_keymap(void *d, struct wl_keyboard *kb, uint32_t format, int32_t fd, uint32_t size)
{
    char *map;
    (void)d;
    (void)kb;
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }
    map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map != MAP_FAILED) {
        struct xkb_keymap *km = xkb_keymap_new_from_string(s_xkb, map, XKB_KEYMAP_FORMAT_TEXT_V1,
                                                           XKB_KEYMAP_COMPILE_NO_FLAGS);
        if (km) {
            struct xkb_state *st = xkb_state_new(km);
            if (st) {
                xkb_keymap_unref(s_keymap);
                xkb_state_unref(s_xstate);
                s_keymap = km;
                s_xstate = st;
            } else {
                xkb_keymap_unref(km);
            }
        }
        munmap(map, size);
    }
    close(fd);
}

static void kb_enter(void *d, struct wl_keyboard *kb, uint32_t serial, struct wl_surface *sf,
                     struct wl_array *keys)
{
    host_event e;
    uint32_t *k;
    (void)d; (void)kb; (void)serial; (void)sf;
    s_focus = 1;
    memset(&e, 0, sizeof e);
    e.type = HOST_EV_FOCUS;
    e.down = 1;
    qpush(&e);                                  /* the games pause on leave; this resumes them */
    wl_array_for_each(k, keys)
        key_event(*k, 1, 0);
}

static void kb_leave(void *d, struct wl_keyboard *kb, uint32_t serial, struct wl_surface *sf)
{
    host_event e;
    size_t i;
    (void)d; (void)kb; (void)serial; (void)sf;
    /* keys held when focus leaves go up, or they stay down for good */
    for (i = 0; i <= KEY_MAX; i++)
        if (s_down[i])
            key_event((uint32_t)i, 0, 0);
    memset(&e, 0, sizeof e);
    e.type = HOST_EV_FOCUS;
    e.down = 0;
    qpush(&e);
    s_focus = 0;
}

static void kb_key(void *d, struct wl_keyboard *kb, uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
    int down = state == WL_KEYBOARD_KEY_STATE_PRESSED;
    uint32_t ch = 0;
    (void)d; (void)kb; (void)serial; (void)time;
    if (down && s_xstate)
        ch = xkb_state_key_get_utf32(s_xstate, key + 8);
    key_event(key, down, ch);
    if (down)
        char_event(ch);
}

static void kb_modifiers(void *d, struct wl_keyboard *kb, uint32_t serial, uint32_t dep,
                         uint32_t latched, uint32_t locked, uint32_t group)
{
    (void)d; (void)kb; (void)serial;
    if (s_xstate)
        xkb_state_update_mask(s_xstate, dep, latched, locked, 0, 0, group);
}

static void kb_repeat(void *d, struct wl_keyboard *kb, int32_t rate, int32_t delay)
{
    (void)d; (void)kb; (void)rate; (void)delay;
}

static const struct wl_keyboard_listener k_kb_listener = {
    kb_keymap, kb_enter, kb_leave, kb_key, kb_modifiers, kb_repeat,
};

static int s_ptr_x, s_ptr_y;                  /* the last pointer place, in the game's units */

static void ptr_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *sf,
                      wl_fixed_t sx, wl_fixed_t sy)
{
    (void)d; (void)p; (void)serial; (void)sf;
    s_ptr_x = s_px_w > 0 ? (int)((long)wl_fixed_to_int(sx) * s_win_w / s_px_w) : wl_fixed_to_int(sx);
    s_ptr_y = s_px_h > 0 ? (int)((long)wl_fixed_to_int(sy) * s_win_h / s_px_h) : wl_fixed_to_int(sy);
}

static void ptr_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *sf)
{
    (void)d; (void)p; (void)serial; (void)sf;
}

static void ptr_motion(void *d, struct wl_pointer *p, uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
    host_event e;
    (void)d; (void)p; (void)time;
    s_ptr_x = s_px_w > 0 ? (int)((long)wl_fixed_to_int(sx) * s_win_w / s_px_w) : wl_fixed_to_int(sx);
    s_ptr_y = s_px_h > 0 ? (int)((long)wl_fixed_to_int(sy) * s_win_h / s_px_h) : wl_fixed_to_int(sy);
    memset(&e, 0, sizeof e);
    e.type = HOST_EV_MOUSE;
    e.x = s_ptr_x;
    e.y = s_ptr_y;
    qpush(&e);
}

static void ptr_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t time,
                       uint32_t button, uint32_t state)
{
    host_event e;
    (void)d; (void)p; (void)serial; (void)time;
    if (state != WL_POINTER_BUTTON_STATE_PRESSED && state != WL_POINTER_BUTTON_STATE_RELEASED)
        return;
    memset(&e, 0, sizeof e);
    e.type = HOST_EV_MOUSE;
    e.buttons = state == WL_POINTER_BUTTON_STATE_PRESSED
                    ? (button == BTN_LEFT ? 1 : button == BTN_RIGHT ? 2 : 4)
                    : 0;
    e.x = s_ptr_x;                             /* the place the last motion left it */
    e.y = s_ptr_y;
    qpush(&e);
}

static void ptr_axis(void *d, struct wl_pointer *p, uint32_t time, uint32_t axis, wl_fixed_t value)
{
    (void)d; (void)p; (void)time; (void)axis; (void)value;
}

static void ptr_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void ptr_axis_source(void *d, struct wl_pointer *p, uint32_t s) { (void)d; (void)p; (void)s; }
static void ptr_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) { (void)d; (void)p; (void)t; (void)a; }
static void ptr_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v) { (void)d; (void)p; (void)a; (void)v; }

static const struct wl_pointer_listener k_ptr_listener = {
    ptr_enter, ptr_leave, ptr_motion, ptr_button, ptr_axis,
    ptr_frame, ptr_axis_source, ptr_axis_stop, ptr_axis_discrete,
};

static void seat_caps(void *d, struct wl_seat *seat, uint32_t caps)
{
    (void)d;
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !s_kb) {
        s_kb = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(s_kb, &k_kb_listener, NULL);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && s_kb) {
        wl_keyboard_destroy(s_kb);
        s_kb = NULL;
    }
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !s_ptr) {
        s_ptr = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(s_ptr, &k_ptr_listener, NULL);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && s_ptr) {
        wl_pointer_destroy(s_ptr);
        s_ptr = NULL;
    }
}

static void seat_name(void *d, struct wl_seat *seat, const char *name) { (void)d; (void)seat; (void)name; }

static const struct wl_seat_listener k_seat_listener = { seat_caps, seat_name };

static void wm_ping(void *d, struct xdg_wm_base *wm, uint32_t serial) { (void)d; xdg_wm_base_pong(wm, serial); }
static const struct xdg_wm_base_listener k_wm_listener = { wm_ping };

static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *states)
{
    (void)d; (void)t; (void)states;
    if (w > 0 && h > 0) {
        s_px_w = w;
        s_px_h = h;
    }
}

static void top_close(void *d, struct xdg_toplevel *t)
{
    host_event e;
    (void)d; (void)t;
    memset(&e, 0, sizeof e);
    e.type = HOST_EV_CLOSE;
    qpush(&e);
}

static void top_configure_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h)
{
    (void)d; (void)t; (void)w; (void)h;
}
static void top_wm_capabilities(void *d, struct xdg_toplevel *t, struct wl_array *caps)
{
    (void)d; (void)t; (void)caps;
}

static const struct xdg_toplevel_listener k_top_listener = {
    top_configure, top_close, top_configure_bounds, top_wm_capabilities,
};

static void deco_configure(void *d, struct zxdg_toplevel_decoration_v1 *deco, uint32_t mode)
{
    (void)d; (void)deco; (void)mode;         /* the compositor owns the titlebar */
}
static const struct zxdg_toplevel_decoration_v1_listener k_deco_listener = { deco_configure };

static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial)
{
    (void)d;
    xdg_surface_ack_configure(xs, serial);
    s_configured = 1;
    wl_surface_commit(s_surf);
}

static const struct xdg_surface_listener k_xs_listener = { xs_configure };

static void reg_global(void *d, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t version)
{
    (void)d;
    if (!strcmp(iface, wl_compositor_interface.name))
        s_comp = wl_registry_bind(reg, name, &wl_compositor_interface, version < 4 ? version : 4);
    else if (!strcmp(iface, wl_shm_interface.name))
        s_shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, wl_seat_interface.name)) {
        s_seat = wl_registry_bind(reg, name, &wl_seat_interface, version < 5 ? version : 5);
        wl_seat_add_listener(s_seat, &k_seat_listener, NULL);
    } else if (!strcmp(iface, xdg_wm_base_interface.name)) {
        s_wm = wl_registry_bind(reg, name, &xdg_wm_base_interface, version < 2 ? version : 2);
        xdg_wm_base_add_listener(s_wm, &k_wm_listener, NULL);
    } else if (!strcmp(iface, zxdg_decoration_manager_v1_interface.name)) {
        s_deco_mgr = wl_registry_bind(reg, name, &zxdg_decoration_manager_v1_interface, 1);
    }
}

static void reg_remove(void *d, struct wl_registry *reg, uint32_t name) { (void)d; (void)reg; (void)name; }
static const struct wl_registry_listener k_reg_listener = { reg_global, reg_remove };

int host_window_open(int width, int height, const char *title)
{
    if (s_open)
        return 1;
    s_win_w = width > 0 ? width : 640;
    s_win_h = height > 0 ? height : 480;
    s_dpy = wl_display_connect(NULL);
    if (!s_dpy) {
        fprintf(stderr, "host: no Wayland display (set WAYLAND_DISPLAY)\n");
        return 0;
    }
    s_xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    s_reg = wl_display_get_registry(s_dpy);
    wl_registry_add_listener(s_reg, &k_reg_listener, NULL);
    wl_display_roundtrip(s_dpy);
    if (!s_comp || !s_wm || !s_shm) {
        fprintf(stderr, "host: the compositor lacks wl_compositor, wl_shm or xdg_wm_base\n");
        host_window_close();
        return 0;
    }
    s_surf = wl_compositor_create_surface(s_comp);
    s_xsurf = xdg_wm_base_get_xdg_surface(s_wm, s_surf);
    xdg_surface_add_listener(s_xsurf, &k_xs_listener, NULL);
    s_top = xdg_surface_get_toplevel(s_xsurf);
    xdg_toplevel_add_listener(s_top, &k_top_listener, NULL);
    xdg_toplevel_set_title(s_top, title ? title : s_app_title);
    xdg_toplevel_set_app_id(s_top, s_app_title);
    /* the titlebar and its buttons are the compositor's (server-side), so a
     * compositor that would otherwise leave the client to decorate its own
     * window still gives one; the decoration must exist before the first
     * commit (xdg-decoration v1) */
    if (s_deco_mgr) {
        s_deco = zxdg_decoration_manager_v1_get_toplevel_decoration(s_deco_mgr, s_top);
        zxdg_toplevel_decoration_v1_add_listener(s_deco, &k_deco_listener, NULL);
        zxdg_toplevel_decoration_v1_set_mode(s_deco, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
    wl_surface_commit(s_surf);
    /* the first configure: the size the compositor gives the window */
    while (!s_configured && wl_display_dispatch(s_dpy) != -1)
        ;
    while (wl_display_dispatch_pending(s_dpy) > 0)
        ;
    s_open = 1;
    return 1;
}

static void shmbuf_drop(void)
{
    if (s_shm) {
        if (s_shmbuf.buf)
            wl_buffer_destroy(s_shmbuf.buf);
        if (s_shmbuf.data)
            munmap(s_shmbuf.data, s_shmbuf.size);
    }
    memset(&s_shmbuf, 0, sizeof s_shmbuf);
}

void host_window_close(void)
{
    shmbuf_drop();
    if (s_deco)
        zxdg_toplevel_decoration_v1_destroy(s_deco);
    if (s_deco_mgr)
        zxdg_decoration_manager_v1_destroy(s_deco_mgr);
    if (s_top)
        xdg_toplevel_destroy(s_top);
    if (s_xsurf)
        xdg_surface_destroy(s_xsurf);
    if (s_surf)
        wl_surface_destroy(s_surf);
    if (s_ptr)
        wl_pointer_destroy(s_ptr);
    if (s_kb)
        wl_keyboard_destroy(s_kb);
    if (s_seat)
        wl_seat_destroy(s_seat);
    if (s_wm)
        xdg_wm_base_destroy(s_wm);
    if (s_shm)
        wl_shm_destroy(s_shm);
    if (s_comp)
        wl_compositor_destroy(s_comp);
    if (s_reg)
        wl_registry_destroy(s_reg);
    if (s_dpy)
        wl_display_disconnect(s_dpy);
    if (s_xstate)
        xkb_state_unref(s_xstate);
    if (s_keymap)
        xkb_keymap_unref(s_keymap);
    if (s_xkb)
        xkb_context_unref(s_xkb);
    s_deco = NULL;
    s_deco_mgr = NULL;
    s_top = NULL;
    s_xsurf = NULL;
    s_surf = NULL;
    s_ptr = NULL;
    s_kb = NULL;
    s_seat = NULL;
    s_wm = NULL;
    s_shm = NULL;
    s_comp = NULL;
    s_reg = NULL;
    s_dpy = NULL;
    s_xstate = NULL;
    s_keymap = NULL;
    s_xkb = NULL;
    s_open = s_configured = s_focus = 0;
    s_px_w = s_px_h = 0;
}

int host_window_visible(void) { return s_open && s_configured && s_visible; }

void host_window_lock_aspect(int lock)
{
    /* the compositor owns the size; the games' picture is fitted inside
     * whatever shape the player makes (mux.c asks this of no window) */
    (void)lock;
}

/* the wl_surface the renderers draw into, and the display they share */
void *host_window_handle(void) { return s_surf; }
void *host_wayland_display(void) { return s_dpy; }
void *host_wayland_surface(void) { return s_surf; }

void host_window_release_register(host_window_release_fn fn, void *user)
{
    int i;
    if (!fn)
        return;
    pthread_mutex_lock(&s_release_m);
    for (i = 0; i < s_nrelease; i++)
        if (s_release[i].fn == fn && s_release[i].user == user)
            break;
    if (i == s_nrelease && s_nrelease < RELEASE_MAX) {
        s_release[s_nrelease].fn = fn;
        s_release[s_nrelease].user = user;
        s_nrelease++;
    }
    pthread_mutex_unlock(&s_release_m);
}

void host_window_release_unregister(host_window_release_fn fn, void *user)
{
    int i;
    pthread_mutex_lock(&s_release_m);
    for (i = 0; i < s_nrelease; i++)
        if (s_release[i].fn == fn && s_release[i].user == user) {
            s_release[i] = s_release[--s_nrelease];
            break;
        }
    pthread_mutex_unlock(&s_release_m);
}

void host_window_release_all(void)
{
    struct release_entry r[RELEASE_MAX];
    int i, n;
    pthread_mutex_lock(&s_release_m);
    n = s_nrelease;
    memcpy(r, s_release, (size_t)n * sizeof *r);
    pthread_mutex_unlock(&s_release_m);
    for (i = 0; i < n; i++)
        r[i].fn(r[i].user);
}

void host_window_pixels(int *w, int *h)
{
    if (s_px_w > 0 && s_px_h > 0) {
        *w = s_px_w;
        *h = s_px_h;
    } else {
        *w = s_win_w;
        *h = s_win_h;
    }
}

/* a frame of ARGB pixels (0xAARRGGBB, top row first) into a shared-memory
 * buffer on the window (the software renderer; the window's own shape is the
 * frame's, scaled by the compositor) */
void host_present(const uint32_t *argb, int w, int h)
{
    int stride, fd;
    if (!s_surf || !argb || w <= 0 || h <= 0)
        return;
    if (s_shmbuf.w != w || s_shmbuf.h != h) {
        struct wl_shm_pool *pool;
        shmbuf_drop();
        stride = w * 4;
        fd = memfd_create("br-frame", MFD_CLOEXEC);
        if (fd < 0)
            return;
        if (ftruncate(fd, (off_t)stride * h) != 0) {
            close(fd);
            return;
        }
        s_shmbuf.data = mmap(NULL, (size_t)stride * h, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (s_shmbuf.data == MAP_FAILED) {
            s_shmbuf.data = NULL;
            close(fd);
            return;
        }
        pool = wl_shm_create_pool(s_shm, fd, stride * h);
        s_shmbuf.buf = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool);
        close(fd);
        s_shmbuf.size = (size_t)stride * h;
        s_shmbuf.w = w;
        s_shmbuf.h = h;
    }
    memcpy(s_shmbuf.data, argb, (size_t)w * h * 4);
    wl_surface_attach(s_surf, s_shmbuf.buf, 0, 0);
    wl_surface_damage(s_surf, 0, 0, w, h);
    wl_surface_commit(s_surf);
    wl_display_flush(s_dpy);
}

int host_poll_event(host_event *ev, uint32_t wait_ms)
{
    struct pollfd pf;
    int timeout, r;
    if (!s_dpy) {
        if (wait_ms && wait_ms != 0xFFFFFFFFu)
            host_sleep_ms(wait_ms);
        return 0;
    }
    wl_display_dispatch_pending(s_dpy);
    if (s_qh == s_qt) {
        wl_display_flush(s_dpy);
        pf.fd = wl_display_get_fd(s_dpy);
        pf.events = POLLIN;
        timeout = wait_ms == 0xFFFFFFFFu ? -1 : (int)wait_ms;
        r = poll(&pf, 1, timeout);
        if (r > 0 && (pf.revents & POLLIN)) {
            if (wl_display_dispatch(s_dpy) == -1) {
                s_visible = 0;                 /* the compositor went: the window is gone */
                if (!s_lost) {                 /* (a protocol error: end the app, not a
                                                * music-playing process without a window) */
                    host_event ce;
                    s_lost = 1;
                    memset(&ce, 0, sizeof ce);
                    ce.type = HOST_EV_CLOSE;
                    qpush(&ce);
                }
            }
            wl_display_dispatch_pending(s_dpy);
        }
    }
    if (s_qh == s_qt)
        return 0;
    *ev = s_q[s_qh];
    s_qh = (s_qh + 1) % QMAX;
    return 1;
}

void host_message_box(const char *text, const char *caption)
{
    fprintf(stderr, "host: message box [%s] %s\n", caption ? caption : s_app_title, text ? text : "");
}

/* ---- audio out: ALSA pulling the platform's mixer ------------------------------- */
static snd_pcm_t *s_pcm;
static host_audio_fn s_afn;
static void *s_auser;
static pthread_t s_athread;
static volatile int s_arun, s_aopen;

static void *audio_thread(void *arg)
{
    float buf[2 * 512];
    (void)arg;
    while (s_arun) {
        snd_pcm_sframes_t n;
        if (s_afn)
            s_afn(buf, 512, s_auser);
        else
            memset(buf, 0, sizeof buf);
        n = snd_pcm_writei(s_pcm, buf, 512);
        if (n < 0)
            snd_pcm_recover(s_pcm, (int)n, 1);
    }
    return NULL;
}

int host_audio_open(int rate, host_audio_fn fn, void *user)
{
    if (s_aopen)
        return 1;
    if (snd_pcm_open(&s_pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) {
        fprintf(stderr, "host: no ALSA device\n");
        return 0;
    }
    if (snd_pcm_set_params(s_pcm, SND_PCM_FORMAT_FLOAT_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2,
                           (unsigned)rate, 1, 60000) < 0) {
        fprintf(stderr, "host: ALSA refused %d Hz float stereo\n", rate);
        snd_pcm_close(s_pcm);
        s_pcm = NULL;
        return 0;
    }
    s_afn = fn;
    s_auser = user;
    s_arun = 1;
    s_aopen = 1;
    if (pthread_create(&s_athread, NULL, audio_thread, NULL) != 0) {
        s_arun = 0;
        s_aopen = 0;
        snd_pcm_close(s_pcm);
        s_pcm = NULL;
        return 0;
    }
    return 1;
}

void host_audio_close(void)
{
    if (!s_aopen)
        return;
    s_arun = 0;
    if (s_pcm)
        snd_pcm_drop(s_pcm);            /* (a write blocked in the device returns) */
    pthread_join(s_athread, NULL);
    snd_pcm_close(s_pcm);
    s_pcm = NULL;
    s_afn = NULL;
    s_aopen = 0;
}

/* ---- decoded audio files: FFmpeg reads FLAC, WAV, MP3 and resamples -------------- */
struct host_stream {
#if defined(BR_HAVE_FFMPEG)
    AVFormatContext *fmt;
    AVCodecContext *dec;
    SwrContext *swr;
    AVPacket *pkt;
    AVFrame *frame;
    int stream;
    int eof, have, done;
#endif
};

#if defined(BR_HAVE_FFMPEG)
static void stream_free(host_stream *s)
{
    if (!s)
        return;
    av_frame_free(&s->frame);
    av_packet_free(&s->pkt);
    swr_free(&s->swr);
    avcodec_free_context(&s->dec);
    avformat_close_input(&s->fmt);
    free(s);
}
#endif

host_stream *host_stream_open(const char *path, int rate)
{
#if defined(BR_HAVE_FFMPEG)
    host_stream *s = (host_stream *)calloc(1, sizeof *s);
    const AVCodec *codec;
    if (!s)
        return NULL;
    if (avformat_open_input(&s->fmt, path, NULL, NULL) < 0 ||
        avformat_find_stream_info(s->fmt, NULL) < 0)
        goto fail;
    s->stream = av_find_best_stream(s->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (s->stream < 0 || !codec)
        goto fail;
    s->dec = avcodec_alloc_context3(codec);
    if (!s->dec ||
        avcodec_parameters_to_context(s->dec, s->fmt->streams[s->stream]->codecpar) < 0 ||
        avcodec_open2(s->dec, codec, NULL) < 0)
        goto fail;
#if LIBAVCODEC_VERSION_MAJOR >= 59
    {
        AVChannelLayout out_ch = AV_CHANNEL_LAYOUT_STEREO;
        if (swr_alloc_set_opts2(&s->swr, &out_ch, AV_SAMPLE_FMT_FLT, rate,
                                &s->dec->ch_layout, s->dec->sample_fmt, s->dec->sample_rate,
                                0, NULL) < 0)
            goto fail;
    }
#else
    {
        int64_t in_ch = s->dec->channel_layout;
        if (!in_ch)
            in_ch = av_get_default_channel_layout(s->dec->channels);
        s->swr = swr_alloc_set_opts(NULL, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_FLT, rate,
                                    in_ch, s->dec->sample_fmt, s->dec->sample_rate, 0, NULL);
    }
#endif
    if (!s->swr || swr_init(s->swr) < 0)
        goto fail;
    s->pkt = av_packet_alloc();
    s->frame = av_frame_alloc();
    if (!s->pkt || !s->frame)
        goto fail;
    av_log_set_level(AV_LOG_ERROR);
    return s;
fail:
    stream_free(s);
    return NULL;
#else
    (void)path;
    (void)rate;
    return NULL;
#endif
}

int host_stream_read(host_stream *s, float *lr, int frames)
{
#if defined(BR_HAVE_FFMPEG)
    int got = 0;
    if (!s || s->done || frames <= 0)
        return 0;
    while (got < frames) {
        if (!s->have) {
            int r;
            if (!s->eof) {
                r = av_read_frame(s->fmt, s->pkt);
                if (r < 0) {
                    avcodec_send_packet(s->dec, NULL);      /* drain the decoder */
                    s->eof = 1;
                } else {
                    if (s->pkt->stream_index == s->stream)
                        avcodec_send_packet(s->dec, s->pkt);
                    av_packet_unref(s->pkt);
                }
            }
            r = avcodec_receive_frame(s->dec, s->frame);
            if (r == AVERROR_EOF) {
                s->done = 1;
                break;
            }
            if (r == AVERROR(EAGAIN)) {
                if (s->eof) {
                    s->done = 1;
                    break;
                }
                continue;                                   /* read another packet */
            }
            if (r < 0) {
                s->done = 1;
                break;
            }
            s->have = 1;
        }
        {
            uint8_t *dst = (uint8_t *)lr + (size_t)got * 8;
            int n = swr_convert(s->swr, &dst, frames - got,
                                (const uint8_t **)s->frame->extended_data, s->frame->nb_samples);
            if (n < 0) {
                s->done = 1;
                break;
            }
            got += n;
            s->have = 0;                                    /* the rest stays in the resampler */
            if (n == 0 && got == 0) {
                s->done = 1;                                /* no progress: do not spin */
                break;
            }
        }
    }
    if (!s->done && got < frames && s->eof && !s->have) {
        uint8_t *dst = (uint8_t *)lr + (size_t)got * 8;
        int n = swr_convert(s->swr, &dst, frames - got, NULL, 0);
        if (n > 0)
            got += n;
        else
            s->done = 1;
    }
    return got;
#else
    (void)s;
    (void)lr;
    (void)frames;
    return 0;
#endif
}

void host_stream_close(host_stream *s)
{
#if defined(BR_HAVE_FFMPEG)
    stream_free(s);
#else
    (void)s;
#endif
}

/* ---- game controllers: evdev, up to two ------------------------------------------- */
#define PADS 2
static struct {
    int fd;
    char path[64];
    int tried;
    uint64_t tried_ns;
    int have;
    int abs_lo[ABS_MAX + 1], abs_hi[ABS_MAX + 1];
    int abs[ABS_MAX + 1];
    unsigned buttons;
    int hat_x, hat_y;
} s_pads[PADS];

static int pad_is_gamepad(int fd)
{
    unsigned long bits[8];
    memset(bits, 0, sizeof bits);
    if (ioctl(fd, EVIOCGBIT(0, sizeof bits), bits) < 0)
        return 0;
    if (!(bits[EV_ABS / (8 * sizeof(long))] & (1UL << (EV_ABS % (8 * sizeof(long))))))
        return 0;
    memset(bits, 0, sizeof bits);
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof bits), bits) < 0)
        return 0;
    /* a pad's own buttons, not a mouse's or a keyboard's */
#define HAS(b) (bits[(b) / (8 * sizeof(long))] & (1UL << ((b) % (8 * sizeof(long)))))
    return HAS(BTN_GAMEPAD) || HAS(BTN_JOYSTICK) || HAS(BTN_SOUTH);
#undef HAS
}

static void pad_open(int i)
{
    char path[64];
    int fd, a;
    if (s_pads[i].fd > 0 || (s_pads[i].tried && host_ticks_ns() - s_pads[i].tried_ns < 5000000000ull))
        return;
    s_pads[i].tried = 1;
    s_pads[i].tried_ns = host_ticks_ns();
    for (a = 0; a < 32; a++) {
        snprintf(path, sizeof path, "/dev/input/event%d", a);
        fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0)
            continue;
        if (pad_is_gamepad(fd) && (i == 0 || strcmp(path, s_pads[0].path) != 0)) {
            struct input_absinfo ai;
            int c;
            s_pads[i].fd = fd;
            snprintf(s_pads[i].path, sizeof s_pads[i].path, "%s", path);
            s_pads[i].have = 1;
            for (c = 0; c <= ABS_MAX; c++) {
                if (ioctl(fd, EVIOCGABS(c), &ai) == 0 && ai.minimum != ai.maximum) {
                    s_pads[i].abs_lo[c] = ai.minimum;
                    s_pads[i].abs_hi[c] = ai.maximum;
                    s_pads[i].abs[c] = ai.value;
                }
            }
            return;
        }
        close(fd);
    }
}

static float pad_axis(int i, int code, float lo, float hi, int flip)
{
    int lo_ = s_pads[i].abs_lo[code], hi_ = s_pads[i].abs_hi[code];
    float v;
    if (hi_ <= lo_)
        return 0;
    v = (float)(s_pads[i].abs[code] - lo_) / (float)(hi_ - lo_) * (hi - lo) + lo;
    return flip ? -v : v;
}

static void pad_drain(int i)
{
    struct input_event ev;
    ssize_t n;
    while ((n = read(s_pads[i].fd, &ev, sizeof ev)) == sizeof ev) {
        if (ev.type == EV_ABS) {
            if (ev.code <= ABS_MAX) {
                s_pads[i].abs[ev.code] = ev.value;
                if (ev.code == ABS_HAT0X)
                    s_pads[i].hat_x = ev.value;
                else if (ev.code == ABS_HAT0Y)
                    s_pads[i].hat_y = ev.value;
            }
        } else if (ev.type == EV_KEY) {
            unsigned bit;
            switch (ev.code) {                 /* the Xbox/DirectInput layout host.h carries */
            case BTN_SOUTH:                  bit = 0; break;   /* A */
            case BTN_EAST:                   bit = 1; break;   /* B */
            case BTN_WEST:                   bit = 2; break;   /* X */
            case BTN_NORTH:                  bit = 3; break;   /* Y */
            case BTN_TL:                     bit = 4; break;
            case BTN_TR:                     bit = 5; break;
            case BTN_SELECT: case BTN_BACK:  bit = 6; break;
            case BTN_START:                  bit = 7; break;
            case BTN_THUMBL:                 bit = 8; break;
            case BTN_THUMBR:                 bit = 9; break;
            case BTN_DPAD_UP:                bit = 12; break;
            case BTN_DPAD_RIGHT:             bit = 13; break;
            case BTN_DPAD_DOWN:              bit = 14; break;
            case BTN_DPAD_LEFT:              bit = 15; break;
            default: continue;
            }
            if (ev.value)
                s_pads[i].buttons |= 1u << bit;
            else
                s_pads[i].buttons &= ~(1u << bit);
        } else if (ev.type == EV_SYN && ev.code == SYN_DROPPED) {
            /* the state is stale: nothing to resync from, the reads fill it in */
        }
    }
}

int host_pad_read_n(int i, host_pad *o)
{
    int u, r, d, l;
    memset(o, 0, sizeof *o);
    o->pov = -1;
    if (i < 0 || i >= PADS)
        return 0;
    pad_open(i);
    if (s_pads[i].fd <= 0)
        return 0;
    pad_drain(i);
    /* the left stick, y positive downward, as DirectInput gives it */
    o->x = pad_axis(i, ABS_X, -1, 1, 0);
    o->y = pad_axis(i, ABS_Y, -1, 1, 0);
    o->rx = pad_axis(i, ABS_RX, -1, 1, 0);
    o->ry = pad_axis(i, ABS_RY, -1, 1, 0);
    o->z = pad_axis(i, ABS_RZ, 0, 1, 0) - pad_axis(i, ABS_Z, 0, 1, 0);
    o->buttons = s_pads[i].buttons;
    if (pad_axis(i, ABS_RZ, 0, 1, 0) > 0.5f)
        o->buttons |= 1u << 11;
    if (pad_axis(i, ABS_Z, 0, 1, 0) > 0.5f)
        o->buttons |= 1u << 10;
    u = s_pads[i].hat_y < 0 || (o->buttons & (1u << 12));
    d = s_pads[i].hat_y > 0 || (o->buttons & (1u << 14));
    l = s_pads[i].hat_x < 0 || (o->buttons & (1u << 15));
    r = s_pads[i].hat_x > 0 || (o->buttons & (1u << 13));
    if (u || r || d || l) {
        static const int ang[3][3] = { { 22500, 27000, 31500 }, { 18000, -1, 0 }, { 13500, 9000, 4500 } };
        o->pov = ang[r - l + 1][u - d + 1];
    }
    return 1;
}

int host_pad_read(host_pad *o) { return host_pad_read_n(0, o); }
