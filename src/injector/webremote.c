/* LegoToypad web remote, hosted on the PS5 by the Virtual Toy Pad injector.
 *
 * Serves harrysof's LegoToypad phone UI (Web/ files, unchanged) and its catalog from
 * /data/ldmod/web (built by tools/build_bundle.py), and implements the same HTTP API the
 * desktop app's web remote exposes, backed by the in-game virtual Toy Pad:
 *
 *   GET  /api/catalog /api/state /api/leds /api/favorites /api/sort /api/selection
 *   POST /api/load /api/move /api/clear /api/clearall /api/favorite /api/sort
 *        /api/selection /api/ledmirror
 *   GET  / /index.html /app.js /style.css /InterVariable.woff2 /img/<id> /favicon.ico
 *
 * Figure changes go to the game through the header mailbox (ld_header.mbox), one request at
 * a time, with LegoToypad's timing: a figure on an occupied slot is removed first and placed
 * 100 ms later, and a move is a lift, 500 ms, then a place, so keystone puzzles register it.
 * The LED mirror decodes the game's Toy Pad LED commands (0xC0-0xC8) from the USB sniffer
 * ring and animates them exactly like LegoToypad's ComputeLedFrame().
 *
 * Listens on all interfaces, port 8765, no authentication: anyone on the LAN can use it,
 * the same model as LegoToypad's own web remote.
 */
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <ps5/kernel.h>

#include "../hook/ldhook.h"
#include "webremote.h"

#define WEB_DIR    "/data/ldmod/web"
#define PREFS_FILE "/data/ldmod/web_prefs.txt"
#define WEB_PORT   8765
#define VTOKEN     "?v=ps5"

void web_log(const char *fmt, ...);

/* ------------------------------------------------------------ slot map */

/* LegoToypad's kSlots order (index -> wire pad, label) and the Virtual Toy Pad slot for each
 * (VTP: 0 centre, 1-3 left, 4-6 right). */
static const struct { int pad, vtp; const char *label; } SLOTS[7] = {
    {2, 1, "Left - upper"},      {1, 0, "Center"},           {3, 4, "Right - upper"},
    {2, 2, "Left - lower left"}, {2, 3, "Left - lower right"},
    {3, 5, "Right - lower left"}, {3, 6, "Right - lower right"}};

/* ------------------------------------------------------------ catalog index */

typedef struct { uint32_t fid; int bin; char name[64]; char color[8]; char portrait[48]; } fig_info;
typedef struct { int bin; char kind; char franchise[64]; char base[64]; char *json; } entry_info;

static fig_info *g_figs;    static int g_nfigs;
static entry_info *g_ents;  static int g_nents;
static char g_background[64] = "/img/0" VTOKEN, g_favlogo[64] = "/img/0" VTOKEN;
static int g_appicon;

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = n >= 0 ? malloc((size_t)n + 1) : 0;
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = 0; }
    fclose(f);
    if (b) { b[n] = 0; if (len) *len = (size_t)n; }
    return b;
}

static int split_tabs(char *line, char **col, int max) {
    int n = 0;
    col[n++] = line;
    for (char *p = line; *p && n < max; p++)
        if (*p == '\t') { *p = 0; col[n++] = p + 1; }
    for (char *p = col[n - 1]; *p; p++) if (*p == '\r' || *p == '\n') { *p = 0; break; }
    return n;
}

static void load_index(void) {
    char *data = read_file(WEB_DIR "/figures.tsv", 0);
    if (data) {
        int lines = 0;
        for (char *p = data; *p; p++) lines += *p == '\n';
        g_figs = calloc((size_t)lines + 1, sizeof *g_figs);
        for (char *ln = strtok(data, "\n"); ln && g_figs; ln = strtok(0, "\n")) {
            char *c[5];
            if (split_tabs(ln, c, 5) < 5) continue;
            fig_info *f = &g_figs[g_nfigs++];
            f->fid = (uint32_t)strtoul(c[0], 0, 10);
            f->bin = atoi(c[1]);
            snprintf(f->name, sizeof f->name, "%s", c[2]);
            snprintf(f->color, sizeof f->color, "%s", c[3]);
            snprintf(f->portrait, sizeof f->portrait, "%s", c[4]);
        }
        free(data);
    }
    data = read_file(WEB_DIR "/entries.tsv", 0);
    if (data) {
        int lines = 0;
        for (char *p = data; *p; p++) lines += *p == '\n';
        g_ents = calloc((size_t)lines + 1, sizeof *g_ents);
        char *save = 0;
        for (char *ln = strtok_r(data, "\n", &save); ln && g_ents; ln = strtok_r(0, "\n", &save)) {
            char *c[5];
            if (split_tabs(ln, c, 5) < 5) continue;
            entry_info *e = &g_ents[g_nents++];
            e->bin = atoi(c[0]);
            e->kind = c[1][0];
            snprintf(e->franchise, sizeof e->franchise, "%s", c[2]);
            snprintf(e->base, sizeof e->base, "%s", c[3]);
            e->json = strdup(c[4]);
        }
        free(data);
    }
    data = read_file(WEB_DIR "/ui.tsv", 0);
    if (data) {
        for (char *ln = strtok(data, "\n"); ln; ln = strtok(0, "\n")) {
            char *c[2];
            if (split_tabs(ln, c, 2) < 2) continue;
            if (!strcmp(c[0], "background")) snprintf(g_background, sizeof g_background, "%s", c[1]);
            else if (!strcmp(c[0], "favoritesLogo")) snprintf(g_favlogo, sizeof g_favlogo, "%s", c[1]);
            else if (!strcmp(c[0], "appIcon")) g_appicon = atoi(c[1]);
        }
        free(data);
    }
    web_log("web remote: %d figures, %d catalog entries", g_nfigs, g_nents);
}

