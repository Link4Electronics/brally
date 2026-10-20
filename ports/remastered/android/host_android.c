/* host_android.c: the Android host -- the SurfaceView's window (Vulkan draws
 * into it), the touch screen (the app's Java turns fingers into the pad, the
 * menus' taps and swipes, and a game controller into the second pad), AAudio
 * out, decoded music through MediaCodec, the app's files.
 * host/posix under it supplies time, threads, files, network and processes.
 *
 * The game keeps its own loop on a thread of its own (nativeStart), so this
 * host never runs an Android or Java loop: the Java activity pushes the
 * events (surface, focus, close) and the pad, and host_poll_event waits on
 * them for the time the game asks. The app's Java is GameActivity.java and
 * TouchOverlay.java beside this file; the surface is handed over with
 * ANativeWindow_fromSurface, the music decoded by MediaExtractor/MediaCodec
 * (FLAC) and resampled to the rate the mixer runs at.
 *
 * Android has no GL 3.3 core: the app builds RENDER=vulkan. host_present is
 * here for the software renderer (an ARGB blit through ANativeWindow_lock).
 *
 * The Java side's methods (Native.java): init, start, surface, close, focus,
 * pad, gamepad, tap, swipe, pause, filter, front, driving, speed.
 */
#define _GNU_SOURCE
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <aaudio/AAudio.h>
#include <jni.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "host.h"

#define LOG_TAG "remastered"

/* the process, built as rm_main (ports/remastered/platform/main.c) */
int rm_main(int argc, char **argv);
/* the two games' state the touch needs (mux.c, handoff.c, out.c) */
int   mux_front_game(void);
int   tgr_view_driving(void);
float tgr_race_speed(void);
void  tgr_audio_lowpass(int hz);
void  tgr_touch_tap(float u, float v);
void  tgr_touch_swipe(float dx, float dy);
void  tgr_touch_press(unsigned short button);
int   br_race_driving(void);
void  br_race_key(int key);
void  br_race_tap(float nx, float ny);

enum { MUX_TGR, MUX_BR };
enum { BR_KEY_UP, BR_KEY_DOWN, BR_KEY_LEFT, BR_KEY_RIGHT, BR_KEY_ENTER, BR_KEY_ESCAPE };

static char s_data[1024], s_game[1024], s_cd[1024], s_save[1024], s_music[1024], s_res[1024];
static const char *s_dir = "Top Gear Rally Remastered";

/* ---- the event queue: the Java activity pushes, the game drains -------------- */
enum { QMAX = 256 };
static host_event s_q[QMAX];
static int s_qh, s_qt;
static pthread_mutex_t s_qm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_qc = PTHREAD_COND_INITIALIZER;

static void qpush(const host_event *e)
{
    pthread_mutex_lock(&s_qm);
    if ((s_qt + 1) % QMAX != s_qh) {
        s_q[s_qt] = *e;
        s_qt = (s_qt + 1) % QMAX;
    }
    pthread_cond_signal(&s_qc);
    pthread_mutex_unlock(&s_qm);
}

static void qclose(void)
{
    host_event e;
    memset(&e, 0, sizeof e);
    e.type = HOST_EV_CLOSE;
    qpush(&e);
}

int host_poll_event(host_event *ev, uint32_t wait_ms)
{
    struct timespec ts;
    pthread_mutex_lock(&s_qm);
    while (s_qh == s_qt) {
        if (wait_ms == 0) {
            pthread_mutex_unlock(&s_qm);
            return 0;
        }
        if (wait_ms == 0xFFFFFFFFu) {
            pthread_cond_wait(&s_qc, &s_qm);
        } else {
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += wait_ms / 1000;
            ts.tv_nsec += (long)(wait_ms % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&s_qc, &s_qm, &ts);
        }
        if (s_qh != s_qt)
            break;
        if (wait_ms != 0xFFFFFFFFu)
            break;                                  /* one wait, as the other hosts */
    }
    if (s_qh == s_qt) {
        pthread_mutex_unlock(&s_qm);
        return 0;
    }
    *ev = s_q[s_qh];
    s_qh = (s_qh + 1) % QMAX;
    pthread_mutex_unlock(&s_qm);
    return 1;
}

/* ---- the window (the SurfaceView's surface) -------------------------------- */
static ANativeWindow *s_win;
static pthread_mutex_t s_wm = PTHREAD_MUTEX_INITIALIZER;

void *host_window_handle(void) { return s_win; }

