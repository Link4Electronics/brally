/* gl_ctx.c: the OpenGL renderer's context (gl_api.h), built with the
 * system's own headers (link64.sh, as the OS layer):
 *
 *   Windows  WGL on the host's window: a pixel format with double buffering,
 *            a legacy context to find wglCreateContextAttribsARB, then a 3.3
 *            core one; offscreen, the same on a hidden window of its own
 *   macOS    CGL, a 3.2+ core profile (4.1 in practice), always offscreen: the
 *            Mac draws with Metal, this is for checking the GL renderer there
 *   Linux    EGL on the Wayland host's wl_surface (a wl_egl_window), a 3.3
 *            core profile; offscreen, a pbuffer
 *
 * Anything else: no context (the renderer fails to open). */
#include <stdio.h>
#include <string.h>

#include "gl_api.h"

#define GL_DEF(ret, name, args) PFN_##name name##_p;
GL_FUNCS(GL_DEF)
#undef GL_DEF

int gl_api_load(void)
{
    int ok = 1;
#define GL_LOAD(ret, name, args) \
    if (!(name##_p = (PFN_##name)gl_ctx_proc(#name))) { fprintf(stderr, "brr: gl: no %s\n", #name); ok = 0; }
    GL_FUNCS(GL_LOAD)
#undef GL_LOAD
    return ok;
}

#if defined(_WIN32)
#include <windows.h>

#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001
typedef HGLRC (WINAPI *PFNCREATEATTRIBS)(HDC, HGLRC, const int *);
typedef BOOL (WINAPI *PFNSWAPINTERVAL)(int);

static HDC s_dc;
static HGLRC s_rc;
static HMODULE s_gl;

void *gl_ctx_proc(const char *name)
{
    void *p = (void *)wglGetProcAddress(name);
    if (!p || p == (void *)1 || p == (void *)2 || p == (void *)3 || p == (void *)-1) {   /* the 1.1 ones: opengl32.dll's */
        if (!s_gl)
            s_gl = LoadLibraryA("opengl32.dll");
        p = s_gl ? (void *)GetProcAddress(s_gl, name) : NULL;
    }
    return p;
}

int gl_ctx_open(void *win)
{
    HWND w = (HWND)win;
    PIXELFORMATDESCRIPTOR pfd;
    HGLRC legacy;
    PFNCREATEATTRIBS create;
    PFNSWAPINTERVAL interval;
    int pf;
    static const int attrs[] = { WGL_CONTEXT_MAJOR_VERSION_ARB, 3, WGL_CONTEXT_MINOR_VERSION_ARB, 3,
                                 WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0 };
    if (!w) {   /* offscreen: a hidden window to hold the context */
        WNDCLASSA wc;
        memset(&wc, 0, sizeof wc);
        wc.style = CS_OWNDC;
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = GetModuleHandleA(NULL);
        wc.lpszClassName = "BrGlHidden";
        RegisterClassA(&wc);
        w = CreateWindowExA(0, "BrGlHidden", "", WS_OVERLAPPEDWINDOW, 0, 0, 16, 16, NULL, NULL, wc.hInstance, NULL);
        if (!w)
            return 0;
    }
    s_dc = GetDC(w);
    memset(&pfd, 0, sizeof pfd);
    pfd.nSize = sizeof pfd;
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    if (!(pf = ChoosePixelFormat(s_dc, &pfd)) || !SetPixelFormat(s_dc, pf, &pfd))
        return 0;
    if (!(legacy = wglCreateContext(s_dc)) || !wglMakeCurrent(s_dc, legacy))
        return 0;
    create = (PFNCREATEATTRIBS)(void *)wglGetProcAddress("wglCreateContextAttribsARB");
    if (!create || !(s_rc = create(s_dc, NULL, attrs))) {
        fprintf(stderr, "brr: gl: no 3.3 core context\n");
        wglMakeCurrent(NULL, NULL);
        wglDeleteContext(legacy);
        return 0;
    }
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(legacy);
    if (!wglMakeCurrent(s_dc, s_rc))
        return 0;
    if ((interval = (PFNSWAPINTERVAL)(void *)wglGetProcAddress("wglSwapIntervalEXT")))
        interval(0);   /* the game paces its own frames */
    return 1;
}

void gl_ctx_swap(void)
{
    if (s_dc)
        SwapBuffers(s_dc);
}

#elif defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>
#include <dlfcn.h>

static CGLContextObj s_ctx;

void *gl_ctx_proc(const char *name)
{
    return dlsym(RTLD_DEFAULT, name);
}

int gl_ctx_open(void *win)
{
    CGLPixelFormatAttribute a[] = { kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core,
                                    kCGLPFAAccelerated, kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
                                    kCGLPFAAlphaSize, (CGLPixelFormatAttribute)8, (CGLPixelFormatAttribute)0 };
    CGLPixelFormatObj pf;
    GLint n;
    (void)win;   /* (the Mac's windows draw with Metal) */
    if (CGLChoosePixelFormat(a, &pf, &n) != kCGLNoError || !pf)
        return 0;
    if (CGLCreateContext(pf, NULL, &s_ctx) != kCGLNoError) {
        CGLDestroyPixelFormat(pf);
        return 0;
    }
    CGLDestroyPixelFormat(pf);
    return CGLSetCurrentContext(s_ctx) == kCGLNoError;
}

void gl_ctx_swap(void) {}

#elif defined(__linux__)
#include <stdlib.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#ifndef EGL_PLATFORM_WAYLAND_KHR
#define EGL_PLATFORM_WAYLAND_KHR 0x31D8
#endif
#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif

/* the Wayland host's, for the surface the context draws into */
void *host_wayland_display(void);
void *host_wayland_surface(void);
void host_window_pixels(int *w, int *h);

struct wl_egl_window;                          /* wayland-egl.h, only for these four */
struct wl_egl_window *wl_egl_window_create(void *surface, int w, int h);
void wl_egl_window_resize(struct wl_egl_window *win, int w, int h, int dx, int dy);
void wl_egl_window_destroy(struct wl_egl_window *win);

static EGLDisplay s_edpy = EGL_NO_DISPLAY;
static EGLSurface s_esurf = EGL_NO_SURFACE;
static EGLContext s_ectx = EGL_NO_CONTEXT;
static struct wl_egl_window *s_ewin;
static int s_ew, s_eh;

void *gl_ctx_proc(const char *name)
{
    return (void *)eglGetProcAddress(name);
}

static EGLDisplay display_for(void *win)
{
    EGLDisplay d = EGL_NO_DISPLAY;
    void *wdpy = host_wayland_display();
    if (wdpy)                                  /* a Wayland session (or a window that needs one) */
        d = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, wdpy, NULL);
    if (d == EGL_NO_DISPLAY && !win && !getenv("WAYLAND_DISPLAY"))
        d = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    if (d == EGL_NO_DISPLAY && !wdpy)
        d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    return d;
}

int gl_ctx_open(void *win)
{
    EGLint major, minor, n;
    EGLConfig cfg;
    EGLint cfg_attr[32];
    int c = 0;
    static const EGLint ctx_attr[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
                                       EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                                       EGL_NONE };
    s_edpy = display_for(win);
    if (s_edpy == EGL_NO_DISPLAY || !eglInitialize(s_edpy, &major, &minor)) {
        fprintf(stderr, "brr: gl: no EGL display\n");
        return 0;
    }
    if (!eglBindAPI(EGL_OPENGL_API)) {
        fprintf(stderr, "brr: gl: no desktop OpenGL in EGL\n");
        return 0;
    }
    cfg_attr[c++] = EGL_SURFACE_TYPE;
    cfg_attr[c++] = win ? EGL_WINDOW_BIT : EGL_PBUFFER_BIT;
    cfg_attr[c++] = EGL_RENDERABLE_TYPE;
    cfg_attr[c++] = EGL_OPENGL_BIT;
    cfg_attr[c++] = EGL_RED_SIZE;   cfg_attr[c++] = 8;
    cfg_attr[c++] = EGL_GREEN_SIZE; cfg_attr[c++] = 8;
    cfg_attr[c++] = EGL_BLUE_SIZE;  cfg_attr[c++] = 8;
    cfg_attr[c++] = EGL_ALPHA_SIZE; cfg_attr[c++] = 8;
    cfg_attr[c++] = EGL_DEPTH_SIZE; cfg_attr[c++] = 24;
    cfg_attr[c++] = EGL_NONE;
    if (!eglChooseConfig(s_edpy, cfg_attr, &cfg, 1, &n) || n < 1) {
        fprintf(stderr, "brr: gl: no EGL config\n");
        return 0;
    }
    if (win) {
        host_window_pixels(&s_ew, &s_eh);
        if (s_ew < 1 || s_eh < 1)
            s_ew = s_eh = 640;
        if (!(s_ewin = wl_egl_window_create(host_wayland_surface(), s_ew, s_eh))) {
            fprintf(stderr, "brr: gl: no wl_egl_window\n");
            return 0;
        }
        s_esurf = eglCreatePlatformWindowSurface(s_edpy, cfg, s_ewin, NULL);
    } else {
        static const EGLint pb_attr[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
        s_esurf = eglCreatePbufferSurface(s_edpy, cfg, pb_attr);
    }
    if (s_esurf == EGL_NO_SURFACE) {
        fprintf(stderr, "brr: gl: no EGL surface\n");
        return 0;
    }
    if (!(s_ectx = eglCreateContext(s_edpy, cfg, EGL_NO_CONTEXT, ctx_attr)) ||
        !eglMakeCurrent(s_edpy, s_esurf, s_esurf, s_ectx)) {
        fprintf(stderr, "brr: gl: no 3.3 core context (EGL %d.%d)\n", major, minor);
        return 0;
    }
    eglSwapInterval(s_edpy, 0);
    return 1;
}

void gl_ctx_swap(void)
{
    if (s_ewin) {                              /* the surface follows the window's size */
        int w, h;
        host_window_pixels(&w, &h);
        if (w > 0 && h > 0 && (w != s_ew || h != s_eh)) {
            wl_egl_window_resize(s_ewin, w, h, 0, 0);
            s_ew = w;
            s_eh = h;
        }
    }
    if (s_edpy != EGL_NO_DISPLAY && s_esurf != EGL_NO_SURFACE)
        eglSwapBuffers(s_edpy, s_esurf);
}

#else
void *gl_ctx_proc(const char *name) { (void)name; return NULL; }
int gl_ctx_open(void *win) { (void)win; return 0; }
void gl_ctx_swap(void) {}
#endif