static const fig_info *fig_by_id(uint32_t fid) {
    for (int i = 0; i < g_nfigs; i++) if (g_figs[i].fid == fid) return &g_figs[i];
    return 0;
}
static const entry_info *entry_by_bin(int bin) {
    for (int i = 0; i < g_nents; i++) if (g_ents[i].bin == bin) return &g_ents[i];
    return 0;
}

/* figure id of a catalog tag, decoded from its bin file */
uint32_t ld_tag_figure_id(const uint8_t *tag180);
static uint32_t bin_figure_id(int bin) {
    char path[96];
    snprintf(path, sizeof path, WEB_DIR "/bins/%d.bin", bin);
    size_t n = 0;
    char *d = read_file(path, &n);
    uint32_t id = d && n == LD_TAG_SIZE ? ld_tag_figure_id((const uint8_t *)d) : 0;
    free(d);
    return id;
}

/* ------------------------------------------------------------ shared state */

static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;   /* game link, prefs, LEDs, status */
static pthread_mutex_t g_cmd = PTHREAD_MUTEX_INITIALIZER;  /* one figure command at a time */
static pid_t g_pid;
static intptr_t g_mem;
static char g_status[160] = "Start LEGO Dimensions on the PS5.";

static int g_sort, g_led_mirror = 1;
static int g_fav[256], g_nfav;
static struct { int world; char virt[12]; int bin, ability; } g_sel = {-1, "", 0, -1};

static uint64_t ms_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void set_status(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&g_mx);
    vsnprintf(g_status, sizeof g_status, fmt, ap);
    pthread_mutex_unlock(&g_mx);
    va_end(ap);
}

static void save_prefs(void) {
    FILE *f = fopen(PREFS_FILE, "w");
    if (!f) return;
    fprintf(f, "sort=%d\nledmirror=%d\nfavorites=", g_sort, g_led_mirror);
    for (int i = 0; i < g_nfav; i++) fprintf(f, "%s%d", i ? "," : "", g_fav[i]);
    fprintf(f, "\n");
    fclose(f);
}

static void load_prefs(void) {
    FILE *f = fopen(PREFS_FILE, "r");
    if (!f) return;
    char ln[2048];
    while (fgets(ln, sizeof ln, f)) {
        int v;
        if (sscanf(ln, "sort=%d", &v) == 1 && v >= 0 && v <= 6) g_sort = v;
        else if (sscanf(ln, "ledmirror=%d", &v) == 1) g_led_mirror = v != 0;
        else if (!strncmp(ln, "favorites=", 10)) {
            g_nfav = 0;
            for (char *p = ln + 10; *p && *p != '\n' && g_nfav < 256;) {
                char *end;
                long b = strtol(p, &end, 10);
                if (end == p) break;
                g_fav[g_nfav++] = (int)b;
                p = *end == ',' ? end + 1 : end;
            }
        }
    }
    fclose(f);
}

void web_attach(pid_t pid, intptr_t mem) {
    pthread_mutex_lock(&g_mx);
    g_pid = pid; g_mem = mem;
    snprintf(g_status, sizeof g_status, "Connected to LEGO Dimensions on the PS5.");
    pthread_mutex_unlock(&g_mx);
}

void web_detach(void) {
    pthread_mutex_lock(&g_mx);
    g_pid = 0; g_mem = 0;
    snprintf(g_status, sizeof g_status, "LEGO Dimensions closed. Start it again on the PS5.");
    pthread_mutex_unlock(&g_mx);
}

static int game_link(pid_t *pid, intptr_t *mem) {
    pthread_mutex_lock(&g_mx);
    *pid = g_pid; *mem = g_mem;
    pthread_mutex_unlock(&g_mx);
    return *pid > 0 && *mem;
}

/* read the 7 VTP slots: used flag and figure id */
static int read_pad(int used[7], uint32_t id[7]) {
    pid_t pid; intptr_t mem;
    for (int i = 0; i < 7; i++) { used[i] = 0; id[i] = 0; }
    if (!game_link(&pid, &mem)) return -1;
    ld_figure fig[LD_MAX_SLOTS];
    if (kernel_proc_copyout(pid, mem + offsetof(ld_header, pad) + offsetof(ld_pad, fig), fig, sizeof fig)) return -1;
    for (int i = 0; i < 7; i++) { used[i] = fig[i].used != 0; id[i] = fig[i].id; }
    return 0;
}