int host_window_open(int width, int height, const char *title)
{
    (void)width; (void)height; (void)title;
    return s_win != NULL;
}

void host_window_close(void)
{
    /* the Java side owns the surface and releases it; the window's ref is let
     * go when the process ends */
    s_win = NULL;
}

int host_window_visible(void) { return s_win != NULL; }

void host_window_lock_aspect(int lock) { (void)lock; }
void host_window_release_register(host_window_release_fn fn, void *user) { (void)fn; (void)user; }
void host_window_release_unregister(host_window_release_fn fn, void *user) { (void)fn; (void)user; }
void host_window_release_all(void) {}

void host_window_pixels(int *w, int *h)
{
    int pw = 0, ph = 0;
    if (s_win) {
        pw = ANativeWindow_getWidth(s_win);
        ph = ANativeWindow_getHeight(s_win);
    }
    *w = pw;
    *h = ph;
}

void host_present(const uint32_t *argb, int w, int h)
{
    ANativeWindow_Buffer buf;
    int x, y;
    if (!s_win)
        return;
    ANativeWindow_setBuffersGeometry(s_win, w, h, WINDOW_FORMAT_RGBA_8888);
    if (ANativeWindow_lock(s_win, &buf, NULL) != 0)
        return;
    for (y = 0; y < h && y < buf.height; y++) {
        uint32_t *dst = (uint32_t *)((uint8_t *)buf.bits + (size_t)y * buf.stride * 4);
        const uint32_t *src = argb + (size_t)y * w;
        for (x = 0; x < w && x < buf.width; x++) {
            uint32_t p = src[x];
            dst[x] = (p & 0xFF00FF00u) | ((p & 0x00FF0000u) >> 16) | ((p & 0x000000FFu) << 16);
        }
    }
    ANativeWindow_unlockAndPost(s_win);
}

/* ---- directories and process ------------------------------------------------ */
void host_set_app_name(const char *dir, const char *title)
{
    if (dir)
        s_dir = dir;
    (void)title;
}

void host_init(int argc, char **argv)
{
    (void)argv;
    (void)argc;
    host_mkdir(s_save);
}

void host_shutdown(void) { host_audio_close(); }

const char *host_resource_dir(void) { return s_res; }
const char *host_game_dir(void) { return s_game; }
const char *host_cd_dir(void) { return s_cd; }
const char *host_save_dir(void) { return s_save; }
const char *host_music_dir(void)
{
    char p[1200];
    snprintf(p, sizeof p, "%s/track02.flac", s_music);
    return access(p, R_OK) == 0 ? s_music : NULL;
}

/* ---- messages to the player (the activity's Toast) -------------------------- */
static JavaVM *s_vm;
static jclass s_cls;
static jmethodID s_msg;

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved)
{
    (void)reserved;
    s_vm = vm;
    return JNI_VERSION_1_6;
}

void host_message_box(const char *text, const char *caption)
{
    JNIEnv *env = NULL;
    jstring js;
    if (!text)
        text = "";
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "%s: %s", caption ? caption : "", text);
    if (!s_vm || !s_cls || !s_msg)
        return;
    if ((*s_vm)->GetEnv(s_vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*s_vm)->AttachCurrentThread(s_vm, &env, NULL) != JNI_OK)
            return;
    }
    js = (*env)->NewStringUTF(env, text);
    if (js) {
        (*env)->CallStaticVoidMethod(env, s_cls, s_msg, js);
        (*env)->DeleteLocalRef(env, js);
    }
}

/* ---- the pad: 0 the touch screen, 1 a game controller ----------------------- */
static pthread_mutex_t s_pm = PTHREAD_MUTEX_INITIALIZER;
static host_pad s_touch;
static host_pad s_gamepad;
static int s_gamepad_on;

int host_pad_read_n(int i, host_pad *o)
{
    memset(o, 0, sizeof *o);
    o->pov = -1;
    pthread_mutex_lock(&s_pm);
    if (i == 0) {
        *o = s_touch;
        o->pov = -1;
    } else if (i == 1 && s_gamepad_on) {
        *o = s_gamepad;
    } else {
        pthread_mutex_unlock(&s_pm);
        return 0;
    }
    pthread_mutex_unlock(&s_pm);
    return 1;
}

int host_pad_read(host_pad *o) { return host_pad_read_n(0, o); }

/* ---- audio out: AAudio pulling the multiplexer's mix ------------------------ */
static AAudioStream *s_stream;
static host_audio_fn s_afn;
static void *s_auser;

