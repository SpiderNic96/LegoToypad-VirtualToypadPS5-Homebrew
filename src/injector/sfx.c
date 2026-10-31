/* UI sound effects for the console picker.
 *
 * A payload process cannot use the PS5 audio stack (sceAudioOutInit never returns there), but SceShellUI
 * can, and the overlay renderer already lives inside it. So this file only
 *   1. decodes the WAV files from /data/ldmod/sfx/ once (to 48 kHz stereo 16-bit),
 *   2. resolves the libSceAudioOut function addresses inside SceShellUI and tells the renderer,
 *   3. uploads the PCM and sends tiny "play N" / "volume N" messages over the overlay link.
 * The renderer (src/overlay/renderer/vtp_shellui.cpp) opens the audio port and mixes. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/nid.h>

#include "sfx.h"
#include "../overlay/common/ovl_proto.h"

void web_log(const char *fmt, ...);
#define SLOG(...) web_log("sfx: " __VA_ARGS__)

/* on-screen PS5 notification, used only when something goes wrong */
typedef struct { char useless1[45]; char message[3075]; } sfx_notify_t;
int sceKernelSendNotificationRequest(int, sfx_notify_t *, size_t, int);
static void sfx_note(const char *fmt, ...) {
    sfx_notify_t req;
    memset(&req, 0, sizeof req);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(req.message, 200, fmt, ap);
    va_end(ap);
    web_log("sfx: %s", req.message);
    sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}

#define SFX_DIR  "/data/ldmod/sfx/"
#define RATE     48000
#define MAX_SECS 10

static const char *NAMES[SFX_COUNT][2] = {
    {"Navigate.wav", "navigate.wav"}, {"Select.wav", "select.wav"},
    {"Move.wav", "move.wav"},         {"Remove.wav", "remove.wav"}};

typedef struct { int16_t *pcm; uint32_t frames; } sound_t;   /* interleaved stereo */
static sound_t g_snd[SFX_COUNT];

static int g_pid, g_need_upload, g_loaded_done, g_up_done, g_volume = 70;
static sfx_send_fn g_send;
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

/* PCM 16-bit mono/stereo WAV -> 48 kHz stereo (full scale; the renderer applies the volume) */
static int load_wav(const char *path, sound_t *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 44 || size > 16 * 1024 * 1024) { fclose(f); return -2; }
    uint8_t *buf = malloc((size_t)size);
    if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size) { free(buf); fclose(f); return -3; }
    fclose(f);
    if (memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) { free(buf); return -4; }

    unsigned fmt = 0, ch = 0, bits = 0;
    uint32_t rate = 0, dlen = 0;
    const uint8_t *data = 0;
    for (long p = 12; p + 8 <= size;) {
        uint32_t len = rd32(buf + p + 4);
        const uint8_t *body = buf + p + 8;
        if (!memcmp(buf + p, "fmt ", 4) && len >= 16 && p + 8 + 16 <= size) {
            fmt = rd16(body); ch = rd16(body + 2); rate = rd32(body + 4); bits = rd16(body + 14);
        } else if (!memcmp(buf + p, "data", 4)) {
            data = body;
            dlen = (p + 8 + (long)len > size) ? (uint32_t)(size - p - 8) : len;
            break;
        }
        p += 8 + (long)len + (len & 1);
    }
    if (!data || (fmt != 1 && fmt != 0xFFFE) || bits != 16 || (ch != 1 && ch != 2) || rate < 8000 || rate > 192000) {
        free(buf);
        return -5;
    }
    uint32_t in_frames = dlen / (2 * ch);
    uint64_t of = (uint64_t)in_frames * RATE / rate;
    if (!in_frames || of > (uint64_t)RATE * MAX_SECS) { free(buf); return -6; }
    int16_t *pcm = malloc((size_t)of * 2 * sizeof(int16_t));
    if (!pcm) { free(buf); return -7; }
    for (uint32_t i = 0; i < (uint32_t)of; i++) {
        double src = (double)i * rate / RATE;
        uint32_t a = (uint32_t)src, b = a + 1 < in_frames ? a + 1 : a;
        double t = src - a;
        for (unsigned c = 0; c < 2; c++) {
            unsigned cc = ch == 1 ? 0 : c;
            int16_t s0 = (int16_t)rd16(data + ((size_t)a * ch + cc) * 2);
            int16_t s1 = (int16_t)rd16(data + ((size_t)b * ch + cc) * 2);
            double v = s0 + (s1 - s0) * t;
            pcm[i * 2 + c] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
    }
    free(buf);
    out->pcm = pcm;
    out->frames = (uint32_t)of;
    return 0;
}