/* one mailbox request, waiting until the game has handled it (it polls on every pad read) */
static int mailbox(uint32_t op, uint32_t a, uint32_t b) {
    pid_t pid; intptr_t mem;
    if (!game_link(&pid, &mem)) return -2;
    intptr_t mb = mem + offsetof(ld_header, mbox);
    uint32_t req = 0, done = 0;
    kernel_proc_copyout(pid, mb + offsetof(typeof(((ld_header *)0)->mbox), done), &done, 4);
    uint32_t args[3] = {op, a, b};
    kernel_proc_copyin(pid, args, mb + offsetof(typeof(((ld_header *)0)->mbox), op), sizeof args);
    req = done + 1;
    kernel_proc_copyin(pid, &req, mb + offsetof(typeof(((ld_header *)0)->mbox), req), 4);
    for (uint64_t t0 = ms_now(); ms_now() - t0 < 3000;) {
        kernel_proc_copyout(pid, mb + offsetof(typeof(((ld_header *)0)->mbox), done), &done, 4);
        if (done == req) {
            int32_t res = -1;
            kernel_proc_copyout(pid, mb + offsetof(typeof(((ld_header *)0)->mbox), result), &res, 4);
            return res;
        }
        usleep(10 * 1000);
    }
    return -3;   /* game not polling input: paused in a system menu, or loading */
}

static const char *figure_name(uint32_t fid, char *buf, size_t n) {
    const fig_info *f = fig_by_id(fid);
    if (f) return f->name;
    snprintf(buf, n, "figure %u", fid);
    return buf;
}

/* ------------------------------------------------------------ LED mirror */

enum { LED_OFF, LED_SOLID, LED_FLASH, LED_FADE };
typedef struct {
    int mode;
    uint8_t r, g, b, fr, fg, fb;
    int on, off, count, speed;
    uint64_t start;
    int has_base;
    uint8_t br, bg, bb;
    /* emulator-side record of the last command, used for the "unchanged" check */
    uint8_t wmode, wr, wg, wb, won, woff, wcount, wspeed;
} led_region;
static led_region g_led[3];   /* LegoToypad region order: 0 left, 1 centre, 2 right */
static uint32_t g_led_rng = 0x9E3779B9u;

static int region_for_pad(int pad) { return pad == 2 ? 0 : pad == 1 ? 1 : pad == 3 ? 2 : -1; }

/* shadPS4 DimensionsToypad::SetLedState + LegoToypad ApplyLedCommand */
static void led_set(int pad, int mode, uint8_t r, uint8_t g, uint8_t b, int on, int off, int count, int speed) {
    int pads[3] = {1, 2, 3}, np = 0;
    if (pad == 0) np = 3; else { pads[0] = pad; np = 1; }
    for (int i = 0; i < np; i++) {
        int reg = region_for_pad(pads[i]);
        if (reg < 0) continue;
        led_region *L = &g_led[reg];
        if (L->wmode == mode && L->wr == r && L->wg == g && L->wb == b && L->won == on && L->woff == off &&
            L->wcount == count && L->wspeed == speed && L->start)
            continue;
        uint8_t fr = L->wr, fg = L->wg, fb = L->wb;   /* fade starts from the pad's previous colour */
        L->wmode = (uint8_t)mode; L->wr = r; L->wg = g; L->wb = b;
        L->won = (uint8_t)on; L->woff = (uint8_t)off; L->wcount = (uint8_t)count; L->wspeed = (uint8_t)speed;
        if (mode != LED_FLASH) { L->has_base = 1; L->br = r; L->bg = g; L->bb = b; }
        L->mode = mode; L->r = r; L->g = g; L->b = b;
        if (mode == LED_FADE) { L->fr = fr; L->fg = fg; L->fb = fb; }
        L->on = on; L->off = off; L->count = count; L->speed = speed;
        L->start = ms_now();
    }
}

static uint8_t led_rand(void) {
    g_led_rng ^= g_led_rng << 13; g_led_rng ^= g_led_rng >> 17; g_led_rng ^= g_led_rng << 5;
    return (uint8_t)g_led_rng;
}