static aaudio_data_callback_result_t audio_cb(AAudioStream *s, void *user, void *data, int32_t frames)
{
    (void)s; (void)user;
    if (s_afn)
        s_afn((float *)data, frames, s_auser);
    else
        memset(data, 0, (size_t)frames * 2 * sizeof(float));
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static int audio_build(int rate, int low_latency)
{
    AAudioStreamBuilder *b = NULL;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK || !b)
        return 0;
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSampleRate(b, rate);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
    if (low_latency) {
        AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setFramesPerDataCallback(b, 256);
    }
    AAudioStreamBuilder_setDataCallback(b, audio_cb, NULL);
    if (AAudioStreamBuilder_openStream(b, &s_stream) != AAUDIO_OK || !s_stream)
        s_stream = NULL;
    AAudioStreamBuilder_delete(b);
    return s_stream != NULL;
}

int host_audio_open(int rate, host_audio_fn fn, void *user)
{
    if (s_stream)
        return 1;
    s_afn = fn;
    s_auser = user;
    if (!audio_build(rate, 1) && !audio_build(rate, 0)) {
        s_afn = NULL;
        return 0;
    }
    if (AAudioStream_requestStart(s_stream) != AAUDIO_OK) {
        AAudioStream_close(s_stream);
        s_stream = NULL;
        s_afn = NULL;
        return 0;
    }
    return 1;
}

void host_audio_close(void)
{
    if (!s_stream)
        return;
    AAudioStream_requestStop(s_stream);
    AAudioStream_close(s_stream);
    s_stream = NULL;
    s_afn = NULL;
}

/* ---- the CD's music: MediaExtractor/MediaCodec (FLAC), resampled ------------ */
struct host_stream {
    AMediaExtractor *ex;
    AMediaCodec *codec;
    int src_rate, src_ch, pcm_float;
    int eos, done, decoding;
    float *pcm;                                  /* interleaved stereo, source rate */
    int pcm_n, pcm_cap;
    double pos;                                  /* in source frames */
    int target_rate;
};

static int pcm_append(host_stream *s, const uint8_t *p, int n)
{
    int frames, i, bps = s->pcm_float ? 4 : 2;
    if (s->src_ch < 0)
        s->src_ch = 2;
    if (s->src_ch < 1 || s->src_rate < 1)
        return 0;
    frames = n / (bps * s->src_ch);
    if (frames <= 0)
        return 1;
    if (s->pcm_n + frames > s->pcm_cap) {
        int cap = s->pcm_cap ? s->pcm_cap * 2 : 16384;
        float *q;
        while (cap < s->pcm_n + frames)
            cap *= 2;
        q = (float *)realloc(s->pcm, (size_t)cap * 2 * sizeof(float));
        if (!q)
            return 0;
        s->pcm = q;
        s->pcm_cap = cap;
    }
    for (i = 0; i < frames; i++) {
        float l, r;
        if (s->pcm_float) {
            const float *f = (const float *)p + (size_t)i * s->src_ch;
            l = f[0];
            r = s->src_ch > 1 ? f[1] : f[0];
        } else {
            const int16_t *q = (const int16_t *)p + (size_t)i * s->src_ch;
            l = q[0] / 32768.0f;
            r = s->src_ch > 1 ? q[1] / 32768.0f : q[0] / 32768.0f;
        }
        s->pcm[s->pcm_n * 2] = l;
        s->pcm[s->pcm_n * 2 + 1] = r;
        s->pcm_n++;
    }
    return 1;
}

static void stream_format(host_stream *s, AMediaFormat *f)
{
    int32_t v;
    if (f && AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_SAMPLE_RATE, &v) && v > 0)
        s->src_rate = v;
    if (f && AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &v) && v > 0)
        s->src_ch = v;
    s->pcm_float = 0;
    if (f && AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_PCM_ENCODING, &v))
        s->pcm_float = v == 4;                   /* ENCODING_PCM_FLOAT */
}

