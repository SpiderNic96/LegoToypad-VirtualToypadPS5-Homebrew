/* UI sound effects for the console picker.
 *
 * Uses the PS5's libSceAudioOut, loaded at runtime (so the payload still links and runs if the
 * library or an audio port is unavailable: the picker just stays silent). WAV files are read from
 * /data/ldmod/sfx/, converted to 48 kHz stereo 16-bit once at start, and played from a worker
 * thread one 256-frame grain at a time, so a new sound can cut the old one instantly.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#include "sfx.h"

void web_log(const char *fmt, ...);
#define SLOG(...) web_log("sfx: " __VA_ARGS__)

/* libkernel: weak so the link cannot fail; checked for NULL at run time */
int sceKernelLoadStartModule(const char *name, size_t argc, const void *argv, unsigned flags,
                             const void *opt, int *res) __attribute__((weak));
int sceKernelDlsym(int handle, const char *symbol, void **addr) __attribute__((weak));

typedef int (*audio_init_fn)(void);
typedef int (*audio_open_fn)(int user, int type, int index, uint32_t len, uint32_t freq, uint32_t param);
typedef int (*audio_output_fn)(int handle, const void *buf);

#define SFX_DIR   "/data/ldmod/sfx/"
#define RATE      48000
#define GRAIN     256            /* frames per sceAudioOutOutput call (~5 ms) */
#define MAX_SECS  10

static const char *NAMES[SFX_COUNT][2] = {
    {"Navigate.wav", "navigate.wav"}, {"Select.wav", "select.wav"},
    {"Move.wav", "move.wav"},         {"Remove.wav", "remove.wav"}};

typedef struct { int16_t *pcm; uint32_t frames; } sound_t;   /* interleaved stereo */
static sound_t g_snd[SFX_COUNT];
static volatile int g_ready;
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_req = -1;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

/* PCM 16-bit mono/stereo WAV -> 48 kHz stereo, scaled by volume (0-100) */
static int load_wav(const char *path, int volume, sound_t *out) {
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
            double v = (s0 + (s1 - s0) * t) * volume / 100.0;
            pcm[i * 2 + c] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
    }
    free(buf);
    out->pcm = pcm;
    out->frames = (uint32_t)of;
    return 0;
}

static int read_volume(void) {
    int v = 70;
    FILE *f = fopen(SFX_DIR "volume.txt", "r");
    if (f) { int x; if (fscanf(f, "%d", &x) == 1) v = x < 0 ? 0 : x > 100 ? 100 : x; fclose(f); }
    return v;
}

static int open_audio(audio_output_fn *output) {
    if (!sceKernelLoadStartModule || !sceKernelDlsym) { SLOG("libkernel module loader unavailable"); return -1; }
    int mod = sceKernelLoadStartModule("/system/common/lib/libSceAudioOut.sprx", 0, 0, 0, 0, 0);
    if (mod < 0) mod = sceKernelLoadStartModule("libSceAudioOut.sprx", 0, 0, 0, 0, 0);
    if (mod < 0) { SLOG("cannot load libSceAudioOut (0x%x)", (unsigned)mod); return -1; }
    audio_init_fn init = 0; audio_open_fn open_ = 0; audio_output_fn out = 0;
    sceKernelDlsym(mod, "sceAudioOutInit", (void **)&init);
    sceKernelDlsym(mod, "sceAudioOutOpen", (void **)&open_);
    sceKernelDlsym(mod, "sceAudioOutOutput", (void **)&out);
    if (!open_ || !out) { SLOG("audio symbols not found"); return -1; }
    if (init) init();
    /* user id 0xFF = system; port types: 0 MAIN, 1 BGM. Format 1 = signed 16-bit stereo. */
    static const int TYPES[] = {0, 1};
    for (unsigned i = 0; i < sizeof TYPES / sizeof TYPES[0]; i++) {
        int h = open_(0xFF, TYPES[i], 0, GRAIN, RATE, 1);
        if (h >= 0) { *output = out; SLOG("audio port opened (type %d)", TYPES[i]); return h; }
        SLOG("sceAudioOutOpen type %d failed (0x%x)", TYPES[i], (unsigned)h);
    }
    return -1;
}

static void *sfx_thread(void *arg) {
    (void)arg;
    int volume = read_volume(), loaded = 0;
    for (int i = 0; i < SFX_COUNT; i++) {
        char path[96];
        for (int n = 0; n < 2; n++) {
            snprintf(path, sizeof path, SFX_DIR "%s", NAMES[i][n]);
            int rc = load_wav(path, volume, &g_snd[i]);
            if (!rc) { loaded++; break; }
            if (rc != -1) { SLOG("%s: unsupported or bad WAV (%d)", path, rc); break; }
        }
    }
    if (!loaded || volume <= 0) { SLOG("no sounds loaded (or volume 0): staying silent"); return 0; }
    audio_output_fn output = 0;
    int h = open_audio(&output);
    if (h < 0) return 0;
    SLOG("ready: %d sound(s), volume %d", loaded, volume);
    g_ready = 1;

    int16_t grain[GRAIN * 2];
    const sound_t *cur = 0;
    uint32_t pos = 0;
    for (;;) {
        pthread_mutex_lock(&g_mx);
        while (g_req < 0 && !cur) pthread_cond_wait(&g_cv, &g_mx);
        if (g_req >= 0) { cur = g_snd[g_req].pcm ? &g_snd[g_req] : 0; pos = 0; g_req = -1; }
        pthread_mutex_unlock(&g_mx);
        if (!cur) continue;
        memset(grain, 0, sizeof grain);
        uint32_t n = cur->frames - pos < GRAIN ? cur->frames - pos : GRAIN;
        memcpy(grain, cur->pcm + (size_t)pos * 2, (size_t)n * 2 * sizeof(int16_t));
        pos += n;
        output(h, grain);          /* blocks for one grain: this paces the loop */
        if (pos >= cur->frames) cur = 0;
    }
    return 0;
}

void sfx_init(void) {
    pthread_t t;
    if (pthread_create(&t, 0, sfx_thread, 0) == 0) pthread_detach(t);
}

void sfx_play(int which) {
    if (!g_ready || which < 0 || which >= SFX_COUNT || !g_snd[which].pcm) return;
    pthread_mutex_lock(&g_mx);
    g_req = which;
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mx);
}