/* one OUT report from the game: 55 len cmd seq args... */
void web_feed_out(const uint8_t *m, int len) {
    if (len < 25 || m[0] != 0x55) return;
    pthread_mutex_lock(&g_mx);
    switch (m[2]) {
    case 0xC0: led_set(m[4], LED_SOLID, m[5], m[6], m[7], 0, 0, 0, 0); break;
    case 0xC2: led_set(m[4], LED_FADE, m[7], m[8], m[9], 0, 0, m[6], m[5]); break;
    case 0xC3: led_set(m[4], LED_FLASH, m[8], m[9], m[10], m[5], m[6], m[7] == 0xFF ? 0 : m[7], 0); break;
    case 0xC4:
        if (m[4] == 0) for (int p = 1; p <= 3; p++) led_set(p, LED_FADE, led_rand(), led_rand(), led_rand(), 0, 0, m[6], m[5]);
        else led_set(m[4], LED_FADE, led_rand(), led_rand(), led_rand(), 0, 0, m[6], m[5]);
        break;
    case 0xC6:
        for (int r = 0; r < 3; r++) {
            const uint8_t *o = m + 4 + r * 6;
            if (!o[0]) led_set(r + 1, LED_OFF, 0, 0, 0, 0, 0, 0, 0);
            else led_set(r + 1, LED_FADE, o[3], o[4], o[5], 0, 0, o[2], o[1]);
        }
        break;
    case 0xC7:
        for (int r = 0; r < 3; r++) {
            const uint8_t *o = m + 4 + r * 7;
            if (!o[0]) led_set(r + 1, LED_OFF, 0, 0, 0, 0, 0, 0, 0);
            else led_set(r + 1, LED_FLASH, o[4], o[5], o[6], o[1], o[2], o[3] == 0xFF ? 0 : o[3], 0);
        }
        break;
    case 0xC8:
        for (int r = 0; r < 3; r++) {
            const uint8_t *o = m + 4 + r * 4;
            if (!o[0]) led_set(r + 1, LED_OFF, 0, 0, 0, 0, 0, 0, 0);
            else led_set(r + 1, LED_SOLID, o[1], o[2], o[3], 0, 0, 0, 0);
        }
        break;
    }
    pthread_mutex_unlock(&g_mx);
}

/* LegoToypad ComputeLedFrame(); caller holds g_mx */
static float led_frame(led_region *L, uint64_t now, uint8_t *cr, uint8_t *cg, uint8_t *cb) {
    const int tick = 40, levels = 9;
    *cr = L->r; *cg = L->g; *cb = L->b;
    if (L->mode == LED_OFF) return 0.0f;
    if (L->mode == LED_SOLID) return 1.0f;
    int elapsed = (int)(now - L->start);
    if (L->mode == LED_FLASH) {
        int period = (L->on + L->off) * tick;
        if (period <= 0) return 1.0f;
        if (L->count > 0 && elapsed >= period * L->count) {
            if (L->has_base) {
                L->mode = LED_SOLID; L->r = L->br; L->g = L->bg; L->b = L->bb;
                *cr = L->r; *cg = L->g; *cb = L->b;
                return 1.0f;
            }
            L->mode = LED_OFF;
            return 0.0f;
        }
        return (elapsed % period) < L->on * tick ? 1.0f : 0.0f;
    }
    int step = (L->speed > 0 ? L->speed : 25) * tick;
    int idx = elapsed / step;
    float t = (float)(elapsed % step) / (float)step;
    if (L->count > 0 && idx >= L->count) { idx = L->count - 1; t = 1.0f; }
    float eased = (float)(0.5 - 0.5 * cos(t * 3.14159265358979323846));
    float q = roundf(eased * (levels - 1)) / (float)(levels - 1);
    int fwd = (idx % 2) == 0;
    uint8_t sr = fwd ? L->fr : L->r, sg = fwd ? L->fg : L->g, sb = fwd ? L->fb : L->b;
    uint8_t er = fwd ? L->r : L->fr, eg = fwd ? L->g : L->fg, eb = fwd ? L->b : L->fb;
    *cr = (uint8_t)(sr + (er - sr) * q + 0.5f);
    *cg = (uint8_t)(sg + (eg - sg) * q + 0.5f);
    *cb = (uint8_t)(sb + (eb - sb) * q + 0.5f);
    if (L->count > 0 && idx == L->count - 1 && t >= 1.0f) {
        L->r = *cr; L->g = *cg; L->b = *cb;
        L->mode = LED_SOLID;
        L->has_base = 1; L->br = L->r; L->bg = L->g; L->bb = L->b;
    }
    return 1.0f;
}

/* ------------------------------------------------------------ JSON helpers */

typedef struct { char *p; size_t n, cap; } sbuf;
static void sb_add(sbuf *s, const char *t, size_t n) {
    if (s->n + n + 1 > s->cap) {
        size_t c = s->cap ? s->cap * 2 : 1024;
        while (c < s->n + n + 1) c *= 2;
        char *np = realloc(s->p, c);
        if (!np) return;
        s->p = np; s->cap = c;
    }
    memcpy(s->p + s->n, t, n);
    s->n += n;
    s->p[s->n] = 0;
}
static void sb_puts(sbuf *s, const char *t) { sb_add(s, t, strlen(t)); }
static void sb_printf(sbuf *s, const char *fmt, ...) {
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0) sb_add(s, tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
}
static void sb_jstr(sbuf *s, const char *t) {
    sb_puts(s, "\"");
    for (; *t; t++) {
        unsigned char c = (unsigned char)*t;
        if (c == '"') sb_puts(s, "\\\"");
        else if (c == '\\') sb_puts(s, "\\\\");
        else if (c < 0x20) sb_printf(s, "\\u%04x", c);
        else sb_add(s, (const char *)&c, 1);
    }
    sb_puts(s, "\"");
}

static int json_int(const char *body, const char *key, int def) {
    char pat[40];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = body ? strstr(body, pat) : 0;
    if (!p) return def;
    p = strchr(p + strlen(pat), ':');
    if (!p) return def;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (!strncmp(p, "true", 4)) return 1;
    if (!strncmp(p, "false", 5)) return 0;
    char *end;
    long v = strtol(p, &end, 10);
    return end == p ? def : (int)v;
}