/* one pass: queue a packet, take what the decoder gives; 0 when nothing moved */
static int stream_pump(host_stream *s)
{
    int moved = 0;
    ssize_t idx;
    AMediaCodecBufferInfo info;
    ssize_t oi;
    if (s->eos)
        return 0;
    idx = AMediaCodec_dequeueInputBuffer(s->codec, 0);
    if (idx >= 0) {
        size_t cap = 0;
        uint8_t *b = AMediaCodec_getInputBuffer(s->codec, (size_t)idx, &cap);
        if (b && cap > 0) {
            ssize_t n = AMediaExtractor_readSampleData(s->ex, b, (int)cap);
            if (n < 0) {
                AMediaCodec_queueInputBuffer(s->codec, (size_t)idx, 0, 0, 0,
                                             AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                s->eos = 1;
            } else {
                int64_t pts = AMediaExtractor_getSampleTime(s->ex);
                AMediaCodec_queueInputBuffer(s->codec, (size_t)idx, 0, (size_t)n, pts, 0);
                AMediaExtractor_advance(s->ex);
            }
            moved = 1;
        } else {
            AMediaCodec_queueInputBuffer(s->codec, (size_t)idx, 0, 0, 0, 0);
        }
    }
    oi = AMediaCodec_dequeueOutputBuffer(s->codec, &info, 0);
    if (oi == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
        AMediaFormat *f = AMediaCodec_getOutputFormat(s->codec);
        stream_format(s, f);
        if (f)
            AMediaFormat_delete(f);
        moved = 1;
    } else if (oi >= 0) {
        size_t osize = 0;
        uint8_t *ob = AMediaCodec_getOutputBuffer(s->codec, (size_t)oi, &osize);
        if (ob && info.size > 0 && !(info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG))
            pcm_append(s, ob + info.offset, info.size);
        if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM)
            s->eos = 1;
        AMediaCodec_releaseOutputBuffer(s->codec, (size_t)oi, false);
        moved = 1;
    }
    return moved;
}

host_stream *host_stream_open(const char *path, int rate)
{
    host_stream *s = (host_stream *)calloc(1, sizeof *s);
    AMediaExtractor *ex;
    AMediaCodec *codec = NULL;
    AMediaFormat *fmt = NULL;
    int track = -1, i, tracks;
    char mime[64] = "";
    if (!s)
        return NULL;
    s->target_rate = rate > 0 ? rate : 44100;
    s->ex = AMediaExtractor_new();
    ex = s->ex;
    if (!ex)
        goto fail;
    if (AMediaExtractor_setDataSource(ex, path) != AMEDIA_OK)
        goto fail;
    tracks = (int)AMediaExtractor_getTrackCount(ex);
    for (i = 0; i < tracks; i++) {
        AMediaFormat *f = NULL;
        const char *m = NULL;
        if (AMediaExtractor_getTrackFormat(ex, (size_t)i, &f) != AMEDIA_OK || !f)
            continue;
        m = AMediaFormat_getString(f, AMEDIAFORMAT_KEY_MIME, &m);
        if (m && strncmp(m, "audio/", 6) == 0) {
            track = i;
            fmt = f;
            snprintf(mime, sizeof mime, "%s", m);
            break;
        }
        AMediaFormat_delete(f);
    }
    if (track < 0 || !fmt)
        goto fail;
    AMediaExtractor_selectTrack(ex, (size_t)track);
    stream_format(s, fmt);
    if (!s->src_rate)
        s->src_rate = 44100;
    if (!s->src_ch)
        s->src_ch = 2;
    codec = AMediaCodec_createDecoderByType(mime);   /* (the track's format is what configure takes) */
    if (!codec)
        goto fail;
    s->codec = codec;
    if (AMediaCodec_configure(codec, fmt, NULL, NULL, 0) != AMEDIA_OK ||
        AMediaCodec_start(codec) != AMEDIA_OK)
        goto fail;
    AMediaFormat_delete(fmt);
    fmt = NULL;
    s->pcm_cap = 16384;
    s->pcm = (float *)malloc((size_t)s->pcm_cap * 2 * sizeof(float));
    if (!s->pcm)
        goto fail;
    s->pcm[0] = s->pcm[1] = 0.0f;                /* the sample before the first, for the cubic */
    s->pcm_n = 1;
    s->pos = 1.0;
    return s;
fail:
    if (fmt)
        AMediaFormat_delete(fmt);
    if (codec)
        AMediaCodec_delete(codec);
    if (s->ex)
        AMediaExtractor_delete(s->ex);
    free(s);
    return NULL;
}

static float sample_at(const host_stream *s, int i, int c)
{
    if (i < 0)
        i = 0;
    if (i >= s->pcm_n)
        i = s->pcm_n - 1;
    return s->pcm[i * 2 + c];
}