/* resolve the audio functions inside SceShellUI and upload everything (once per overlay link) */
static void try_upload(void) {
    pthread_mutex_lock(&g_mx);
    if (!g_need_upload || !g_send || !g_loaded_done || g_pid <= 0) { pthread_mutex_unlock(&g_mx); return; }
    g_need_upload = 0;
    uint32_t kh = 0;
    if (kernel_dynlib_handle(g_pid, "libSceAudioOut.sprx", &kh) < 0 || !kh) {
        SLOG("ShellUI (pid %d) has no libSceAudioOut loaded", g_pid);
        sfx_note("Sound effects unavailable: the PS5 UI has no audio library");
        pthread_mutex_unlock(&g_mx);
        return;
    }
    ovl_audio a;
    memset(&a, 0, sizeof a);
    a.open = (uint64_t)kernel_dynlib_dlsym(g_pid, kh, "sceAudioOutOpen");
    a.output = (uint64_t)kernel_dynlib_dlsym(g_pid, kh, "sceAudioOutOutput");
    char nid[12];
    if (!a.open && nid_encode("sceAudioOutOpen", nid)) a.open = (uint64_t)kernel_dynlib_resolve(g_pid, kh, nid);
    if (!a.output && nid_encode("sceAudioOutOutput", nid)) a.output = (uint64_t)kernel_dynlib_resolve(g_pid, kh, nid);
    if (!a.open || !a.output) {
        SLOG("ShellUI audio functions not found (open=%d output=%d)", a.open != 0, a.output != 0);
        sfx_note("Sound effects unavailable: audio functions not found");
        pthread_mutex_unlock(&g_mx);
        return;
    }
    for (int i = 0; i < SFX_COUNT; i++) {
        if (!g_snd[i].pcm) continue;
        uint32_t id = (uint32_t)i;
        g_send(OVL_SNDDATA, &id, 4, g_snd[i].pcm, g_snd[i].frames * 4);
    }
    uint32_t vol = (uint32_t)g_volume;
    g_send(OVL_SNDVOL, &vol, 4, 0, 0);
    g_send(OVL_AUDIO, &a, sizeof a, 0, 0);
    g_up_done = 1;
    SLOG("sounds sent to ShellUI (volume %d)", g_volume);
    pthread_mutex_unlock(&g_mx);
}

static void *sfx_thread(void *arg) {
    (void)arg;
    int loaded = 0;
    for (int i = 0; i < SFX_COUNT; i++) {
        char path[96];
        for (int n = 0; n < 2; n++) {
            snprintf(path, sizeof path, SFX_DIR "%s", NAMES[i][n]);
            int rc = load_wav(path, &g_snd[i]);
            if (!rc) { loaded++; break; }
            if (rc != -1) { SLOG("%s: unsupported or bad WAV (%d)", path, rc); break; }
        }
    }
    SLOG("%d sound file(s) loaded", loaded);
    g_loaded_done = 1;
    if (loaded) try_upload();
    return 0;
}

void sfx_init(void) {
    pthread_t t;
    if (pthread_create(&t, 0, sfx_thread, 0) == 0) pthread_detach(t);
}

void sfx_link_up(int shellui_pid, sfx_send_fn send) {
    pthread_mutex_lock(&g_mx);
    g_pid = shellui_pid; g_send = send; g_need_upload = 1; g_up_done = 0;
    pthread_mutex_unlock(&g_mx);
    try_upload();
}

void sfx_set_volume(int percent) {
    g_volume = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    if (g_up_done && g_send) {
        uint32_t vol = (uint32_t)g_volume;
        g_send(OVL_SNDVOL, &vol, 4, 0, 0);
    }
}

void sfx_play(int which) {
    if (g_volume <= 0 || !g_up_done || !g_send || which < 0 || which >= SFX_COUNT || !g_snd[which].pcm) return;
    uint32_t id = (uint32_t)which;
    g_send(OVL_SNDPLAY, &id, 4, 0, 0);
}

void sfx_status(const char *msg) {
    if (!strncmp(msg, "SFX!", 4)) sfx_note("%s", msg + 4);
}