static void ok_json(sbuf *s, const char *msg) { sb_puts(s, "{\"ok\":true,\"status\":"); sb_jstr(s, msg); sb_puts(s, "}"); }
static void err_json(sbuf *s, const char *msg) { sb_puts(s, "{\"ok\":false,\"status\":"); sb_jstr(s, msg); sb_puts(s, "}"); }

/* ------------------------------------------------------------ API */

static void api_state(sbuf *s) {
    int used[7]; uint32_t id[7];
    read_pad(used, id);
    sb_puts(s, "{\"background\":"); sb_jstr(s, g_background); sb_puts(s, ",\"pads\":[");
    for (int i = 0; i < 7; i++) {
        int v = SLOTS[i].vtp;
        const fig_info *f = used[v] ? fig_by_id(id[v]) : 0;
        char nm[32];
        sb_printf(s, "%s{\"index\":%d,\"pad\":%d,\"label\":", i ? "," : "", i, SLOTS[i].pad);
        sb_jstr(s, SLOTS[i].label);
        sb_printf(s, ",\"occupied\":%s,\"name\":", used[v] ? "true" : "false");
        sb_jstr(s, used[v] ? figure_name(id[v], nm, sizeof nm) : "");
        sb_puts(s, ",\"color\":"); sb_jstr(s, f ? f->color : "#000000");
        sb_printf(s, ",\"bin\":%d,\"portrait\":", f ? f->bin : 0);
        sb_jstr(s, f ? f->portrait : "/img/0" VTOKEN);
        sb_puts(s, "}");
    }
    pthread_mutex_lock(&g_mx);
    sb_printf(s, "],\"sort\":%d,\"ledMirror\":%s,\"status\":", g_sort, g_led_mirror ? "true" : "false");
    sb_jstr(s, g_status);
    pthread_mutex_unlock(&g_mx);
    sb_puts(s, "}");
}

static void api_leds(sbuf *s) {
    static const char *names[] = {"off", "solid", "flash", "fade"};
    uint64_t now = ms_now();
    pthread_mutex_lock(&g_mx);
    sb_printf(s, "{\"enabled\":%s,\"regions\":[", g_led_mirror ? "true" : "false");
    for (int r = 0; r < 3; r++) {
        uint8_t cr, cg, cb;
        float in = led_frame(&g_led[r], now, &cr, &cg, &cb);
        /* LegoToypad CalibrateLedColor: the wire's white point is (255,110,24) */
        int R = cr * 255 / 255, G = cg * 255 / 110, B = cb * 255 / 24;
        R = R > 255 ? 255 : R; G = G > 255 ? 255 : G; B = B > 255 ? 255 : B;
        sb_printf(s, "%s{\"region\":%d,\"mode\":\"%s\",\"color\":\"#%02X%02X%02X\",\"intensity\":%.3f}", r ? "," : "", r,
                  names[g_led[r].mode], R, G, B, g_led[r].mode == LED_OFF ? 0.0 : (double)in);
    }
    pthread_mutex_unlock(&g_mx);
    sb_puts(s, "]}");
}

static const char *sort_label(int id) {
    static const char *l[] = {"Default", "User", "Story", "Favorites", "Year 1", "Year 2", "Abilities"};
    return id >= 0 && id <= 6 ? l[id] : "Default";
}

static void api_sort(sbuf *s) {
    pthread_mutex_lock(&g_mx);
    sb_printf(s, "{\"ok\":true,\"sort\":%d,\"label\":", g_sort);
    sb_jstr(s, sort_label(g_sort));
    pthread_mutex_unlock(&g_mx);
    sb_puts(s, ",\"userOrder\":[");
    int nworld = 0;
    for (int i = 0; i < g_nents; i++) {   /* franchises in catalog order */
        int seen = 0;
        if (!strcmp(g_ents[i].franchise, "Special")) continue;
        for (int j = 0; j < i; j++) if (!strcmp(g_ents[j].franchise, g_ents[i].franchise)) { seen = 1; break; }
        if (!seen) sb_printf(s, "%s%d", nworld ? "," : "", nworld), nworld++;
    }
    sb_puts(s, "]}");
}

static void api_selection(sbuf *s) {
    pthread_mutex_lock(&g_mx);
    sb_printf(s, "{\"ok\":true,\"world\":%d,\"virtual\":", g_sel.world);
    sb_jstr(s, g_sel.virt);
    sb_printf(s, ",\"bin\":%d,\"ability\":%d}", g_sel.bin, g_sel.ability);
    pthread_mutex_unlock(&g_mx);
}