/* Catmull-Rom between p1 and p2 (t in 0..1) */
static float cubic(float p0, float p1, float p2, float p3, float t)
{
    float a = 0.5f * (-p0 + 3.0f * p1 - 3.0f * p2 + p3);
    float b = 0.5f * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3);
    float c = 0.5f * (p2 - p0);
    float y = ((a * t + b) * t + c) * t + p1;
    return y < -1.0f ? -1.0f : y > 1.0f ? 1.0f : y;
}

int host_stream_read(host_stream *s, float *lr, int frames)
{
    int out = 0;
    if (!s || s->done || frames <= 0)
        return 0;
    while (out < frames) {
        int i, tries = 0;
        double t;
        /* the decoder is asked for a few milliseconds' worth at a time: the
         * music thread has 90 ms of slack for each 4K frames it reads */
        while (!s->eos && s->pcm_n < (int)s->pos + 4 && tries++ < 500) {
            if (!stream_pump(s))
                host_sleep_ms(1);
        }
        if (s->pcm_n < (int)s->pos + 2) {
            if (s->eos)
                s->done = 1;
            break;
        }
        i = (int)s->pos;
        t = s->pos - i;
        lr[out * 2] = cubic(sample_at(s, i - 1, 0), sample_at(s, i, 0),
                            sample_at(s, i + 1, 0), sample_at(s, i + 2, 0), (float)t);
        lr[out * 2 + 1] = cubic(sample_at(s, i - 1, 1), sample_at(s, i, 1),
                                sample_at(s, i + 1, 1), sample_at(s, i + 2, 1), (float)t);
        s->pos += (double)s->src_rate / s->target_rate;
        out++;
        if (s->pos > 65536.0) {                   /* keep the buffer small */
            int k = (int)s->pos - 1;
            memmove(s->pcm, s->pcm + (size_t)k * 2, (size_t)(s->pcm_n - k) * 2 * sizeof(float));
            s->pcm_n -= k;
            s->pos -= k;
        }
    }
    if (s->eos && s->pcm_n < (int)s->pos + 2)
        s->done = 1;
    return out;
}

void host_stream_close(host_stream *s)
{
    if (!s)
        return;
    if (s->codec) {
        AMediaCodec_stop(s->codec);
        AMediaCodec_delete(s->codec);
    }
    if (s->ex)
        AMediaExtractor_delete(s->ex);
    free(s->pcm);
    free(s);
}

/* ---- the Java side (Native.java) --------------------------------------------- */
JNIEXPORT void JNICALL
Java_com_strutb_tgrally_remastered_Native_init(JNIEnv *env, jclass cls, jstring root)
{
    const char *r = NULL;
    (void)cls;
    if (root)
        r = (*env)->GetStringUTFChars(env, root, NULL);
    snprintf(s_data, sizeof s_data, "%s", r ? r : "/data/data/com.strutb.tgrally.remastered/files");
    if (root && r)
        (*env)->ReleaseStringUTFChars(env, root, r);
    snprintf(s_res, sizeof s_res, "%s/data", s_data);
    snprintf(s_game, sizeof s_game, "%s/data", s_data);
    snprintf(s_cd, sizeof s_cd, "%s/data/disc", s_data);
    snprintf(s_music, sizeof s_music, "%s/data/music/cd", s_data);
    snprintf(s_save, sizeof s_save, "%s/saves", s_data);
    if (!s_cls) {
        /* the activity's Toast (host_message_box); found here, on the UI thread */
        jclass ac = (*env)->FindClass(env, "com/strutb/tgrally/remastered/GameActivity");
        if (ac) {
            s_cls = (jclass)(*env)->NewGlobalRef(env, ac);
            (*env)->DeleteLocalRef(env, ac);
            if (s_cls)
                s_msg = (*env)->GetStaticMethodID(env, s_cls, "postMessage", "(Ljava/lang/String;)V");
        }
    }
}

static void *game_thread(void *arg)
{
    char name[] = "remastered";
    char *argv[] = { name, NULL };
    (void)arg;
    rm_main(1, argv);
    /* the session is over: the process goes with it (the next launch starts
     * afresh, as a phone game does) */
    exit(0);
    return NULL;
}

JNIEXPORT void JNICALL
Java_com_strutb_tgrally_remastered_Native_start(JNIEnv *env, jclass cls)
{
    pthread_t t;
    pthread_attr_t a;
    (void)env; (void)cls;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 16u << 20);
    if (pthread_create(&t, &a, game_thread, NULL) == 0)
        pthread_detach(t);
    pthread_attr_destroy(&a);
}