static void api_favorites(sbuf *s) {
    sb_puts(s, "{\"name\":\"Favorites\",\"logo\":");
    sb_jstr(s, g_favlogo);
    sb_puts(s, ",\"characters\":[");
    pthread_mutex_lock(&g_mx);
    int first = 1;
    for (int i = 0; i < g_nfav; i++) {
        const entry_info *e = entry_by_bin(g_fav[i]);
        if (!e || e->kind != 'c') continue;
        if (!first) sb_puts(s, ",");
        first = 0;
        sb_puts(s, e->json);
    }
    sb_puts(s, "],\"vehicles\":[");
    first = 1;
    for (int i = 0; i < g_nfav; i++) {
        const entry_info *e = entry_by_bin(g_fav[i]);
        if (!e || e->kind != 'v') continue;
        if (!first) sb_puts(s, ",");
        first = 0;
        sb_puts(s, "{\"base\":"); sb_jstr(s, e->base);
        sb_puts(s, ",\"franchise\":"); sb_jstr(s, e->franchise);
        sb_puts(s, ",\"builds\":["); sb_puts(s, e->json); sb_puts(s, "]}");
    }
    pthread_mutex_unlock(&g_mx);
    sb_puts(s, "]}");
}

static void api_load(sbuf *s, int slot, int bin) {
    if (slot < 0 || slot > 6) { err_json(s, "Invalid pad slot."); return; }
    const entry_info *e = entry_by_bin(bin);
    uint32_t fid = e ? bin_figure_id(bin) : 0;
    if (!fid) { err_json(s, "Unknown tag id."); return; }
    pthread_mutex_lock(&g_cmd);
    int used[7]; uint32_t id[7];
    int v = SLOTS[slot].vtp, r;
    if (read_pad(used, id)) { pthread_mutex_unlock(&g_cmd); err_json(s, "LEGO Dimensions isn't running with the Virtual Toy Pad."); return; }
    if (used[v]) {
        mailbox(LD_MB_REMOVE, (uint32_t)v, 0);
        usleep(100 * 1000);   /* let the game see the removal as its own event */
    }
    r = mailbox(LD_MB_PLACE_ID, (uint32_t)v, fid);
    pthread_mutex_unlock(&g_cmd);
    char nm[32], msg[160];
    if (r == 0) {
        snprintf(msg, sizeof msg, "LOAD sent: %s -> %s", figure_name(fid, nm, sizeof nm), SLOTS[slot].label);
        set_status("%s", msg);
        ok_json(s, msg);
    } else {
        snprintf(msg, sizeof msg, r == -3 ? "The game didn't respond (paused or loading?). Try again."
                                          : "Could not place %s.", figure_name(fid, nm, sizeof nm));
        set_status("%s", msg);
        err_json(s, msg);
    }
}

static void api_move(sbuf *s, int src, int dst) {
    if (src < 0 || src > 6 || dst < 0 || dst > 6) { err_json(s, "Invalid pad slot."); return; }
    pthread_mutex_lock(&g_cmd);
    int used[7]; uint32_t id[7];
    int a = SLOTS[src].vtp, b = SLOTS[dst].vtp;
    if (read_pad(used, id) || !used[a]) {
        pthread_mutex_unlock(&g_cmd);
        err_json(s, "There is nothing tracked on that pad to move.");
        return;
    }
    if (a == b) { pthread_mutex_unlock(&g_cmd); ok_json(s, "MOVE sent"); return; }
    uint32_t fa = id[a], fb = used[b] ? id[b] : 0;
    /* lift first, let the game see the pickup, then place (LegoToypad/emulators use 500 ms) */
    int r = mailbox(LD_MB_REMOVE, (uint32_t)a, 0);
    if (fb) r |= mailbox(LD_MB_REMOVE, (uint32_t)b, 0);
    usleep(500 * 1000);
    r |= mailbox(LD_MB_PLACE_ID, (uint32_t)b, fa);
    if (fb) r |= mailbox(LD_MB_PLACE_ID, (uint32_t)a, fb);
    pthread_mutex_unlock(&g_cmd);
    char nm[32], msg[160];
    snprintf(msg, sizeof msg, "%s sent: %s -> %s", fb ? "SWAP" : "MOVE", figure_name(fa, nm, sizeof nm), SLOTS[dst].label);
    set_status("%s", r ? "Move failed (game paused or loading?)" : msg);
    if (r) err_json(s, "Move failed (game paused or loading?)");
    else ok_json(s, msg);
}

static void api_clear(sbuf *s, int slot) {
    if (slot < 0 || slot > 6) { err_json(s, "Invalid pad slot."); return; }
    pthread_mutex_lock(&g_cmd);
    int r = mailbox(LD_MB_REMOVE, (uint32_t)SLOTS[slot].vtp, 0);
    pthread_mutex_unlock(&g_cmd);
    char msg[96];
    snprintf(msg, sizeof msg, "CLEAR sent: %s", SLOTS[slot].label);
    if (r == 0) { set_status("%s", msg); ok_json(s, msg); }
    else err_json(s, r == -1 ? "That pad is already empty." : "The game didn't respond. Try again.");
}

static void api_clearall(sbuf *s) {
    pthread_mutex_lock(&g_cmd);
    int r = mailbox(LD_MB_CLEAR_ALL, 0, 0);
    pthread_mutex_unlock(&g_cmd);
    if (r == 0) { set_status("Clear all pad sent"); ok_json(s, "Clear all pad sent"); }
    else err_json(s, "The game didn't respond. Try again.");
}

static void api_favorite(sbuf *s, int bin) {
    if (!entry_by_bin(bin)) { err_json(s, "Nothing on this pad to favorite."); return; }
    pthread_mutex_lock(&g_mx);
    int at = -1;
    for (int i = 0; i < g_nfav; i++) if (g_fav[i] == bin) at = i;
    if (at >= 0) { memmove(g_fav + at, g_fav + at + 1, (size_t)(g_nfav - at - 1) * sizeof *g_fav); g_nfav--; }
    else if (g_nfav < 256) g_fav[g_nfav++] = bin;
    save_prefs();
    pthread_mutex_unlock(&g_mx);
    sb_printf(s, "{\"ok\":true,\"favorited\":%s}", at >= 0 ? "false" : "true");
}

/* ------------------------------------------------------------ HTTP */

static int send_all(int fd, const void *d, size_t n) {
    const char *p = d;
    while (n) {
        ssize_t k = send(fd, p, n, 0);
        if (k <= 0) { if (k < 0 && errno == EINTR) continue; return -1; }
        p += k; n -= (size_t)k;
    }
    return 0;
}

static void respond(int fd, int code, const char *type, const void *body, size_t len, int cache) {
    char h[384];
    const char *msg = code == 200 ? "OK" : code == 404 ? "Not Found" : code == 400 ? "Bad Request" : "Error";
    int n = snprintf(h, sizeof h,
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n%s"
                     "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
                     code, msg, type, len, cache ? "Cache-Control: public, max-age=31536000, immutable\r\n"
                                                 : "Cache-Control: no-store\r\n");
    send_all(fd, h, (size_t)n);
    if (len) send_all(fd, body, len);
}

static const char *mime_for(const char *path, const unsigned char *d, size_t n) {
    size_t l = strlen(path);
    if (l > 5 && !strcmp(path + l - 5, ".html")) return "text/html; charset=utf-8";
    if (l > 4 && !strcmp(path + l - 4, ".css")) return "text/css; charset=utf-8";
    if (l > 3 && !strcmp(path + l - 3, ".js")) return "application/javascript; charset=utf-8";
    if (l > 5 && !strcmp(path + l - 5, ".json")) return "application/json; charset=utf-8";
    if (l > 6 && !strcmp(path + l - 6, ".woff2")) return "font/woff2";
    if (n >= 8 && !memcmp(d, "\x89PNG\r\n\x1a\n", 8)) return "image/png";
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return "image/jpeg";
    if (n >= 4 && !memcmp(d, "wOF2", 4)) return "font/woff2";
    if (n >= 4 && !memcmp(d, "OTTO", 4)) return "font/otf";
    if (n >= 4 && d[0] == 0 && d[1] == 1 && d[2] == 0 && d[3] == 0) return "font/ttf";
    if (n >= 4 && !memcmp(d, "\0\0\1\0", 4)) return "image/x-icon";
    return "application/octet-stream";
}

static void serve_file(int fd, const char *rel, int cache) {
    char path[160];
    snprintf(path, sizeof path, WEB_DIR "/%s", rel);
    size_t n = 0;
    char *d = read_file(path, &n);
    if (!d) {
        const char *nf = strcmp(rel, "index.html") ? "Not Found"
            : "<h2>LegoToypad web files missing</h2><p>Copy the bundle's web folder to /data/ldmod/web on the PS5.</p>";
        respond(fd, 404, strcmp(rel, "index.html") ? "text/plain" : "text/html; charset=utf-8", nf, strlen(nf), 0);
        return;
    }
    respond(fd, 200, mime_for(rel, (unsigned char *)d, n), d, n, cache);
    free(d);
}