JNIEXPORT void JNICALL
Java_com_strutb_tgrally_remastered_Native_surface(JNIEnv *env, jclass cls, jobject surface)
{
    (void)cls;
    pthread_mutex_lock(&s_wm);
    if (!surface) {
        s_win = NULL;                            /* the old ref is kept: the game may still
                                                  * be in a frame on it as it quits */
        pthread_mutex_unlock(&s_wm);
        qclose();
        return;
    }
    {
        ANativeWindow *w = ANativeWindow_fromSurface(env, surface);
        if (w)
            s_win = w;
    }
    pthread_mutex_unlock(&s_wm);
}

JNIEXPORT void JNICALL
Java_com_strutb_tgrally_remastered_Native_close(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    qclose();
}

JNIEXPORT void JNICALL
Java_com_strutb_tgrally_remastered_Native_focus(JNIEnv *env, jclass cls, jint on)
{
    host_event e;
    (void)env; (void)cls;
    memset(&e, 0, sizeof e);
    e.type = HOST_EV_FOCUS;
    e.down = on ? 1 : 0;
    qpush(&e);
}

JNIEXPORT void JNICALL
Java_com_strutb_tgrally_remastered_Native_pad(JNIEnv *env, jclass cls, jfloat x, jfloat y, jint buttons)
{
    (void)env; (void)cls;
    pthread_mutex_lock(&s_pm);
    s_touch.x = x;
    s_touch.y = y;
    s_touch.buttons = (unsigned)buttons;
    pthread_mutex_unlock(&s_pm);
}

JNIEXPORT void JNICALL
Java_com_strutb_tgrally_remastered_Native_gamepad(JNIEnv *env, jclass cls,
                                                   jfloat x, jfloat y, jfloat rx, jfloat ry,
                                                   jfloat z, jint buttons, jint connected)
{
    (void)env; (void)cls;
    pthread_mutex_lock(&s_pm);
    s_gamepad_on = connected != 0;
    s_gamepad.x = x;
    s_gamepad.y = y;
    s_gamepad.rx = rx;
    s_gamepad.ry = ry;
    s_gamepad.z = z;
    s_gamepad.buttons = (unsigned)buttons;
    s_gamepad.pov = -1;
    pthread_mutex_unlock(&s_pm);
}

JNIEXPORT jint JNICALL
Java_com_strutb_tgrally_remastered_Native_front(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    return mux_front_game();
}

JNIEXPORT jint JNICALL
Java_com_strutb_tgrally_remastered_Native_driving(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    return mux_front_game() == MUX_BR ? br_race_driving() : tgr_view_driving();
}

JNIEXPORT jfloat JNICALL
Java_com_strutb_tgrally_remastered_Native_speed(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    return tgr_race_speed();
}

JNIEXPORT void JNICALL
Java_com_strutb_tgrally_remastered_Native_tap(JNIEnv *env, jclass cls, jfloat nx, jfloat ny)
{
    (void)env; (void)cls;
    if (mux_front_game() == MUX_BR)
        br_race_tap(nx, ny);
    else
        tgr_touch_tap(nx, ny);
}

JNIEXPORT void JNICALL
Java_com_strutb_tgrally_remastered_Native_swipe(JNIEnv *env, jclass cls, jfloat dx, jfloat dy)
{
    (void)env; (void)cls;
    if (mux_front_game() != MUX_BR) {
        tgr_touch_swipe(dx, dy);
        return;
    }
    {
        int key;
        if (dx < 0 && -dx >= (dy < 0 ? -dy : dy))
            key = BR_KEY_RIGHT;
        else if (dx >= 0 && dx >= (dy < 0 ? -dy : dy))
            key = BR_KEY_LEFT;
        else if (dy < 0)
            key = BR_KEY_DOWN;
        else
            key = BR_KEY_UP;
        br_race_key(key);
    }
}

JNIEXPORT void JNICALL
Java_com_strutb_tgrally_remastered_Native_pause(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    if (mux_front_game() == MUX_BR)
        br_race_key(BR_KEY_ESCAPE);
    else
        tgr_touch_press(0x1000);                 /* START */
}

JNIEXPORT void JNICALL
Java_com_strutb_tgrally_remastered_Native_setFilter(JNIEnv *env, jclass cls, jint hz)
{
    (void)env; (void)cls;
    tgr_audio_lowpass(hz);
}