static void handle(int fd) {
    char req[8192];
    size_t got = 0;
    char *hdr_end = 0;
    while (got < sizeof req - 1) {
        ssize_t k = recv(fd, req + got, sizeof req - 1 - got, 0);
        if (k <= 0) return;
        got += (size_t)k;
        req[got] = 0;
        if ((hdr_end = strstr(req, "\r\n\r\n"))) break;
    }
    if (!hdr_end) return;
    char method[8] = "", target[512] = "";
    if (sscanf(req, "%7s %511s", method, target) != 2) return;
    size_t clen = 0;
    for (char *p = req; p < hdr_end; p++)
        if ((*p == 'C' || *p == 'c') && !strncasecmp(p, "Content-Length:", 15)) { clen = strtoul(p + 15, 0, 10); break; }
    char *body = hdr_end + 4;
    size_t have = got - (size_t)(body - req);
    while (have < clen && got < sizeof req - 1) {
        ssize_t k = recv(fd, req + got, sizeof req - 1 - got, 0);
        if (k <= 0) break;
        got += (size_t)k; have += (size_t)k;
        req[got] = 0;
    }
    char *q = strchr(target, '?');
    if (q) *q = 0;

    sbuf s = {0};
    int is_json = 0;
    if (!strcmp(method, "GET")) {
        if (!strcmp(target, "/") || !strcmp(target, "/index.html")) serve_file(fd, "index.html", 0);
        else if (!strcmp(target, "/app.js")) serve_file(fd, "app.js", 0);
        else if (!strcmp(target, "/style.css")) serve_file(fd, "style.css", 0);
        else if (!strcmp(target, "/InterVariable.woff2")) serve_file(fd, "InterVariable.woff2", 1);
        else if (!strcmp(target, "/api/catalog")) serve_file(fd, "catalog.json", 0);
        else if (!strncmp(target, "/img/", 5)) {
            char *end;
            long id = strtol(target + 5, &end, 10);
            char rel[32];
            snprintf(rel, sizeof rel, "img/%ld", id);
            if (*end || id == 0) respond(fd, 404, "text/plain", "Not Found", 9, 0);
            else serve_file(fd, rel, 1);
        } else if (!strcmp(target, "/favicon.ico")) {
            char rel[32];
            snprintf(rel, sizeof rel, "img/%d", g_appicon);
            serve_file(fd, rel, 1);
        } else if (!strcmp(target, "/api/state")) { api_state(&s); is_json = 1; }
        else if (!strcmp(target, "/api/leds")) { api_leds(&s); is_json = 1; }
        else if (!strcmp(target, "/api/favorites")) { api_favorites(&s); is_json = 1; }
        else if (!strcmp(target, "/api/sort")) { api_sort(&s); is_json = 1; }
        else if (!strcmp(target, "/api/selection")) { api_selection(&s); is_json = 1; }
        else respond(fd, 404, "text/plain", "Not Found", 9, 0);
    } else if (!strcmp(method, "POST")) {
        is_json = 1;
        if (!strcmp(target, "/api/load")) api_load(&s, json_int(body, "slot", -1), json_int(body, "bin", 0));
        else if (!strcmp(target, "/api/move")) api_move(&s, json_int(body, "src", -1), json_int(body, "dest", -1));
        else if (!strcmp(target, "/api/clear")) api_clear(&s, json_int(body, "slot", -1));
        else if (!strcmp(target, "/api/clearall")) api_clearall(&s);
        else if (!strcmp(target, "/api/favorite")) api_favorite(&s, json_int(body, "bin", 0));
        else if (!strcmp(target, "/api/sort")) {
            int v = json_int(body, "sort", -1);
            if (v < 0 || v > 6) err_json(&s, "Invalid sort.");
            else {
                pthread_mutex_lock(&g_mx); g_sort = v; save_prefs(); pthread_mutex_unlock(&g_mx);
                api_sort(&s);
            }
        } else if (!strcmp(target, "/api/selection")) {
            int kind = json_int(body, "kind", 0), value = json_int(body, "value", 0);
            pthread_mutex_lock(&g_mx);
            if (kind == 1) { g_sel.world = value; g_sel.virt[0] = 0; }
            else if (kind == 2) { g_sel.world = -1; snprintf(g_sel.virt, sizeof g_sel.virt, "%s", value ? "custom" : "favorites"); }
            else if (kind == 3) g_sel.bin = value;
            else if (kind == 4) g_sel.ability = value;
            pthread_mutex_unlock(&g_mx);
            api_selection(&s);
        } else if (!strcmp(target, "/api/ledmirror")) {
            pthread_mutex_lock(&g_mx);
            g_led_mirror = json_int(body, "enabled", 1) != 0;
            save_prefs();
            sb_printf(&s, "{\"ok\":true,\"enabled\":%s}", g_led_mirror ? "true" : "false");
            pthread_mutex_unlock(&g_mx);
        } else { is_json = 0; respond(fd, 404, "text/plain", "Not Found", 9, 0); }
    } else respond(fd, 400, "text/plain", "Bad Request", 11, 0);
    if (is_json) respond(fd, 200, "application/json; charset=utf-8", s.p ? s.p : "{}", s.p ? s.n : 2, 0);
    free(s.p);
}

static void *client_thread(void *arg) {
    int fd = (int)(intptr_t)arg;
    handle(fd);
    close(fd);
    return 0;
}

static void *server_thread(void *arg) {
    (void)arg;
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(WEB_PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (srv < 0 || bind(srv, (struct sockaddr *)&a, sizeof a) || listen(srv, 16)) {
        web_log("web remote: cannot listen on port %d (errno %d)", WEB_PORT, errno);
        return 0;
    }
    web_log("web remote: listening on port %d", WEB_PORT);
    for (;;) {
        int c = accept(srv, 0, 0);
        if (c < 0) { if (errno == EINTR) continue; usleep(100 * 1000); continue; }
        struct timeval tv = {5, 0};
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        pthread_t t;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &at, client_thread, (void *)(intptr_t)c)) { handle(c); close(c); }
        pthread_attr_destroy(&at);
    }
    return 0;
}

int web_start(void) {
    struct stat st;
    if (stat(WEB_DIR "/catalog.json", &st)) {
        web_log("web remote: no bundle at " WEB_DIR " (phone remote disabled)");
        return -1;
    }
    load_index();
    load_prefs();
    pthread_t t;
    return pthread_create(&t, 0, server_thread, 0);
}
