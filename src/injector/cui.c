/* Virtual Toy Pad console overlay: LegoToypad's picker on the TV.
 *
 * harrysof's LegoToypad (MIT) is a Windows overlay: a 900x610 window with the seven toypad
 * pads, a Load / Move / Clear action bar, the world grid, the portrait roster, the vehicle
 * build picker, a hold-to-peek HUD and live toypad LEDs. This file is that picker's logic and
 * layout, re-implemented for the PS5: the screens, navigation and geometry follow LegoToypad's
 * main.cpp (Confirm, Back, NavigatePadGrid, MoveFranchiseSelection, MoveRosterSelection,
 * kPadCells, ComputePeekCells, DrawLedRegionGlows/Tint ...), and the art comes from the "ui"
 * pack built from LegoToypad's own assets (tools/build_ui_pack.py).
 *
 * The overlay itself is drawn by the renderer this payload loads into SceShellUI
 * (src/overlay); this thread sends it widgets, textures and per-frame deltas over a local
 * TCP socket (ovl_proto.h). Controller input comes from the game hook (ld_header.ext), which
 * also hides it from the game while the picker is open. Figure commands go through the same
 * mailbox and LegoToypad timing as the phone remote (vtp_api.h).
 *
 * Copyright (C) 2026 SpiderNic96. SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "cui.h"
#include "vtp_api.h"
#include "../overlay/common/ovl_proto.h"
#ifndef CUI_HOST_TEST
#include "../overlay/controller/ovl_controller.h"
#endif

#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_ASSERT(x) ((void)0)
#define STB_IMAGE_IMPLEMENTATION
#include "third_party/stb_image.h"

void web_log(const char *fmt, ...);
#define LOGC(...) web_log("overlay: " __VA_ARGS__)

#ifndef UI_DIR
#define UI_DIR "/data/ldmod/web/ui"
#endif

static cui_config C = {1, 0x100, 2, 1, 0, 0x2 | 0x4, 0, 92, 0, 0, 0};

void cui_configure(const cui_config *c) { C = *c; }

/* LegoToypad's 900x610 window, centred on the 1920x1080 canvas. K is canvas px per window unit:
 * 1.5 at "UI size 100%"; FONT_MUL scales every label (UI size x Text size). */
static float K = 1.5f, OX = 285.0f, OY = 82.0f, FONT_MUL = 1.0f;
static float FS(float font) { return font * FONT_MUL; }

static uint64_t ms_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* ------------------------------------------------------------------ pad buttons */

enum {
    B_L3 = 0x2, B_R3 = 0x4, B_OPTIONS = 0x8, B_UP = 0x10, B_RIGHT = 0x20, B_DOWN = 0x40, B_LEFT = 0x80,
    B_L2 = 0x100, B_R2 = 0x200, B_L1 = 0x400, B_R1 = 0x800, B_TRIANGLE = 0x1000, B_CIRCLE = 0x2000,
    B_CROSS = 0x4000, B_SQUARE = 0x8000,
};

/* ------------------------------------------------------------------ catalog (ui/catalog.tsv) */

typedef struct { char name[64]; int year1, year2, starter; } world_t;
typedef struct { char kind; int world, group, build, bin; char name[64]; uint8_t rgb[3]; } entry_t;
typedef struct { int world; char base[64]; int first, count; } group_t;

#define MAXW 48
#define MAXE 480
#define MAXG 320
static world_t Wd[MAXW];
static int NW;
static entry_t E[MAXE];
static int NE;
static group_t G[MAXG];
static int NG;

static char *slurp(const char *path, size_t *len) {
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

static void hexrgb(const char *h, uint8_t rgb[3]) {
    unsigned v = (unsigned)strtoul(h + (h[0] == '#'), 0, 16);
    rgb[0] = (uint8_t)(v >> 16); rgb[1] = (uint8_t)(v >> 8); rgb[2] = (uint8_t)v;
}

static int split(char *ln, char **c, int max) {
    int n = 0;
    c[n++] = ln;
    for (char *p = ln; *p && n < max; p++) if (*p == '\t') { *p = 0; c[n++] = p + 1; }
    for (char *p = c[n - 1]; *p; p++) if (*p == '\r' || *p == '\n') { *p = 0; break; }
    return n;
}

static int load_catalog(void) {
    char *d = slurp(UI_DIR "/catalog.tsv", 0);
    if (!d) return -1;
    NW = NE = NG = 0;
    char *save = 0;
    for (char *ln = strtok_r(d, "\n", &save); ln; ln = strtok_r(0, "\n", &save)) {
        char *c[8];
        int n = split(ln, c, 8);
        if (c[0][0] == 'W' && n >= 6 && NW < MAXW) {
            world_t *w = &Wd[NW++];
            snprintf(w->name, sizeof w->name, "%s", c[2]);
            w->year1 = atoi(c[3]); w->year2 = atoi(c[4]); w->starter = atoi(c[5]);
        } else if (c[0][0] == 'C' && n >= 5 && NE < MAXE) {
            entry_t *e = &E[NE++];
            e->kind = 'c'; e->world = atoi(c[1]); e->bin = atoi(c[2]); e->group = -1; e->build = 0;
            snprintf(e->name, sizeof e->name, "%s", c[3]);
            hexrgb(c[4], e->rgb);
        } else if (c[0][0] == 'G' && n >= 4 && NG < MAXG) {
            group_t *g = &G[NG++];
            g->world = atoi(c[1]);
            snprintf(g->base, sizeof g->base, "%s", c[3]);
            g->first = NE; g->count = 0;
        } else if (c[0][0] == 'V' && n >= 7 && NE < MAXE && NG > 0) {
            entry_t *e = &E[NE++];
            e->kind = 'v'; e->world = atoi(c[1]); e->group = NG - 1; e->build = atoi(c[3]); e->bin = atoi(c[4]);
            snprintf(e->name, sizeof e->name, "%s", c[5]);
            hexrgb(c[6], e->rgb);
            G[NG - 1].count++;
        }
    }
    free(d);
    LOGC("catalog: %d worlds, %d entries, %d vehicle groups", NW, NE, NG);
    return NW > 0 ? 0 : -1;
}

static int entry_by_bin(int bin) {
    for (int i = 0; i < NE; i++) if (E[i].bin == bin) return i;
    return -1;
}

static int find_world(const char *name) {
    for (int i = 0; i < NW; i++) if (!strcmp(Wd[i].name, name)) return i;
    return -1;
}

static int find_char(int world, const char *name) {
    for (int i = 0; i < NE; i++) if (E[i].kind == 'c' && E[i].world == world && !strcmp(E[i].name, name)) return i;
    return -1;
}

/* ------------------------------------------------------------------ renderer link */

static int g_fd = -1;
static uint32_t g_caps;
static uint64_t g_last_ping, g_next_connect;
static pthread_mutex_t g_send_mx = PTHREAD_MUTEX_INITIALIZER;

static void link_close(const char *why) {
    if (g_fd >= 0) { close(g_fd); LOGC("renderer link closed (%s)", why); }
    g_fd = -1;
}

static int send_all(const void *d, size_t n) {
    const uint8_t *p = d;
    while (n) {
        ssize_t k = send(g_fd, p, n, 0);
        if (k <= 0) { if (k < 0 && errno == EINTR) continue; return -1; }
        p += k; n -= (size_t)k;
    }
    return 0;
}

static int send_msg2(uint16_t type, const void *a, uint32_t na, const void *b, uint32_t nb) {
    if (g_fd < 0) return -1;
    ovl_msg h = {OVL_MAGIC, type, OVL_VERSION, na + nb};
    pthread_mutex_lock(&g_send_mx);
    int r = send_all(&h, sizeof h) || (na && send_all(a, na)) || (nb && send_all(b, nb));
    pthread_mutex_unlock(&g_send_mx);
    if (r) link_close("send failed");
    return r ? -1 : 0;
}

static int recv_full(int fd, void *b, size_t n, int timeout_ms) {
    uint8_t *p = b;
    uint64_t t0 = ms_now();
    while (n) {
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(fd, &rs);
        struct timeval tv = {0, 50 * 1000};
        if (select(fd + 1, &rs, 0, 0, &tv) <= 0) {
            if ((int)(ms_now() - t0) > timeout_ms) return -1;
            continue;
        }
        ssize_t k = recv(fd, p, n, 0);
        if (k <= 0) return -1;
        p += k; n -= (size_t)k;
    }
    return 0;
}

static void reset_sent_state(void);

static int link_open(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(OVL_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&a, sizeof a)) { close(fd); return -1; }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    ovl_msg h;
    ovl_hello hello;
    if (recv_full(fd, &h, sizeof h, 3000) || h.magic != OVL_MAGIC || h.type != OVL_HELLO || h.len != sizeof hello ||
        recv_full(fd, &hello, sizeof hello, 3000)) {
        close(fd);
        LOGC("renderer did not say hello");
        return -1;
    }
    g_fd = fd;
    g_caps = hello.caps;
    LOGC("renderer link up: pid %d caps 0x%x scene %.0fx%.0f (%u widgets, %u textures resident)", hello.pid,
         hello.caps, (double)hello.scene_w, (double)hello.scene_h, hello.widgets, hello.textures);
    ovl_config cfg = {(uint32_t)C.tex_mode, {0, 0, 0}};
    send_msg2(OVL_CONFIG, &cfg, sizeof cfg, 0, 0);
    reset_sent_state();
    return 0;
}

static void link_poll(void) {
    while (g_fd >= 0) {
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(g_fd, &rs);
        struct timeval tv = {0, 0};
        if (select(g_fd + 1, &rs, 0, 0, &tv) <= 0) return;
        ovl_msg h;
        if (recv_full(g_fd, &h, sizeof h, 1000) || h.magic != OVL_MAGIC || h.len > 4096) { link_close("bad read"); return; }
        char body[4097];
        if (h.len && recv_full(g_fd, body, h.len, 1000)) { link_close("bad read"); return; }
        body[h.len] = 0;
        if (h.type == OVL_STATUS) LOGC("renderer: %s", body);
    }
}

/* ------------------------------------------------------------------ textures */

typedef struct { char name[24]; uint32_t id; int sent, dynamic; uint64_t used; int bytes; } texent;
#define MAXT 900
static texent T[MAXT];
static int NT;
static int g_tex_budget;   /* uploads allowed this tick */

static uint32_t name_hash(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) { h ^= (uint8_t)*s; h *= 16777619u; }
    return h ? h : 1;
}

static texent *tex_find(const char *name) {
    for (int i = 0; i < NT; i++) if (!strcmp(T[i].name, name)) return &T[i];
    if (NT >= MAXT) return 0;
    texent *t = &T[NT++];
    memset(t, 0, sizeof *t);
    snprintf(t->name, sizeof t->name, "%s", name);
    t->id = name_hash(name);
    t->dynamic = name[0] == 'p' && name[1] >= '0' && name[1] <= '9';      /* portraits */
    t->dynamic |= name[0] == 'w' && (name[1] == 'l' || (name[1] >= '0' && name[1] <= '9'));  /* worlds */
    t->dynamic |= !strncmp(name, "wall", 4) || !strncmp(name, "pk", 2);  /* wallpapers, pad skins */
    return t;
}

typedef struct wstate wstate;
static void tex_rebind(uint32_t id);

static int tex_upload(texent *t) {
    char path[160];
    snprintf(path, sizeof path, UI_DIR "/%s.png", t->name);
    size_t n = 0;
    uint8_t *png = (uint8_t *)slurp(path, &n);
    if (!png) {   /* wallpapers ship as JPEG */
        snprintf(path, sizeof path, UI_DIR "/%s.jpg", t->name);
        png = (uint8_t *)slurp(path, &n);
    }
    if (!png) { static int warned; if (warned++ < 8) LOGC("missing %s", path); t->sent = -1; return -1; }
    int r;
    if ((g_caps & OVL_CAP_PIXELS) && C.tex_mode != 2) {
        int w = 0, h = 0, comp = 0;
        uint8_t *px = stbi_load_from_memory(png, (int)n, &w, &h, &comp, 4);
        if (!px) { free(png); t->sent = -1; return -1; }
        uint8_t hdr[8];
        uint16_t w16 = (uint16_t)w, h16 = (uint16_t)h;
        memcpy(hdr, &t->id, 4); memcpy(hdr + 4, &w16, 2); memcpy(hdr + 6, &h16, 2);
        r = send_msg2(OVL_TEX, hdr, 8, px, (uint32_t)(w * h * 4));
        t->bytes = w * h * 4;
        stbi_image_free(px);
    } else {
        uint8_t hdr[8];
        int w = 0, h = 0, comp = 0;
        stbi_info_from_memory(png, (int)n, &w, &h, &comp);
        uint16_t w16 = (uint16_t)w, h16 = (uint16_t)h;
        memcpy(hdr, &t->id, 4); memcpy(hdr + 4, &w16, 2); memcpy(hdr + 6, &h16, 2);
        r = send_msg2(OVL_TEXPNG, hdr, 8, png, (uint32_t)n);
        t->bytes = (int)(w * h * 4);
    }
    free(png);
    if (!r) { t->sent = 1; tex_rebind(t->id); }
    return r;
}

/* texture id for a widget this frame; uploads it if needed (bounded per tick) */
static uint32_t tex(const char *name) {
    texent *t = tex_find(name);
    if (!t) return 0;
    t->used = ms_now();
    if (!t->sent && g_fd >= 0 && g_tex_budget > 0) { g_tex_budget--; tex_upload(t); }
    return t->sent == 1 ? t->id : t->id;   /* the renderer waits for late textures */
}

#define DYN_CAP 48                  /* portraits, world art, wallpapers kept resident in ShellUI */
#define DYN_BYTES (9 << 20)
static void tex_evict(void) {
    int live = 0;
    long bytes = 0;
    for (int i = 0; i < NT; i++) if (T[i].dynamic && T[i].sent == 1) { live++; bytes += T[i].bytes; }
    uint64_t now = ms_now();
    while (live > DYN_CAP || bytes > DYN_BYTES) {
        int lru = -1;
        for (int i = 0; i < NT; i++)
            if (T[i].dynamic && T[i].sent == 1 && now - T[i].used > 500 && (lru < 0 || T[i].used < T[lru].used)) lru = i;
        if (lru < 0) break;
        send_msg2(OVL_TEXFREE, &T[lru].id, 4, 0, 0);
        T[lru].sent = 0;
        tex_rebind(T[lru].id);
        live--;
        bytes -= T[lru].bytes;
    }
}

/* texture sizes from ui/sizes.tsv: logos, badges and button glyphs keep their aspect ratio */
typedef struct { char name[24]; uint16_t w, h; } tsize;
static tsize *g_sizes;
static int g_nsizes;

static void load_sizes(void) {
    char *d = slurp(UI_DIR "/sizes.tsv", 0);
    if (!d) return;
    int cap = 1024;
    free(g_sizes);
    g_sizes = calloc((size_t)cap, sizeof *g_sizes);
    g_nsizes = 0;
    char *save = 0;
    for (char *ln = strtok_r(d, "\n", &save); ln && g_sizes && g_nsizes < cap; ln = strtok_r(0, "\n", &save)) {
        char *c[4];
        if (split(ln, c, 4) < 3) continue;
        tsize *t = &g_sizes[g_nsizes++];
        snprintf(t->name, sizeof t->name, "%s", c[0]);
        t->w = (uint16_t)atoi(c[1]);
        t->h = (uint16_t)atoi(c[2]);
    }
    free(d);
}

static float tex_aspect(const char *name, float fallback) {
    for (int i = 0; i < g_nsizes; i++)
        if (!strcmp(g_sizes[i].name, name) && g_sizes[i].h) return (float)g_sizes[i].w / g_sizes[i].h;
    return fallback;
}

/* ------------------------------------------------------------------ widgets */

enum {
    W_BG, W_WORDMARK, W_BYMARK, W_PORTMARK, W_PLATE,
    W_HALO0, W_GLOW0 = W_HALO0 + 3, W_TPREG0 = W_GLOW0 + 7, W_ART0 = W_TPREG0 + 3, W_OCCT0 = W_ART0 + 7, W_LEDT0 = W_OCCT0 + 7,
    W_LEDP0 = W_LEDT0 + 7, W_BORD0 = W_LEDP0 + 7, W_OCC0 = W_BORD0 + 7, W_ACTHALO = W_OCC0 + 7,
    W_ACT0, W_OCCNAME = W_ACT0 + 3,
    W_SORT, W_TGLOW, W_TILE0, W_TBORD0 = W_TILE0 + 16, W_SCROLL = W_TBORD0 + 16,
    W_RPANEL, W_RLOGO, W_SEP, W_RPORT0, W_RFOCUS = W_RPORT0 + 15, W_RSTAR0, W_RNAME0 = W_RSTAR0 + 15,
    W_PPANEL = W_RNAME0 + 15, W_PPORT0, W_PFOCUS = W_PPORT0 + 8, W_PSTAR0, W_PNAME0 = W_PSTAR0 + 8,
    W_SPANEL = W_PNAME0 + 8, W_SROW, W_SRULE0, W_SLAB0 = W_SRULE0 + 6, W_SVAL0 = W_SLAB0 + 12,
    W_SETICON = W_SVAL0 + 12, W_SETTEXT,
    W_QBOX, W_QTEXT, W_QCOUNT, W_KEY0, W_KEYLAB0 = W_KEY0 + 40,
    W_TITLE = W_KEYLAB0 + 40,
    W_TL, W_TM, W_TR, W_TBL, W_TBM, W_TBR, W_TTEXT,
    W_HINTBG, W_HICON0, W_HTEXT0 = W_HICON0 + 7,
    W_COUNT = W_HTEXT0 + 7
};

struct wstate {
    float x, y, w, h;
    uint32_t vis;
    float alpha;
    uint8_t c[4];
    uint32_t tex;
    char text[120];
    uint16_t font;
    uint8_t align, lines;
};

static uint8_t g_types[W_COUNT];
static wstate g_cur[W_COUNT], g_sent[W_COUNT];
static int g_sent_valid;   /* DEFINE + full state sent on this link */

/* a texture id was (re)uploaded or freed: widgets showing it must be pointed at it again */
static void tex_rebind(uint32_t id) {
    for (int i = 0; i < W_COUNT; i++) if (g_sent[i].tex == id) g_sent[i].tex = 0xFFFFFFFFu;
}

static void define_types(void) {
    for (int i = 0; i < W_COUNT; i++) g_types[i] = OVL_W_IMAGE;
    for (int i = 0; i < 7; i++) g_types[W_LEDP0 + i] = OVL_W_PANEL;
    g_types[W_OCCNAME] = g_types[W_TITLE] = g_types[W_TTEXT] = OVL_W_LABEL;
    for (int i = 0; i < 15; i++) g_types[W_RNAME0 + i] = OVL_W_LABEL;
    for (int i = 0; i < 8; i++) g_types[W_PNAME0 + i] = OVL_W_LABEL;
    for (int i = 0; i < 7; i++) g_types[W_HTEXT0 + i] = OVL_W_LABEL;
    for (int i = 0; i < 12; i++) g_types[W_SLAB0 + i] = g_types[W_SVAL0 + i] = OVL_W_LABEL;
    g_types[W_QTEXT] = g_types[W_QCOUNT] = OVL_W_LABEL;
    for (int i = 0; i < 40; i++) g_types[W_KEYLAB0 + i] = OVL_W_LABEL;
}

static void reset_sent_state(void) {
    g_sent_valid = 0;
    for (int i = 0; i < NT; i++) if (T[i].sent == 1) T[i].sent = 0;
}

static void frame_begin(void) {
    for (int i = 0; i < W_COUNT; i++) { g_cur[i] = g_sent[i]; g_cur[i].vis = 0; }
}

static void img(int id, const char *texname, float x, float y, float w, float h, float alpha, const uint8_t *tint) {
    wstate *s = &g_cur[id];
    s->x = x; s->y = y; s->w = w; s->h = h;
    s->vis = 1;
    s->alpha = alpha;
    static const uint8_t white[4] = {255, 255, 255, 255};
    memcpy(s->c, tint ? tint : white, 4);
    s->tex = tex(texname);
}

/* image contained in a box (canvas px) keeping the texture's aspect; align 0 start, 1 centre, 2 end */
static float img_fit(int id, const char *name, float bx, float by, float bw, float bh, int ha, int va, float alpha,
                     const uint8_t *tint) {
    float a = tex_aspect(name, bw / bh), w = bw, h = bw / a;
    if (h > bh) { h = bh; w = bh * a; }
    float x = ha == 0 ? bx : ha == 1 ? bx + (bw - w) / 2 : bx + bw - w;
    float y = va == 0 ? by : va == 1 ? by + (bh - h) / 2 : by + bh - h;
    img(id, name, x, y, w, h, alpha, tint);
    return w;
}

static void panel(int id, float x, float y, float w, float h, const uint8_t rgba[4]) {
    wstate *s = &g_cur[id];
    s->x = x; s->y = y; s->w = w; s->h = h;
    s->vis = 1; s->alpha = 1.0f;
    memcpy(s->c, rgba, 4);
}

static void label(int id, const char *text, float x, float y, float w, int font, int align, int lines,
                  const uint8_t rgba[4], float alpha) {
    wstate *s = &g_cur[id];
    font = (int)(FS((float)font) + 0.5f);
    s->x = x; s->y = y; s->w = w; s->h = (float)font;
    s->vis = 1; s->alpha = alpha;
    memcpy(s->c, rgba, 4);
    s->font = (uint16_t)font; s->align = (uint8_t)align; s->lines = (uint8_t)lines;
    if (!(g_caps & OVL_CAP_TRIM)) {   /* no trimming in the renderer: shorten here */
        int max = w > 0 ? (int)(w / (font * 0.52f)) * (lines ? lines : 1) : 200;
        int n = (int)strlen(text);
        if (n > max && max > 3) {
            snprintf(s->text, sizeof s->text, "%.*s...", max - 3, text);
            return;
        }
    }
    snprintf(s->text, sizeof s->text, "%s", text);
}

static uint8_t g_buf[256 * 1024];

static void frame_send(void) {
    if (g_fd < 0) return;
    if (!g_sent_valid) {
        uint8_t def[2 + W_COUNT];
        uint16_t n = W_COUNT;
        memcpy(def, &n, 2);
        memcpy(def + 2, g_types, W_COUNT);
        if (send_msg2(OVL_DEFINE, def, sizeof def, 0, 0)) return;
        memset(g_sent, 0, sizeof g_sent);
        for (int i = 0; i < W_COUNT; i++) { g_sent[i].alpha = -1; g_sent[i].tex = 0xFFFFFFFFu; g_sent[i].font = 0xFFFF; }
    }
    size_t off = 2;
    uint16_t count = 0;
    for (int i = 0; i < W_COUNT; i++) {
        wstate *a = &g_cur[i], *b = &g_sent[i];
        uint16_t mask = 0;
        if (!g_sent_valid) mask |= OVL_F_VIS;
        if (a->vis != b->vis) mask |= OVL_F_VIS;
        if (a->vis) {
            if (a->x != b->x || a->y != b->y || a->w != b->w || a->h != b->h) mask |= OVL_F_GEOM;
            if (a->alpha != b->alpha) mask |= OVL_F_ALPHA;
            if (memcmp(a->c, b->c, 4)) mask |= OVL_F_COLOR;
            if (g_types[i] == OVL_W_IMAGE && a->tex != b->tex) mask |= OVL_F_TEX;
            if (g_types[i] == OVL_W_LABEL && (strcmp(a->text, b->text) || a->font != b->font || a->align != b->align ||
                                             a->lines != b->lines)) mask |= OVL_F_TEXT;
        }
        if (!mask) continue;
        if (off + 220 > sizeof g_buf) break;
        uint16_t id = (uint16_t)i;
        memcpy(g_buf + off, &id, 2); memcpy(g_buf + off + 2, &mask, 2); off += 4;
        if (mask & OVL_F_GEOM) { memcpy(g_buf + off, &a->x, 4); memcpy(g_buf + off + 4, &a->y, 4);
                                 memcpy(g_buf + off + 8, &a->w, 4); memcpy(g_buf + off + 12, &a->h, 4); off += 16; }
        if (mask & OVL_F_VIS) { memcpy(g_buf + off, &a->vis, 4); off += 4; }
        if (mask & OVL_F_ALPHA) { memcpy(g_buf + off, &a->alpha, 4); off += 4; }
        if (mask & OVL_F_COLOR) { memcpy(g_buf + off, a->c, 4); off += 4; }
        if (mask & OVL_F_TEX) { memcpy(g_buf + off, &a->tex, 4); off += 4; }
        if (mask & OVL_F_TEXT) {
            uint16_t len = (uint16_t)strlen(a->text);
            memcpy(g_buf + off, &a->font, 2); g_buf[off + 2] = a->align; g_buf[off + 3] = a->lines;
            memcpy(g_buf + off + 4, &len, 2); off += 6;
            memcpy(g_buf + off, a->text, len); off += len;
            while (len & 3) { g_buf[off++] = 0; len++; }
        }
        *b = *a;
        count++;
    }
    if (!count && g_sent_valid) return;
    memcpy(g_buf, &count, 2);
    if (send_msg2(OVL_FRAME, g_buf, (uint32_t)off, 0, 0) == 0) g_sent_valid = 1;
}

/* ------------------------------------------------------------------ LegoToypad geometry */

static float X(float x) { return OX + x * K; }
static float Y(float y) { return OY + y * K; }

typedef struct { float l, t, r, b; } rectf;
static const rectf PAD_CELLS[7] = {{100, 220, 240, 356}, {356, 103, 544, 287}, {660, 220, 800, 356},
                                   {100, 364, 236, 500}, {244, 364, 408, 500}, {492, 364, 656, 500},
                                   {664, 364, 800, 500}};
static const char PAD_SHAPE[7] = {'A', 'D', 'A', 'B', 'C', 'C', 'B'};
static const int LED_REGION[7] = {0, 1, 2, 0, 0, 2, 2};
static const int LED_SLOTS[3][3] = {{0, 3, 4}, {1, -1, -1}, {2, 5, 6}};
static const struct { int up, down, left, right; } PAD_NEIGHBORS[7] = {
    {-1, 3, -1, 1}, {-1, 4, 0, 2}, {-1, 6, 1, -1}, {0, -1, -1, 4}, {0, -1, 3, 5}, {1, -1, 4, 6}, {2, -1, 5, -1}};
static const uint8_t COL_BORDER_IDLE[4] = {0x6A, 0x5A, 0x52, 255};
static const uint8_t COL_MOVE[4] = {66, 157, 255, 255};
static const uint8_t COL_TEXT[4] = {224, 230, 237, 255};
static const uint8_t COL_NAME_FOCUS[4] = {255, 236, 190, 255};
static const uint8_t COL_NAME[4] = {214, 220, 230, 255};
static const uint8_t COL_HINT[4] = {196, 204, 216, 255};

/* textures carry a margin around the shape: pads 8, focus glows 20, LED halos 22 (window units) */
#define PAD_M 8
#define GLOW_M 20
#define LED_M 22
#define PORTRAIT_TEX 146.0f   /* portrait texture: 120 px circle + 13 px margin each side */
#define PORTRAIT_CIRCLE 120.0f

/* ------------------------------------------------------------------ UI state */

enum { SCR_PAD, SCR_ACTION, SCR_FRANCHISE, SCR_ROSTER, SCR_PLUS, SCR_SETTINGS, SCR_SEARCH };
enum { SORT_DEFAULT = 0, SORT_USER = 1, SORT_STORY = 2, SORT_FAVORITES = 3, SORT_YEAR1 = 4, SORT_YEAR2 = 5 };
static const int SORT_CYCLE[] = {SORT_DEFAULT, SORT_YEAR1, SORT_YEAR2, SORT_USER, SORT_FAVORITES, SORT_STORY};
#define NSORT 6

typedef struct { int entry, group; } rslot;

static struct {
    int open;                 /* picker visible */
    uint32_t open_seq;
    int screen, slot, action;
    int moving, move_src;
    int sort;
    int fr_list[MAXW + 1], fr_n, fr_fav;   /* display list (world idx), leading Favorites tile */
    int fr_index, fr_top;                  /* logical index incl. the Favorites tile */
    rslot roster[96];
    int r_n, r_chars, r_index, r_top;
    int r_source;                          /* >= 0 world, -1 favorites, -2 story */
    int plus_group, plus_index, plus_back;
    int set_index, set_top;                /* Settings: row in the full list (headings included) */
    /* Search: query typed on the on-screen keyboard, results (entry indices) */
    char query[32];
    int s_res[80], s_n, s_index, s_top, s_zone, k_row, k_col, s_back;
    char status[160];
    uint64_t status_t;
    int status_tone;                       /* 0 neutral, 1 removed, 2 moved */
    /* peek */
    int peek_held;
    float peek_shown;
    uint64_t peek_t;
    int peek_hiding;
    /* input */
    uint32_t ring_rd, nav, held_since_dir;
    uint64_t repeat_at;
    /* pad snapshot */
    int used[7], bin[7];
    uint32_t fid[7];
    int linked;
} S;

static void set_status(int tone, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(S.status, sizeof S.status, fmt, ap);
    va_end(ap);
    S.status_t = ms_now();
    S.status_tone = tone;
}

/* ------------------------------------------------------------------ actions (worker thread) */

enum { ACT_NONE, ACT_LOAD, ACT_MOVE, ACT_CLEAR, ACT_CLEARALL };
static pthread_mutex_t g_act_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_act_cv = PTHREAD_COND_INITIALIZER;
static struct { int kind, a, b, busy; char result[160]; int result_rc, result_ready; } g_act;

static void *action_thread(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_act_mx);
        while (g_act.kind == ACT_NONE) pthread_cond_wait(&g_act_cv, &g_act_mx);
        int kind = g_act.kind, a = g_act.a, b = g_act.b;
        pthread_mutex_unlock(&g_act_mx);
        char msg[160] = "";
        int rc = kind == ACT_LOAD ? vtp_load(a, b, msg, sizeof msg)
               : kind == ACT_MOVE ? vtp_move(a, b, msg, sizeof msg)
               : kind == ACT_CLEARALL ? vtp_clearall(msg, sizeof msg)
               : vtp_clear(a, msg, sizeof msg);
        pthread_mutex_lock(&g_act_mx);
        snprintf(g_act.result, sizeof g_act.result, "%s", msg);
        g_act.result_rc = rc;
        g_act.result_ready = kind;
        g_act.kind = ACT_NONE;
        g_act.busy = 0;
        pthread_mutex_unlock(&g_act_mx);
    }
    return 0;
}

static int act_start(int kind, int a, int b) {
    pthread_mutex_lock(&g_act_mx);
    int ok = !g_act.busy;
    if (ok) { g_act.kind = kind; g_act.a = a; g_act.b = b; g_act.busy = 1; pthread_cond_signal(&g_act_cv); }
    pthread_mutex_unlock(&g_act_mx);
    if (!ok) set_status(0, "Busy - try again in a moment.");
    return ok;
}

static void act_collect(void) {
    pthread_mutex_lock(&g_act_mx);
    int kind = g_act.result_ready, rc = g_act.result_rc;
    char msg[160];
    snprintf(msg, sizeof msg, "%s", g_act.result);
    g_act.result_ready = 0;
    pthread_mutex_unlock(&g_act_mx);
    if (!kind) return;
    set_status(rc ? 1 : (kind == ACT_CLEAR || kind == ACT_CLEARALL) ? 1 : kind == ACT_MOVE ? 2 : 0, "%s", msg);
}

/* ------------------------------------------------------------------ screens */

static void picker_close(int tell_hook) {
    if (tell_hook) vtp_ext_request(-1, 1);
    S.open = 0;
    S.moving = 0;
}

static void rebuild_franchise_list(void) {
    S.fr_n = 0;
    S.fr_fav = S.sort == SORT_USER;
    for (int i = 0; i < NW; i++) {
        if (S.sort == SORT_YEAR1 && !Wd[i].year1) continue;
        if (S.sort == SORT_YEAR2 && !Wd[i].year2) continue;
        S.fr_list[S.fr_n++] = i;
    }
    int total = S.fr_n + S.fr_fav;
    if (S.fr_index >= total) S.fr_index = total ? total - 1 : 0;
}

static void roster_add_char(int e) { if (e >= 0 && S.r_n < 96) { S.roster[S.r_n].entry = e; S.roster[S.r_n].group = -1; S.r_n++; } }
static void roster_add_vehicle(int e, int g) { if (e >= 0 && S.r_n < 96) { S.roster[S.r_n].entry = e; S.roster[S.r_n].group = g; S.r_n++; } }

static void open_roster(int source) {
    S.r_n = 0;
    S.r_source = source;
    if (source >= 0) {
        for (int i = 0; i < NE; i++) if (E[i].kind == 'c' && E[i].world == source) roster_add_char(i);
        S.r_chars = S.r_n;
        for (int g = 0; g < NG; g++) if (G[g].world == source && G[g].count) roster_add_vehicle(G[g].first, g);
    } else if (source == -1) {
        int fav[256];
        int n = vtp_favorites(fav, 256);
        for (int i = 0; i < n; i++) { int e = entry_by_bin(fav[i]); if (e >= 0 && E[e].kind == 'c') roster_add_char(e); }
        S.r_chars = S.r_n;
        for (int i = 0; i < n; i++) { int e = entry_by_bin(fav[i]); if (e >= 0 && E[e].kind == 'v') roster_add_vehicle(e, E[e].group); }
    } else {
        int dc = find_world("DC Comics"), lotr = find_world("The Lord of the Rings"), movie = find_world("The LEGO Movie");
        roster_add_char(find_char(dc, "Batman"));
        roster_add_char(find_char(lotr, "Gandalf"));
        roster_add_char(find_char(movie, "Wyldstyle"));
        S.r_chars = S.r_n;
        for (int g = 0; g < NG; g++) if (G[g].world == dc && !strcmp(G[g].base, "Batmobile")) { roster_add_vehicle(G[g].first, g); break; }
    }
    S.r_index = 0;
    S.r_top = 0;
    S.screen = SCR_ROSTER;
}

static void open_browse(void) {
    if (S.sort == SORT_STORY) open_roster(-2);
    else if (S.sort == SORT_FAVORITES) open_roster(-1);
    else { rebuild_franchise_list(); S.screen = SCR_FRANCHISE; }
}

static void cycle_sort(int dir) {
    int idx = 0;
    for (int i = 0; i < NSORT; i++) if (SORT_CYCLE[i] == S.sort) idx = i;
    idx = (idx + dir + NSORT) % NSORT;
    S.sort = SORT_CYCLE[idx];
    vtp_sort_set(S.sort);
    S.fr_index = 0; S.fr_top = 0;
    open_browse();
    static const char *names[] = {"Default", "User", "Story", "Favorites", "Year 1", "Year 2", "Abilities"};
    set_status(0, "Sort: %s", names[S.sort]);
}

/* roster geometry helpers (GetRosterVisual*) */
#define RCOLS 5
#define RVIS 3
static int r_char_rows(void) { return (S.r_chars + RCOLS - 1) / RCOLS; }
static int r_rows(void) { return r_char_rows() + (S.r_n - S.r_chars + RCOLS - 1) / RCOLS; }
static int r_row_count(int row) {
    int cr = r_char_rows();
    if (row < cr) { int n = S.r_chars - row * RCOLS; return n < RCOLS ? n : RCOLS; }
    int n = (S.r_n - S.r_chars) - (row - cr) * RCOLS;
    return n < RCOLS ? n : RCOLS;
}
static void r_pos(int index, int *row, int *col) {
    if (index < S.r_chars) { *row = index / RCOLS; *col = index % RCOLS; return; }
    int v = index - S.r_chars;
    *row = r_char_rows() + v / RCOLS; *col = v % RCOLS;
}
static int r_index_at(int row, int col) {
    int cr = r_char_rows();
    if (row < cr) return row * RCOLS + col;
    return S.r_chars + (row - cr) * RCOLS + col;
}
static int r_offset(int row) { return (RCOLS - r_row_count(row)) * 156 / 2; }

static void roster_move(int dx, int dy) {
    if (!S.r_n) return;
    int rows = r_rows(), row, col;
    r_pos(S.r_index, &row, &col);
    if (dy) {
        int cx = r_offset(row) + col * 156;
        row = (row + dy + rows) % rows;
        int cnt = r_row_count(row);
        long t = lroundf((float)(cx - r_offset(row)) / 156.0f);
        col = (int)(t < 0 ? 0 : t > cnt - 1 ? cnt - 1 : t);
    }
    if (dx) { int cnt = r_row_count(row); col = (col + dx + cnt) % cnt; }
    S.r_index = r_index_at(row, col);
    while (row < S.r_top) S.r_top--;
    while (row >= S.r_top + RVIS) S.r_top++;
    if (S.r_top < 0) S.r_top = 0;
}

#define FCOLS 4
#define FVIS 4
static void franchise_move(int dx, int dy) {
    int total = S.fr_n + S.fr_fav;
    if (!total) return;
    int rows = (total + FCOLS - 1) / FCOLS;
    int row = S.fr_index / FCOLS, col = S.fr_index % FCOLS;
    row = (row + dy + rows) % rows;
    int last = (total - row * FCOLS < FCOLS ? total - row * FCOLS : FCOLS) - 1;
    col = (col + dx + last + 1) % (last + 1);
    S.fr_index = row * FCOLS + col;
    while (row < S.fr_top) S.fr_top--;
    while (row >= S.fr_top + FVIS) S.fr_top++;
    if (S.fr_top < 0) S.fr_top = 0;
}

static void load_entry(int e) {
    if (e < 0) return;
    if (act_start(ACT_LOAD, S.slot, E[e].bin)) {
        set_status(0, "Loading %s...", E[e].name);
        S.screen = SCR_PAD;
        if (C.fast_load) picker_close(1);
    }
}

static void settings_adjust(int dir, int allow_action);
static void search_key(void);

static void confirm(void) {
    switch (S.screen) {
    case SCR_PAD:
        if (S.moving) {
            if (act_start(ACT_MOVE, S.move_src, S.slot)) set_status(2, "Moving...");
            S.moving = 0;
        } else { S.screen = SCR_ACTION; S.action = 0; }
        break;
    case SCR_ACTION:
        if (S.action == 0) open_browse();
        else if (S.action == 2) { if (act_start(ACT_CLEAR, S.slot, 0)) set_status(1, "Clearing..."); S.screen = SCR_PAD; }
        else {
            if (!S.used[S.slot]) { set_status(0, "There is nothing tracked on that pad to move."); S.screen = SCR_PAD; break; }
            S.move_src = S.slot; S.moving = 1; S.screen = SCR_PAD;
            set_status(0, "Move: pick a destination pad.");
        }
        break;
    case SCR_FRANCHISE:
        if (S.fr_fav && S.fr_index == 0) open_roster(-1);
        else if (S.fr_index - S.fr_fav < S.fr_n) open_roster(S.fr_list[S.fr_index - S.fr_fav]);
        break;
    case SCR_ROSTER:
        if (S.r_index < S.r_n) {
            rslot *r = &S.roster[S.r_index];
            if (r->group >= 0 && G[r->group].count > 1 && S.r_source != -1) {
                S.plus_group = r->group; S.plus_index = 0; S.plus_back = S.r_index; S.screen = SCR_PLUS;
            } else load_entry(r->entry);
        }
        break;
    case SCR_PLUS:
        load_entry(G[S.plus_group].first + S.plus_index);
        break;
    case SCR_SETTINGS:
        settings_adjust(1, 1);
        break;
    case SCR_SEARCH:
        if (S.s_zone == 0) search_key();
        else if (S.s_index < S.s_n) load_entry(S.s_res[S.s_index]);
        break;
    }
}

static void back(void) {
    switch (S.screen) {
    case SCR_PAD:
        if (S.moving) { S.moving = 0; S.slot = S.move_src; S.screen = SCR_ACTION; S.action = 1; }
        else picker_close(1);
        break;
    case SCR_ACTION: S.screen = SCR_PAD; break;
    case SCR_SETTINGS: S.screen = SCR_PAD; break;
    case SCR_SEARCH: S.screen = S.s_back; if (S.s_back == SCR_FRANCHISE) rebuild_franchise_list(); break;
    case SCR_FRANCHISE: S.screen = SCR_PAD; break;
    case SCR_ROSTER:
        if (S.r_source < 0 && (S.sort == SORT_FAVORITES || S.sort == SORT_STORY)) S.screen = SCR_PAD;
        else { rebuild_franchise_list(); S.screen = SCR_FRANCHISE; }
        break;
    case SCR_PLUS:
        S.screen = SCR_ROSTER;
        S.r_index = S.plus_back;
        {
            int row, col;
            r_pos(S.r_index, &row, &col);
            while (row < S.r_top) S.r_top--;
            while (row >= S.r_top + RVIS) S.r_top++;
        }
        break;
    }
}

static void toggle_favorite(int e) {
    if (e < 0) return;
    int now = vtp_toggle_favorite(E[e].bin);
    set_status(0, "%s %s", now ? "Favorited:" : "Unfavorited:", E[e].name);
}

/* ------------------------------------------------------------------ Settings */

/* LegoToypad's Settings screen (BuildSettingsEntries / AdjustSettingsEntry), with the rows that
 * mean something on a PS5: a categorised list, headings skipped by the focus, Left/Right walk a
 * value, Cross also runs the one-shot actions. Changes are saved to config.ini at once. */
enum { SA_HEADING, SA_BACKGROUND, SA_PADSKIN, SA_OPACITY, SA_UISIZE, SA_TEXTSIZE, SA_PEEK, SA_LEDS,
       SA_FASTLOAD, SA_WEB, SA_CLEARALL, SA_RESET };
enum { TONE_NEUTRAL, TONE_ON, TONE_OFF };
typedef struct { int action; const char *label; char value[64]; int tone; } srow_t;
#define SET_MAX 18
#define SET_VISIBLE 12
#define SET_TOP 130
#define SET_PITCH 32

static int g_nwall = 1;                     /* bundled wallpapers (ui/wall*.jpg) */
static int g_nskin = 1;
static char g_skin[4][24] = {"Default"};
static char g_web_url[48];

static void probe_appearance(void) {
    char path[160];
    load_sizes();
    g_nwall = 0;
    for (int i = 0; i < 64; i++) {
        snprintf(path, sizeof path, UI_DIR "/wall%d.jpg", i);
        if (access(path, F_OK)) break;
        g_nwall = i + 1;
    }
    char *d = slurp(UI_DIR "/skins.txt", 0);
    if (d) {
        g_nskin = 0;
        char *save = 0;
        for (char *ln = strtok_r(d, "\r\n", &save); ln && g_nskin < 4; ln = strtok_r(0, "\r\n", &save))
            snprintf(g_skin[g_nskin++], sizeof g_skin[0], "%s", ln);
        free(d);
        if (!g_nskin) { g_nskin = 1; snprintf(g_skin[0], sizeof g_skin[0], "Default"); }
    }
}

/* pad skins: "Toypad" (a real toypad), LegoToypad's "Default" (pad%d) and its extra skins (pk<n>_%d) */
static int skin_index(void) { return C.pad_skin >= 0 && C.pad_skin < g_nskin ? C.pad_skin : 0; }
static int skin_is_toypad(void) { return !strcmp(g_skin[skin_index()], "Toypad"); }
static void skin_pad_texture(int slot, char *buf, size_t n) {
    int i = skin_index(), def = 0;
    for (int k = 0; k < g_nskin; k++) if (!strcmp(g_skin[k], "Default")) def = k;
    if (i <= def || !strcmp(g_skin[i], "Toypad")) snprintf(buf, n, "pad%d", slot);
    else snprintf(buf, n, "pk%d_%d", i - def, slot);
}

static const float UI_SIZES[5] = {1.10f, 0.90f, 1.00f, 1.10f, 1.18f};
static const float TEXT_SIZES[5] = {1.25f, 1.00f, 1.15f, 1.30f, 1.50f};
static void apply_scale(void) {
    float ui = UI_SIZES[(unsigned)C.ui_size < 5 ? C.ui_size : 0];
    float tx = TEXT_SIZES[(unsigned)C.text_size < 5 ? C.text_size : 0];
    K = 1.5f * ui;
    OX = (1920 - 900 * K) / 2;
    OY = (1080 - 610 * K) / 2;
    FONT_MUL = ui * tx;
}

static int bg_count(void) { return (g_nwall ? g_nwall : 1) + 1; }   /* wallpapers + Clear */
static int bg_clear(void) { return C.background == 1; }
/* texture for the current background (position 0 default, 1 Clear, 2.. wallpaper position-1) */
static const char *bg_texture(char *buf, size_t n) {
    if (!g_nwall) return "bg";
    int wall = C.background < 1 ? 0 : C.background - 1;
    if (wall >= g_nwall) wall = 0;
    snprintf(buf, n, "wall%d", wall);
    return buf;
}

static void web_url(void) {
    if (g_web_url[0]) return;
    snprintf(g_web_url, sizeof g_web_url, "port 8765");
    int fd = socket(AF_INET, SOCK_DGRAM, 0);   /* no packet is sent: this only picks the route */
    if (fd < 0) return;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    a.sin_addr.s_addr = htonl(0x01010101);
    socklen_t len = sizeof a;
    if (!connect(fd, (struct sockaddr *)&a, sizeof a) && !getsockname(fd, (struct sockaddr *)&a, &len)) {
        uint32_t ip = ntohl(a.sin_addr.s_addr);
        if (ip) snprintf(g_web_url, sizeof g_web_url, "http://%u.%u.%u.%u:8765", ip >> 24, (ip >> 16) & 255,
                         (ip >> 8) & 255, ip & 255);
    }
    close(fd);
}

static int build_settings(srow_t *r) {
    int n = 0;
#define HEAD(t) (r[n].action = SA_HEADING, r[n].label = (t), r[n].value[0] = 0, r[n].tone = 0, n++)
#define ROW(a, t, tone_, ...) (r[n].action = (a), r[n].label = (t), r[n].tone = (tone_), \
                               snprintf(r[n].value, sizeof r[n].value, __VA_ARGS__), n++)
    static const char *peek[4] = {"Off", "Small", "Medium", "Large"};
    HEAD("Appearance");
    if (bg_clear()) ROW(SA_BACKGROUND, "Background", TONE_NEUTRAL, "Clear (see the game through it)");
    else if (C.background <= 0) ROW(SA_BACKGROUND, "Background", TONE_NEUTRAL, "Default");
    else ROW(SA_BACKGROUND, "Background", TONE_NEUTRAL, "Background %d", C.background);
    ROW(SA_PADSKIN, "Pad skin", TONE_NEUTRAL, "%s (%d/%d)", g_skin[skin_index()], skin_index() + 1, g_nskin);
    ROW(SA_OPACITY, "Window transparency", TONE_NEUTRAL, "%d%% opaque%s", C.opacity, C.opacity >= 100 ? " (solid)" : "");
    {
        int u = (unsigned)C.ui_size < 5 ? C.ui_size : 0, t = (unsigned)C.text_size < 5 ? C.text_size : 0;
        ROW(SA_UISIZE, "UI size", TONE_NEUTRAL, "%s%d%%%s", u ? "" : "Auto (", (int)(UI_SIZES[u] * 100 + 0.5f), u ? "" : ")");
        ROW(SA_TEXTSIZE, "Text size", TONE_NEUTRAL, "%s%d%%%s", t ? "" : "Auto (", (int)(TEXT_SIZES[t] * 100 + 0.5f), t ? "" : ")");
    }
    ROW(SA_PEEK, "Sneak peek", C.peek_size ? TONE_ON : TONE_OFF, "%s", peek[C.peek_size & 3]);
    ROW(SA_LEDS, "Toypad LEDs", vtp_led_mirror() ? TONE_ON : TONE_OFF, "%s", vtp_led_mirror() ? "On" : "Off");
    HEAD("Controls");
    ROW(SA_FASTLOAD, "Fast loading", C.fast_load ? TONE_ON : TONE_OFF, "%s", C.fast_load ? "On" : "Off");
    HEAD("System");
    web_url();
    ROW(SA_WEB, "Web remote", TONE_ON, "%s", g_web_url);
    ROW(SA_CLEARALL, "Clear all pads", TONE_NEUTRAL, "%s", "");
    ROW(SA_RESET, "Reset all settings to defaults", TONE_NEUTRAL, "%s", "");
#undef HEAD
#undef ROW
    return n;
}

static void settings_scroll(int n) {
    if (S.set_index - 1 < S.set_top) S.set_top = S.set_index - 1 < 0 ? 0 : S.set_index - 1;
    while (S.set_index >= S.set_top + SET_VISIBLE) S.set_top++;
    int max = n - SET_VISIBLE > 0 ? n - SET_VISIBLE : 0;
    if (S.set_top > max) S.set_top = max;
    if (S.set_top < 0) S.set_top = 0;
}

static void settings_open(void) {
    srow_t r[SET_MAX];
    int n = build_settings(r);
    S.screen = SCR_SETTINGS;
    if (S.set_index <= 0 || S.set_index >= n) S.set_index = 1;
    while (S.set_index < n - 1 && r[S.set_index].action == SA_HEADING) S.set_index++;
    settings_scroll(n);
}

static void settings_move(int dir) {
    srow_t r[SET_MAX];
    int n = build_settings(r);
    for (int guard = 0; guard < n; guard++) {
        S.set_index = (S.set_index + dir + n) % n;
        if (r[S.set_index].action != SA_HEADING) break;
    }
    settings_scroll(n);
}

static void settings_save(void) {
#ifndef CUI_HOST_TEST
    cui_save_config(&C);
#endif
}

static void settings_adjust(int dir, int allow_action) {
    srow_t r[SET_MAX];
    int n = build_settings(r);
    if (S.set_index < 0 || S.set_index >= n) return;
    int step = dir < 0 ? -1 : 1;
    switch (r[S.set_index].action) {
    case SA_BACKGROUND: { int c = bg_count(); C.background = ((C.background < 0 ? 0 : C.background) + step + c) % c; break; }
    case SA_PADSKIN: C.pad_skin = (skin_index() + step + g_nskin) % g_nskin; break;
    case SA_UISIZE: C.ui_size = (((unsigned)C.ui_size < 5 ? C.ui_size : 0) + step + 5) % 5; break;
    case SA_TEXTSIZE: C.text_size = (((unsigned)C.text_size < 5 ? C.text_size : 0) + step + 5) % 5; break;
    case SA_OPACITY:   /* +/-15 clamped, never wraps (CycleOpacity) */
        C.opacity += step * 15;
        if (C.opacity < 20) C.opacity = 20;
        if (C.opacity > 100) C.opacity = 100;
        break;
    case SA_PEEK: C.peek_size = (C.peek_size + step + 4) % 4; break;
    case SA_LEDS: vtp_led_mirror_set(!vtp_led_mirror()); return;   /* stored by the web remote */
    case SA_FASTLOAD: C.fast_load = !C.fast_load; break;
    case SA_CLEARALL:
        if (allow_action && act_start(ACT_CLEARALL, 0, 0)) set_status(1, "Clearing all pads...");
        return;
    case SA_RESET:
        if (!allow_action) return;
        C.background = 0; C.opacity = 92; C.pad_skin = 0; C.peek_size = 2; C.fast_load = 1;
        C.ui_size = 0; C.text_size = 0;
        vtp_led_mirror_set(1);
        set_status(0, "Settings reset to defaults.");
        break;
    default: return;
    }
    settings_save();
}

/* ------------------------------------------------------------------ Search */

/* Typed on an on-screen keyboard (the PS5 system keyboard cannot be opened from a payload).
 * Matches figure and vehicle names anywhere in the catalogue, then whole worlds by name;
 * results update with every key. */
static const char *KEYS[4][10] = {
    {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"},
    {"Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P"},
    {"A", "S", "D", "F", "G", "H", "J", "K", "L", "-"},
    {"Z", "X", "C", "V", "B", "N", "M", ".", "Space", "Del"}};
#define SCOLS 5
#define SVIS 2

static char lc(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

/* 0 prefix, 1 word prefix, 2 anywhere, -1 no match */
static int match_rank(const char *hay, const char *q) {
    size_t n = strlen(q);
    for (const char *h = hay; *h; h++) {
        size_t k = 0;
        while (k < n && h[k] && lc(h[k]) == lc(q[k])) k++;
        if (k == n) return h == hay ? 0 : (h[-1] == ' ' || h[-1] == '-' || h[-1] == '(') ? 1 : 2;
    }
    return -1;
}

static void search_run(void) {
    S.s_n = 0;
    S.s_index = 0;
    S.s_top = 0;
    if (!S.query[0]) return;
    for (int rank = 0; rank <= 3 && S.s_n < 80; rank++)
        for (int i = 0; i < NE && S.s_n < 80; i++) {
            int r = match_rank(E[i].name, S.query);
            if (r < 0 && E[i].kind == 'v' && E[i].group >= 0) {   /* a build found by its vehicle's name */
                r = match_rank(G[E[i].group].base, S.query);
                if (r >= 0) r = 2;
            }
            if (r < 0 && E[i].world >= 0 && E[i].world < NW && match_rank(Wd[E[i].world].name, S.query) >= 0) r = 3;
            if (r == rank) S.s_res[S.s_n++] = i;
        }
}

static void search_open(int from) {
    S.s_back = from;
    S.screen = SCR_SEARCH;
    S.s_zone = 0;
    S.k_row = 1; S.k_col = 0;
    search_run();
}

static void search_key(void) {
    const char *k = KEYS[S.k_row][S.k_col];
    size_t n = strlen(S.query);
    if (!strcmp(k, "Del")) { if (n) S.query[n - 1] = 0; }
    else if (!strcmp(k, "Space")) { if (n && n + 1 < sizeof S.query && S.query[n - 1] != ' ') { S.query[n] = ' '; S.query[n + 1] = 0; } }
    else if (n + 1 < sizeof S.query) { S.query[n] = k[0]; S.query[n + 1] = 0; }
    search_run();
}

static void search_scroll(void) {
    int row = S.s_index / SCOLS;
    while (row < S.s_top) S.s_top--;
    while (row >= S.s_top + SVIS) S.s_top++;
    if (S.s_top < 0) S.s_top = 0;
}

static void search_move(int dx, int dy) {
    if (S.s_zone == 0) {
        if (dx) S.k_col = (S.k_col + dx + 10) % 10;
        if (dy < 0) {
            if (S.k_row > 0) S.k_row--;
            else if (S.s_n) {   /* up off the keyboard: into the results, nearest column of the last visible row */
                int rows = (S.s_n + SCOLS - 1) / SCOLS, row = S.s_top + SVIS - 1 < rows ? S.s_top + SVIS - 1 : rows - 1;
                int col = S.k_col * SCOLS / 10;
                S.s_index = row * SCOLS + col < S.s_n ? row * SCOLS + col : S.s_n - 1;
                S.s_zone = 1;
                search_scroll();
            }
        }
        if (dy > 0 && S.k_row < 3) S.k_row++;
        return;
    }
    int rows = (S.s_n + SCOLS - 1) / SCOLS, row = S.s_index / SCOLS, col = S.s_index % SCOLS;
    if (dx) {
        int cnt = row == rows - 1 ? S.s_n - row * SCOLS : SCOLS;
        col = (col + dx + cnt) % cnt;
    }
    if (dy < 0 && row > 0) row--;
    if (dy > 0) {
        if (row + 1 < rows) row++;
        else { S.s_zone = 0; S.k_row = 0; S.k_col = col * 10 / SCOLS; return; }
    }
    S.s_index = row * SCOLS + col < S.s_n ? row * SCOLS + col : S.s_n - 1;
    search_scroll();
}

static void press(uint32_t p) {
    int dx = (p & B_RIGHT) ? 1 : (p & B_LEFT) ? -1 : 0, dy = (p & B_DOWN) ? 1 : (p & B_UP) ? -1 : 0;
    if (p & B_CROSS) { confirm(); return; }
    if (p & B_CIRCLE) { back(); return; }
    switch (S.screen) {
    case SCR_PAD:
        if (dx || dy) {
            int t = dy < 0 ? PAD_NEIGHBORS[S.slot].up : dy > 0 ? PAD_NEIGHBORS[S.slot].down
                  : dx < 0 ? PAD_NEIGHBORS[S.slot].left : PAD_NEIGHBORS[S.slot].right;
            if (t >= 0) S.slot = t;
        } else if (!S.moving && (p & B_SQUARE)) {
            if (!S.used[S.slot]) set_status(0, "There is nothing tracked on that pad to move.");
            else { S.move_src = S.slot; S.moving = 1; set_status(0, "Move: pick a destination pad."); }
        } else if (!S.moving && (p & B_R1)) open_browse();
        else if (!S.moving && (p & B_L1)) { if (act_start(ACT_CLEAR, S.slot, 0)) set_status(1, "Clearing..."); }
        else if (!S.moving && (p & B_TRIANGLE)) settings_open();   /* LegoToypad: Y opens Settings */
        break;
    case SCR_SETTINGS:
        if (dy) settings_move(dy);
        else if (dx) settings_adjust(dx, 0);
        break;
    case SCR_SEARCH:
        if (dx || dy) search_move(dx, dy);
        else if (p & B_SQUARE) {   /* backspace */
            size_t n = strlen(S.query);
            if (n) { S.query[n - 1] = 0; search_run(); }
        } else if ((p & B_TRIANGLE) && S.s_zone == 1 && S.s_index < S.s_n) toggle_favorite(S.s_res[S.s_index]);
        break;
    case SCR_ACTION:
        if (dx) S.action = (S.action + dx + 3) % 3;
        break;
    case SCR_FRANCHISE:
        if (dx || dy) franchise_move(dx, dy);
        else if (p & B_SQUARE) search_open(SCR_FRANCHISE);
        else if (p & B_R1) cycle_sort(1);
        else if (p & B_L1) cycle_sort(-1);
        break;
    case SCR_ROSTER:
        if (dx || dy) roster_move(dx, dy);
        else if (p & B_SQUARE) search_open(SCR_ROSTER);
        else if (p & B_TRIANGLE) { if (S.r_index < S.r_n) { toggle_favorite(S.roster[S.r_index].entry);
                                    if (S.r_source == -1) { int keep = S.r_index; open_roster(-1);
                                        S.r_index = keep < S.r_n ? keep : S.r_n - 1; if (S.r_index < 0) S.r_index = 0; } } }
        else if ((p & (B_R1 | B_L1)) && S.r_source < 0 && (S.sort == SORT_FAVORITES || S.sort == SORT_STORY))
            cycle_sort((p & B_R1) ? 1 : -1);
        break;
    case SCR_PLUS:
        if (dx) { int n = G[S.plus_group].count; S.plus_index = (S.plus_index + dx + n) % n; }
        else if (p & B_TRIANGLE) toggle_favorite(G[S.plus_group].first + S.plus_index);
        break;
    }
}

/* ------------------------------------------------------------------ input */

static void input_tick(const vtp_ext *e) {
    uint64_t now = ms_now();
    if (!e->valid) return;
    if (e->open && e->open_seq != S.open_seq) {          /* the hook just opened the picker */
        S.open_seq = e->open_seq;
        S.open = 1;
        S.screen = SCR_PAD;
        S.moving = 0;
        S.ring_rd = e->wr;
        S.nav = C.combo;   /* the opening combo is still held: its release is not a press */
        S.peek_held = 0; S.peek_shown = 0; S.peek_hiding = 0;
    }
    if (!e->open && S.open) { S.open = 0; S.moving = 0; }   /* closed by the combo */
    if (!S.open) return;
    for (; S.ring_rd != e->wr; S.ring_rd++) {
        if (e->wr - S.ring_rd > 32) S.ring_rd = e->wr - 32;
        uint32_t nav = e->ring[S.ring_rd & 31];
        uint32_t pressed = nav & ~S.nav;
        S.nav = nav;
        if (pressed & (B_UP | B_DOWN | B_LEFT | B_RIGHT)) { S.held_since_dir = 1; S.repeat_at = now + 380; }
        if (pressed) press(pressed);
        if (!S.open) return;
    }
    /* D-pad / stick auto-repeat while held */
    uint32_t dirs = S.nav & (B_UP | B_DOWN | B_LEFT | B_RIGHT);
    if (!dirs) S.held_since_dir = 0;
    else if (S.held_since_dir && now >= S.repeat_at) { S.repeat_at = now + 110; press(dirs); }
}

/* ------------------------------------------------------------------ drawing */

static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }

/* portrait texture (circle 120 px of a 146 px texture) placed so its circle is d wide at (cx, cy) */
static void portrait(int id, int e, float cx, float cy, float d, float alpha) {
    char name[24];
    snprintf(name, sizeof name, "p%d", E[e].bin);
    float s = d / PORTRAIT_CIRCLE, w = PORTRAIT_TEX * s;
    img(id, name, cx - w / 2, cy - w / 2, w, w, alpha, 0);
}

static void portrait_focus(int id, int e, float cx, float cy, float d) {
    float s = d / PORTRAIT_CIRCLE, w = PORTRAIT_TEX * s;
    uint8_t tint[4] = {E[e].rgb[0], E[e].rgb[1], E[e].rgb[2], 255};
    img(id, "pfocus", cx - w / 2, cy - w / 2, w, w, 1.0f, tint);
}

/* pads + LEDs + occupants; cells in canvas pixels, `u` = canvas px per window unit */
/* "Toypad" skin: the real toypad is drawn a little smaller (TP_S) and lower (TP_DY) than
 * LegoToypad's pad grid so its base plate clears the wordmark band. Window units. */
#define TP_S 0.86f
#define TP_DY 36.0f
static const rectf TP_PLATE = {56, 46, 844, 558};
static rectf tp_xf(rectf r) {
    rectf o = {450 + (r.l - 450) * TP_S, 301.5f + (r.t - 301.5f) * TP_S + TP_DY,
               450 + (r.r - 450) * TP_S, 301.5f + (r.b - 301.5f) * TP_S + TP_DY};
    return o;
}
/* a pad cell of the picker in window units (skin dependent) */
static rectf pad_cell_w(int i) { return skin_is_toypad() ? tp_xf(PAD_CELLS[i]) : PAD_CELLS[i]; }

static rectf region_box(const rectf cells[7], int r) {
    rectf bb = {1e9f, 1e9f, -1e9f, -1e9f};
    for (int k = 0; k < 3; k++) {
        int s = LED_SLOTS[r][k];
        if (s < 0) continue;
        bb.l = fminf(bb.l, cells[s].l); bb.t = fminf(bb.t, cells[s].t);
        bb.r = fmaxf(bb.r, cells[s].r); bb.b = fmaxf(bb.b, cells[s].b);
    }
    return bb;
}

/* pads + LEDs + occupants; cells in canvas pixels, `u` = canvas px per window unit */
static void draw_pads(const rectf cells[7], float u, int peek, float alpha, int focus_slot) {
    int mode[3]; uint8_t rgb[3][3]; float inten[3];
    vtp_leds(mode, rgb, inten);
    int mirror = vtp_led_mirror();
    int filter = (g_caps & OVL_CAP_FILTER) != 0;
    int toypad = skin_is_toypad();
    char name[32];
    if (toypad && !peek) {
        rectf p = tp_xf(TP_PLATE);
        img(W_PLATE, "tp_plate", X(p.l), Y(p.t), (p.r - p.l) * K, (p.b - p.t) * K, alpha, 0);
    }
    /* LED halos behind the pads (DrawLedRegionGlows) */
    for (int r = 0; r < 3; r++) {
        if (!mirror || !mode[r] || inten[r] <= 0.004f || !filter) continue;
        rectf bb = region_box(cells, r);
        float m = LED_M * u * (toypad && !peek ? TP_S : 1.0f);
        snprintf(name, sizeof name, peek ? "ledp%d" : "ledo%d", r);
        uint8_t tint[4] = {rgb[r][0], rgb[r][1], rgb[r][2], 255};
        img(W_HALO0 + r, name, bb.l - m, bb.t - m, bb.r - bb.l + 2 * m, bb.b - bb.t + 2 * m, inten[r] * alpha, tint);
    }
    /* Toypad skin: one white piece per LED region, lit as a whole like the real pad */
    if (toypad) {
        for (int r = 0; r < 3; r++) {
            rectf bb = region_box(cells, r);
            if (r == 1) {   /* the disc is round: the shorter side of the centre cell */
                float d = fminf(bb.r - bb.l, bb.b - bb.t), cx = (bb.l + bb.r) / 2, cy = (bb.t + bb.b) / 2;
                bb = (rectf){cx - d / 2, cy - d / 2, cx + d / 2, cy + d / 2};
            }
            snprintf(name, sizeof name, "tp_r%d", r);
            img(W_TPREG0 + r, name, bb.l, bb.t, bb.r - bb.l, bb.b - bb.t, alpha, 0);
            if (mirror && mode[r] && (inten[r] > 0.004f || mode[r] == 2)) {
                int dark = mode[r] == 2 && inten[r] <= 0.004f;
                uint8_t tint[4] = {dark ? 0 : rgb[r][0], dark ? 0 : rgb[r][1], dark ? 0 : rgb[r][2], 255};
                float a = dark ? 150.0f / 255.0f : 225.0f / 255.0f * inten[r];
                if (filter) img(W_LEDT0 + r, name, bb.l, bb.t, bb.r - bb.l, bb.b - bb.t, a * alpha, tint);
                else {
                    uint8_t fill[4] = {tint[0], tint[1], tint[2], (uint8_t)(a * alpha * 255)};
                    for (int k = 0; k < 3; k++) {
                        int sl = LED_SLOTS[r][k];
                        if (sl < 0) continue;
                        const rectf *c = &cells[sl];
                        panel(W_LEDP0 + sl, c->l + 4 * u, c->t + 4 * u, c->r - c->l - 8 * u, c->b - c->t - 8 * u, fill);
                    }
                }
            }
        }
    }
    for (int i = 0; i < 7; i++) {
        const rectf *c = &cells[i];
        float cw = c->r - c->l, ch = c->b - c->t;
        char shape = PAD_SHAPE[i];
        int r = LED_REGION[i];
        int lit = mirror && mode[r] != 0;
        int e = S.used[i] ? entry_by_bin(S.bin[i]) : -1;
        int selected = !peek && i == focus_slot;
        int source = !peek && S.moving && i == S.move_src;
        /* focus halo behind the pad */
        if ((selected || source) && !toypad) {
            float m = GLOW_M * u;
            snprintf(name, sizeof name, selected ? "glowsel%c" : "glowmove%c", shape);
            img(W_GLOW0 + i, name, c->l - m, c->t - m, cw + 2 * m, ch + 2 * m, alpha, 0);
        }
        if (toypad) {
            /* the spot itself is part of the white piece: only the focus and move outlines */
            float m = PAD_M * u;
            if (selected) {
                snprintf(name, sizeof name, "sel%c", shape);
                img(W_BORD0 + i, name, c->l - m, c->t - m, cw + 2 * m, ch + 2 * m, alpha, 0);
            } else if (source) {
                snprintf(name, sizeof name, "line%c", shape);
                img(W_BORD0 + i, name, c->l - m, c->t - m, cw + 2 * m, ch + 2 * m, alpha, filter ? COL_MOVE : 0);
            }
        } else {
            /* pad art (a lit region shows bare outlines so the LED colour reads true) */
            if (!lit) {
                skin_pad_texture(i, name, sizeof name);
                img(W_ART0 + i, name, c->l, c->t, cw, ch, alpha, 0);
                if (S.used[i] && e >= 0 && filter) {
                    uint8_t tint[4] = {E[e].rgb[0], E[e].rgb[1], E[e].rgb[2], 255};
                    snprintf(name, sizeof name, "mask%c", shape);
                    img(W_OCCT0 + i, name, c->l, c->t, cw, ch, alpha * 42.0f / 255.0f, tint);
                }
            } else if (inten[r] > 0.004f || mode[r] == 2) {
                /* DrawLedRegionTint: colour at 210 * intensity, a flash's dark beat darkens the tile */
                int dark = mode[r] == 2 && inten[r] <= 0.004f;
                uint8_t tint[4] = {dark ? 0 : rgb[r][0], dark ? 0 : rgb[r][1], dark ? 0 : rgb[r][2], 255};
                float a = dark ? 168.0f / 255.0f : 210.0f / 255.0f * inten[r];
                if (filter) {
                    snprintf(name, sizeof name, "mask%c", shape);
                    img(W_LEDT0 + i, name, c->l, c->t, cw, ch, a * alpha, tint);
                } else {
                    uint8_t fill[4] = {tint[0], tint[1], tint[2], (uint8_t)(a * alpha * 255)};
                    panel(W_LEDP0 + i, c->l + 4 * u, c->t + 4 * u, cw - 8 * u, ch - 8 * u, fill);
                }
            }
            /* border: selected double stroke, else idle / occupant / move-source colour */
            float m = PAD_M * u;
            if (selected) {
                snprintf(name, sizeof name, "sel%c", shape);
                img(W_BORD0 + i, name, c->l - m, c->t - m, cw + 2 * m, ch + 2 * m, alpha, 0);
            } else {
                uint8_t tint[4];
                if (source) memcpy(tint, COL_MOVE, 4);
                else if (S.used[i] && e >= 0) { tint[0] = E[e].rgb[0]; tint[1] = E[e].rgb[1]; tint[2] = E[e].rgb[2]; tint[3] = 255; }
                else memcpy(tint, COL_BORDER_IDLE, 4);
                snprintf(name, sizeof name, "line%c", shape);
                img(W_BORD0 + i, name, c->l - m, c->t - m, cw + 2 * m, ch + 2 * m, alpha, filter ? tint : 0);
            }
        }
        /* occupant portrait: 80% of the shorter side (DrawPadOccupant) */
        if (S.used[i] && e >= 0) {
            float d = fminf(cw, ch) * 0.80f;
            if (!peek) d = clampf(d, 84 * u * (toypad ? TP_S : 1.0f), 148 * u);
            portrait(W_OCC0 + i, e, (c->l + c->r) / 2, (c->t + c->b) / 2, d, alpha);
        }
    }
}

static void overlay_pad_cells(rectf out[7]) {
    for (int i = 0; i < 7; i++) {
        rectf w = pad_cell_w(i);
        out[i].l = X(w.l); out[i].t = Y(w.t);
        out[i].r = X(w.r); out[i].b = Y(w.b);
    }
}

/* ComputePeekCells on the 1920x1080 canvas */
static float peek_cells(rectf c[7]) {
    static const float scales[4] = {0.0f, 0.78f, 1.0f, 1.26f};
    int sz = C.peek_size < 1 ? 1 : C.peek_size > 3 ? 3 : C.peek_size;
    const float width = 1920, height = 1080;
    float s = height * 0.145f * scales[sz] / 136.0f;
    float mx = width * 0.022f, my = height * 0.030f, gutter = 10 * s, vgap = 24 * s;
    float lower = height - my - 136 * s, upper = lower - vgap - 136 * s;
#define R(i, x, y, w, h) c[i] = (rectf){(x), (y), (x) + (w), (y) + (h)}
    R(3, mx, lower, 136 * s, 136 * s);
    R(4, mx + 136 * s + gutter, lower, 164 * s, 136 * s);
    R(0, mx, upper, 140 * s, 136 * s);
    R(6, width - mx - 136 * s, lower, 136 * s, 136 * s);
    R(5, width - mx - 136 * s - gutter - 164 * s, lower, 164 * s, 136 * s);
    R(2, width - mx - 140 * s, upper, 140 * s, 136 * s);
    R(1, (width - 188 * s) / 2, my, 188 * s, 184 * s);
#undef R
    return s;
}

#define HINT_Y 596.0f
static void hint(int *n, const char *glyph, const char *text, float *x) {
    if (*n >= 7) return;
    char name[24];
    snprintf(name, sizeof name, "g_%s", glyph);
    float sz = 23 * K, cy = Y(HINT_Y), f = FS(22);
    float gw = img_fit(W_HICON0 + *n, name, *x, cy - sz / 2, sz * 1.6f, sz, 0, 1, 1.0f, 0);
    float w = strlen(text) * f * 0.62f + 8;
    label(W_HTEXT0 + *n, text, *x + gw + 6, cy - f * 0.6f, w, 22, 0, 1, COL_HINT, 1.0f);
    *x += gw + 6 + w + 14 * K / 1.5f;
    (*n)++;
}

static void draw_hints(void) {
    int n = 0;
    float x0 = X(18), x = x0;
    switch (S.screen) {
    case SCR_PAD:
        if (S.moving) { hint(&n, "A", "Drop here", &x); hint(&n, "B", "Cancel", &x); break; }
        hint(&n, "A", "Select", &x); hint(&n, "B", "Close", &x); hint(&n, "X", "Move", &x);
        hint(&n, "RB", "Load", &x); hint(&n, "LB", "Clear", &x);
        if (C.peek_size) hint(&n, "LT", "Peek (hold)", &x);
        break;
    case SCR_SETTINGS: hint(&n, "A", "Change", &x); hint(&n, "B", "Back", &x); hint(&n, "DpadRight", "Adjust", &x); break;
    case SCR_ACTION: hint(&n, "A", "Choose", &x); hint(&n, "B", "Back", &x); break;
    case SCR_FRANCHISE: hint(&n, "A", "Open", &x); hint(&n, "B", "Back", &x); hint(&n, "X", "Search", &x);
                        hint(&n, "LB", "Prev sort", &x); hint(&n, "RB", "Next sort", &x); break;
    case SCR_ROSTER: hint(&n, "A", "Load", &x); hint(&n, "B", "Back", &x); hint(&n, "X", "Search", &x);
                     hint(&n, "Y", "Favorite", &x); break;
    case SCR_PLUS: hint(&n, "A", "Load", &x); hint(&n, "B", "Back", &x); hint(&n, "Y", "Favorite", &x); break;
    case SCR_SEARCH:
        if (S.s_zone) { hint(&n, "A", "Load", &x); hint(&n, "B", "Back", &x); hint(&n, "Y", "Favorite", &x); hint(&n, "DpadDown", "Keyboard", &x); }
        else { hint(&n, "A", "Type", &x); hint(&n, "X", "Delete", &x); hint(&n, "B", "Back", &x); hint(&n, "DpadUp", "Results", &x); }
        break;
    }
    /* on the Clear background the hints sit on the game: give them a dark strip */
    if (bg_clear() && n) {
        float h = 23 * K + 10, cy = Y(HINT_Y);
        img(W_HINTBG, "hintbg", x0 - 12, cy - h / 2, x - x0 + 10, h, 1.0f, 0);
    }
}

static void draw_toast(float alpha_scale) {
    if (!S.status[0]) return;
    uint64_t age = ms_now() - S.status_t;
    float a = age < 2600 ? 1.0f : age < 3000 ? 1.0f - (age - 2600) / 400.0f : 0.0f;
    a *= alpha_scale;
    if (a <= 0.01f) return;
    float h = fmaxf(44 * K, FS(28) + 20), textw = strlen(S.status) * FS(28) * 0.6f;
    float boxw = fminf(720 * K, textw + 44 * K);
    float x = (1920 - boxw) / 2, y = Y(610 - 20 - 44 - 34);
    static const uint8_t tones[3][4] = {{96, 200, 255, 255}, {230, 70, 70, 255}, {230, 190, 60, 255}};
    const uint8_t *bc = tones[S.status_tone % 3];
    float cap = h;
    img(W_TL, "toastl", x, y, cap, h, a, 0);
    img(W_TM, "toastm", x + cap, y, fmaxf(0, boxw - 2 * cap), h, a, 0);
    img(W_TR, "toastr", x + boxw - cap, y, cap, h, a, 0);
    img(W_TBL, "toastbl", x, y, cap, h, a, bc);
    img(W_TBM, "toastbm", x + cap, y, fmaxf(0, boxw - 2 * cap), h, a, bc);
    img(W_TBR, "toastbr", x + boxw - cap, y, cap, h, a, bc);
    static const uint8_t white[4] = {255, 255, 255, 255};
    label(W_TTEXT, S.status, x + 10, y + h / 2 - FS(28) * 0.6f, boxw - 20, 28, 1, 1, white, a);
}

static void draw_pad_screen(void) {
    rectf cells[7];
    overlay_pad_cells(cells);
    draw_pads(cells, K, 0, 1.0f, S.slot);
    if (S.screen == SCR_ACTION) {
        static const int sizes[7] = {36, 42, 38, 42, 42, 42, 42};
        static const char *btn[3] = {"btn_load", "btn_move", "btn_clear"};
        int bs = sizes[S.slot], gap = S.slot == 0 ? 3 : 4;
        rectf cw = pad_cell_w(S.slot);
        const rectf *c = &cw;
        float roww = 3 * bs + 2 * gap, rx = c->l + (c->r - c->l - roww) / 2;
        float ry = (S.slot == 0 || S.slot == 2) ? c->b - bs - 8 : c->b + 4;
        for (int a = 0; a < 3; a++) {
            int grow = a == S.action ? 4 : 0;
            float x = rx + a * (bs + gap) - grow / 2.0f, y = ry - grow / 2.0f, s = bs + grow;
            if (a == S.action) img(W_ACTHALO, "btnhalo", X(x - 2), Y(y - 2), (s + 4) * K, (s + 4) * K, 1.0f, 0);
            img(W_ACT0 + a, btn[a], X(x), Y(y), s * K, s * K, 1.0f, 0);
        }
    }
    /* focused occupant's name (DrawOccupantLabel) */
    if (S.used[S.slot]) {
        int e = entry_by_bin(S.bin[S.slot]);
        if (e >= 0) {
            rectf cw = pad_cell_w(S.slot);
            const rectf *c = &cw;
            static const uint8_t muted[4] = {214, 222, 234, 255};
            if (skin_is_toypad()) {   /* on the plate the name would sit on grey: put it under the toypad */
                rectf p = tp_xf(TP_PLATE);
                label(W_OCCNAME, E[e].name, X(p.l), Y(p.b + 3), (p.r - p.l) * K, 26, 1, 1, muted, 0.95f);
            } else if (S.slot >= 3) label(W_OCCNAME, E[e].name, X((c->l + c->r) / 2 - 130), Y(c->b + 52), 260 * K, 26, 1, 1, muted, 0.85f);
            else if (S.slot != 2) label(W_OCCNAME, E[e].name, X(c->r + 14), Y((c->t + c->b) / 2 - 12), (S.slot == 1 ? 90 : 100) * K, 26, 0, 2, muted, 0.85f);
            else label(W_OCCNAME, E[e].name, X(14), Y((c->t + c->b) / 2 - 12), (c->l - 28) * K, 26, 2, 2, muted, 0.85f);
        }
    }
}

static void draw_scrollbar(float top, float bottom, int total, int visible, int toprow) {
    if (total <= visible) return;
    float track = bottom - top, thumb = fmaxf(22, track * visible / total);
    float y = top + (track - thumb) * toprow / (float)(total - visible);
    img(W_SCROLL, "scroll", X(900 - 10 - 16), Y(y), 10 * K, thumb * K, 1.0f, 0);
}

static void draw_sort_badge(void) {
    char name[16];
    snprintf(name, sizeof name, "sort%d", S.sort);
    img_fit(W_SORT, name, X(450 - 150), Y(24), 300 * K, 62 * K, 1, 1, 1.0f, 0);
}

static void draw_franchise(void) {
    draw_sort_badge();
    int total = S.fr_n + S.fr_fav;
    for (int row = 0; row < FVIS; row++)
        for (int col = 0; col < FCOLS; col++) {
            int idx = (S.fr_top + row) * FCOLS + col, slot = row * FCOLS + col;
            if (idx >= total) continue;
            float x = 40 + col * 210, y = 128 + row * 108;
            char name[16];
            if (S.fr_fav && idx == 0) snprintf(name, sizeof name, "wfav");
            else snprintf(name, sizeof name, "w%d", S.fr_list[idx - S.fr_fav]);
            int focused = idx == S.fr_index;
            if (focused) img(W_TGLOW, "tileglow", X(x - 20), Y(y - 20), 230 * K, 140 * K, 1.0f, 0);
            img(W_TILE0 + slot, name, X(x - 6), Y(y - 6), 202 * K, 112 * K, 1.0f, 0);
            img(W_TBORD0 + slot, focused ? "tilesel" : "tileline", X(x - 6), Y(y - 6), 202 * K, 112 * K, 1.0f, 0);
        }
    draw_scrollbar(128, 128 + 3 * 108 + 100, (total + FCOLS - 1) / FCOLS, FVIS, S.fr_top);
}

static void name_label(int id, const char *text, float x, float y, float w, int focused) {
    label(id, text, x, y, w, 22, 1, 2, focused ? COL_NAME_FOCUS : COL_NAME, 1.0f);
}

static void draw_roster(void) {
    /* header: world logo, or the sort badge for the Story / Favorites rosters */
    if (S.r_source >= 0) {
        char name[16];
        snprintf(name, sizeof name, "wl%d", S.r_source);
        img_fit(W_RLOGO, name, X(450 - 180), Y(24), 360 * K, 62 * K, 1, 1, 1.0f, 0);
    } else if (S.r_source == -1) img_fit(W_RLOGO, "favlogo", X(450 - 45), Y(24), 90 * K, 66 * K, 1, 1, 1.0f, 0);
    else draw_sort_badge();
    if (!S.r_n) {
        label(W_TITLE, S.r_source == -1 ? "No favorites yet - press Triangle on a figure to add it."
                                       : "No figures in this world.", X(24), Y(200), 852 * K, 30, 1, 2, COL_TEXT, 1.0f);
        return;
    }
    /* metrics: normal / compact / extra compact (kRosterNormalMetrics ...) */
    static const struct { int pitchY, diam, labelH, sepGap, vehGap; } M[3] = {
        {160, 94, 60, 16, 30}, {148, 84, 56, 12, 24}, {134, 72, 46, 8, 16}};
    int rows = r_rows(), cr = r_char_rows(), paint = rows - S.r_top < RVIS ? rows - S.r_top : RVIS;
    int sep_visible = S.r_chars > 0 && S.r_chars < S.r_n && cr > S.r_top && cr < S.r_top + RVIS;
    int mi = 0;
    float sepY = 0, vehOff = 0, bottom = 0;
    for (; mi < 3; mi++) {
        sepY = 120 + (cr - 1 - S.r_top) * M[mi].pitchY + M[mi].diam + 8 + 48 + M[mi].sepGap;
        vehOff = 0;
        if (sep_visible) {
            float base = 120 + (cr - S.r_top) * M[mi].pitchY;
            float minY = sepY + 6 + M[mi].vehGap;
            vehOff = fmaxf(0, minY - base);
        }
        int last = S.r_top + paint - 1;
        bottom = 120 + (paint - 1) * M[mi].pitchY + ((last >= cr && cr > S.r_top) ? vehOff : 0) + M[mi].diam + M[mi].labelH;
        if (!sep_visible || bottom <= 610 - 28 || mi == 2) break;
    }
    float shift = fminf(60, fmaxf(0, (610 - 28 - 120) - (bottom - 120)) / 2);
    /* panel behind the grid */
    float px = 60 - 16, py = 120 + shift - 26;
    img(W_RPANEL, "panel", X(px), Y(py), (60 + 5 * 156 + 16 - px) * K, (bottom + shift + 26 - py) * K, 1.0f, 0);
    if (sep_visible) img(W_SEP, "sep", X(60 + (5 * 156 - 180) / 2.0f), Y(sepY + shift), 180 * K, 6 * K, 1.0f, 0);
    int focus_drawn = 0;
    for (int row = 0; row < paint; row++)
        for (int col = 0; col < RCOLS; col++) {
            int vrow = S.r_top + row;
            if (col >= r_row_count(vrow)) break;
            int idx = r_index_at(vrow, col), cell = row * RCOLS + col;
            int e = S.roster[idx].entry;
            float x = 60 + r_offset(vrow) + col * 156;
            float y = 120 + shift + row * M[mi].pitchY + ((vrow >= cr && cr > S.r_top) ? vehOff : 0);
            float d = M[mi].diam;
            float cx = X(x + 78), cy = Y(y + d / 2);
            portrait(W_RPORT0 + cell, e, cx, cy, d * K, 1.0f);
            if (idx == S.r_index) { portrait_focus(W_RFOCUS, e, cx, cy, d * K); focus_drawn = 1; }
            if (vtp_is_favorite(E[e].bin)) img(W_RSTAR0 + cell, "star", cx + d * K / 2 - 18, cy - d * K / 2 - 4, 26 * K, 26 * K, 1.0f, 0);
            const char *nm = S.roster[idx].group >= 0 && S.r_source != -1 ? G[S.roster[idx].group].base : E[e].name;
            name_label(W_RNAME0 + cell, nm, X(x + 12), Y(y + d + 8), 132 * K, idx == S.r_index);
        }
    (void)focus_drawn;
    draw_scrollbar(120 + shift, bottom + shift, rows, RVIS, S.r_top);
}

static void draw_plus(void) {
    if (S.r_source >= 0) {
        char name[16];
        snprintf(name, sizeof name, "wl%d", S.r_source);
        img_fit(W_RLOGO, name, X(450 - 180), Y(24), 360 * K, 62 * K, 1, 1, 1.0f, 0);
    }
    group_t *g = &G[S.plus_group];
    int n = g->count < 8 ? g->count : 8;
    float step = 156, diam = 90, circleY = 160, labelH = 60;
    float roww = n * step, pw = roww + 40, pxl = (900 - pw) / 2, py = circleY - 20;
    img(W_PPANEL, "panel", X(pxl), Y(py), pw * K, (20 + diam + 8 + labelH + 16) * K, 1.0f, 0);
    for (int i = 0; i < n; i++) {
        int e = g->first + i;
        float cell = pxl + 20 + i * step, cx = X(cell + step / 2), cy = Y(circleY + diam / 2);
        portrait(W_PPORT0 + i, e, cx, cy, diam * K, 1.0f);
        if (i == S.plus_index) portrait_focus(W_PFOCUS, e, cx, cy, diam * K);
        if (vtp_is_favorite(E[e].bin)) img(W_PSTAR0 + i, "star", cx + diam * K / 2 - 18, cy - diam * K / 2 - 4, 26 * K, 26 * K, 1.0f, 0);
        name_label(W_PNAME0 + i, E[e].name, X(cell + 6), Y(circleY + diam + 8), (step - 12) * K, i == S.plus_index);
    }
}

/* Search: query box in the title band, a two-row result grid, the keyboard below */
static void draw_search(void) {
    static const uint8_t grey[4] = {150, 160, 176, 255}, white[4] = {255, 255, 255, 255}, muted[4] = {170, 200, 230, 255};
    float bx = 190, by = 30, bw = 520, bh = 46;
    img(W_QBOX, "qbox", X(bx), Y(by), bw * K, bh * K, 1.0f, 0);
    char q[48];
    float ty = Y(by + bh / 2) - FS(30) * 0.62f;
    if (S.query[0]) {
        snprintf(q, sizeof q, "%s%s", S.query, S.s_zone == 0 && (ms_now() / 500) % 2 ? "_" : "");
        label(W_QTEXT, q, X(bx + 16), ty, (bw - 150) * K, 30, 0, 1, white, 1.0f);
        char cnt[24];
        snprintf(cnt, sizeof cnt, "%d found", S.s_n);
        label(W_QCOUNT, cnt, X(bx + bw - 150), ty + FS(4), 134 * K, 24, 2, 1, muted, 1.0f);
    } else {
        snprintf(q, sizeof q, "Search %d figures", NE);
        label(W_QTEXT, q, X(bx + 16), ty, (bw - 32) * K, 30, 0, 1, grey, 1.0f);
    }
    /* results */
    float px = 44, py = 92, ph = 282;
    img(W_RPANEL, "panel", X(px), Y(py), (900 - 2 * px) * K, ph * K, 1.0f, 0);
    if (!S.s_n) {
        label(W_TITLE, S.query[0] ? "Nothing matches - try fewer letters." : "Type a figure, vehicle or world name below.",
              X(px), Y(py + ph / 2) - FS(28) * 0.6f, (900 - 2 * px) * K, 28, 1, 1, COL_TEXT, 1.0f);
    }
    const float d = 84, pitchx = 156, pitchy = 132, top = py + 18;
    for (int row = 0; row < SVIS; row++)
        for (int col = 0; col < SCOLS; col++) {
            int idx = (S.s_top + row) * SCOLS + col, cell = row * SCOLS + col;
            if (idx >= S.s_n) break;
            int e = S.s_res[idx];
            float x = 60 + col * pitchx, y = top + row * pitchy;
            float cx = X(x + 78), cy = Y(y + d / 2);
            portrait(W_RPORT0 + cell, e, cx, cy, d * K, 1.0f);
            if (S.s_zone == 1 && idx == S.s_index) portrait_focus(W_RFOCUS, e, cx, cy, d * K);
            if (vtp_is_favorite(E[e].bin)) img(W_RSTAR0 + cell, "star", cx + d * K / 2 - 18, cy - d * K / 2 - 4, 26 * K, 26 * K, 1.0f, 0);
            name_label(W_RNAME0 + cell, E[e].name, X(x + 8), Y(y + d + 4), 140 * K, S.s_zone == 1 && idx == S.s_index);
        }
    int rows = (S.s_n + SCOLS - 1) / SCOLS;
    if (rows > SVIS) draw_scrollbar(py + 10, py + ph - 10, rows, SVIS, S.s_top);
    /* keyboard */
    const float kw = 52, kh = 40, gap = 6, kx = (900 - (10 * (kw + gap) - gap)) / 2, ky = 386;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 10; c++) {
            int i = r * 10 + c, focus = S.s_zone == 0 && r == S.k_row && c == S.k_col;
            float x = kx + c * (kw + gap), y = ky + r * (kh + 6);
            img(W_KEY0 + i, focus ? "keysel" : "key", X(x), Y(y), kw * K, kh * K, 1.0f, 0);
            int small = strlen(KEYS[r][c]) > 1, f = small ? 19 : 26;
            label(W_KEYLAB0 + i, KEYS[r][c], X(x), Y(y + kh / 2) - FS((float)f) * 0.62f, kw * K, f, 1, 1,
                  focus ? white : COL_TEXT, 1.0f);
        }
}

/* Settings list (Paint, Screen::Settings): one panel, heading rows centred in gold with a rule
 * either side, the focused row on a blue gradient, values coloured by tone */
static void draw_settings(void) {
    srow_t r[SET_MAX];
    int n = build_settings(r);
    settings_scroll(n);
    static const uint8_t gold[4] = {255, 204, 51, 255}, white[4] = {255, 255, 255, 255},
                         text[4] = {228, 232, 238, 255}, tones[3][4] = {{255, 214, 140, 255}, {126, 226, 142, 255},
                                                                        {255, 108, 108, 255}};
    const float px = 30, pw = 840, pad = 18;
    int vis = n - S.set_top < SET_VISIBLE ? n - S.set_top : SET_VISIBLE;
    img(W_SPANEL, "spanel", X(px), Y(SET_TOP - pad), pw * K, (vis * SET_PITCH + 2 * pad) * K, 1.0f, 0);
    const int font = 30;
    int rules = 0;
    for (int v = 0; v < vis && v < 12; v++) {
        int i = S.set_top + v;
        float y = SET_TOP + v * SET_PITCH, ty = Y(y) + (SET_PITCH * K - font) / 2 - 2;
        if (r[i].action == SA_HEADING) {
            label(W_SLAB0 + v, r[i].label, X(px), ty, pw * K, font, 1, 1, gold, 1.0f);
            float half = strlen(r[i].label) * font * 0.58f / 2 / K + 14, centre = px + pw / 2, ry = y + SET_PITCH / 2.0f - 1;
            if (rules < 6) img(W_SRULE0 + rules++, "rule", X(px + 22), Y(ry), (centre - half - px - 22) * K, 2 * K, 1.0f, 0);
            if (rules < 6) img(W_SRULE0 + rules++, "rule", X(centre + half), Y(ry), (px + pw - 22 - centre - half) * K, 2 * K, 1.0f, 0);
            continue;
        }
        int sel = i == S.set_index;
        if (sel) img(W_SROW, "srow", X(px + 8), Y(y), (pw - 16) * K, (SET_PITCH - 2) * K, 1.0f, 0);
        char lab[64];
        snprintf(lab, sizeof lab, "%s%s", r[i].label, r[i].value[0] ? ":" : "");
        label(W_SLAB0 + v, lab, X(px + 24), ty, (pw / 2 - 24) * K, font, 0, 1, sel ? white : text, 1.0f);
        if (r[i].value[0])
            label(W_SVAL0 + v, r[i].value, X(px + pw / 2), ty, (pw / 2 - 24) * K, font, 2, 1, tones[r[i].tone % 3], 1.0f);
    }
    if (n > SET_VISIBLE) draw_scrollbar(SET_TOP, SET_TOP + vis * SET_PITCH, n, SET_VISIBLE, S.set_top);
}

/* "[Y] settings" in the pad screen's bottom-right corner, as in LegoToypad */
static void draw_settings_hint(void) {
    const float icon = 34, th = 30, tw = tex_aspect("settext", 234.0f / 105.0f) * th, gap = 6, cy = HINT_Y;
    float x = 900 - 20 - (icon + gap + tw);
    img_fit(W_SETICON, "g_Y", X(x), Y(cy - icon / 2), icon * K, icon * K, 1, 1, 1.0f, 0);
    img(W_SETTEXT, "settext", X(x + icon + gap), Y(cy - th / 2), tw * K, th * K, 1.0f, 0);
}

static void draw_overlay(void) {
    char bgname[16];
    if (!bg_clear()) img(W_BG, bg_texture(bgname, sizeof bgname), X(0), Y(0), 900 * K, 610 * K, 1.0f, 0);
    /* wordmark: LegoToypad fits it in a 460x90 box at (20,20); our texture is cropped to the art */
    img_fit(W_WORDMARK, "wordmark", X(20), Y(20 + 14), 460 * K, 61 * K, 0, 1, 1.0f, 0);
    img_fit(W_BYMARK, "bymark", X(900 - 20 - 300), Y(20 + 30 - 6), 300 * K, 30 * K, 2, 1, 1.0f, 0);
    img_fit(W_PORTMARK, "portmark", X(900 - 20 - 300), Y(20 + 30 + 27), 300 * K, 19 * K, 2, 1, 0.9f, 0);
    switch (S.screen) {
    case SCR_PAD:
    case SCR_ACTION: draw_pad_screen(); break;
    case SCR_FRANCHISE: draw_franchise(); break;
    case SCR_ROSTER: draw_roster(); break;
    case SCR_PLUS: draw_plus(); break;
    case SCR_SETTINGS: draw_settings(); break;
    case SCR_SEARCH: draw_search(); break;
    }
    if (S.screen == SCR_PAD && !S.moving) draw_settings_hint();
    draw_hints();
    draw_toast(1.0f);
    /* Window transparency: the whole picker fades together (LegoToypad's layered-window alpha) */
    if (C.opacity < 100) {
        float o = (C.opacity < 20 ? 20 : C.opacity) / 100.0f;
        for (int i = 0; i < W_COUNT; i++) if (g_cur[i].vis) g_cur[i].alpha *= o;
    }
}

static float ease_out_cubic(float t) { t = clampf(t, 0, 1); return 1 - (1 - t) * (1 - t) * (1 - t); }

static void draw_peek(void) {
    uint64_t now = ms_now();
    float dur = S.peek_hiding ? 130.0f : 90.0f;
    float t = ease_out_cubic((now - S.peek_t) / dur);
    float a = S.peek_hiding ? (1 - t) * S.peek_shown : S.peek_shown + (1 - S.peek_shown) * t;
    if (a <= 0.01f) return;
    rectf cells[7];
    float s = peek_cells(cells);
    draw_pads(cells, s, 1, a, -1);
}

/* ------------------------------------------------------------------ main loop */

static void layout_once(void) {
    apply_scale();
    frame_begin();
    if (S.open) draw_overlay();
    else if (S.peek_held || (S.peek_hiding && ms_now() - S.peek_t < 140)) draw_peek();
    else if (S.status[0] && ms_now() - S.status_t < 3000 && !S.open) {
        /* a result that lands after the picker closed (fast loading) still shows its toast */
        draw_toast(1.0f);
    }
    frame_send();
}

static void peek_tick(const vtp_ext *e) {
    int held = C.peek_size > 0 && e->valid && !S.open && C.peek_mask && (e->raw & C.peek_mask) == C.peek_mask;
    uint64_t now = ms_now();
    if (held && !S.peek_held) {
        float dur = S.peek_hiding ? 130.0f : 90.0f;
        float cur = S.peek_hiding ? (1 - ease_out_cubic((now - S.peek_t) / dur)) * S.peek_shown : 0.0f;
        S.peek_shown = cur; S.peek_t = now; S.peek_hiding = 0; S.peek_held = 1;
    } else if (!held && S.peek_held) {
        float t = ease_out_cubic((now - S.peek_t) / 90.0f);
        S.peek_shown = S.peek_shown + (1 - S.peek_shown) * t;
        S.peek_t = now; S.peek_hiding = 1; S.peek_held = 0;
    }
}

static void *cui_thread(void *arg) {
    (void)arg;
    vtp_ext_request(0, 0);                 /* classic menu until the overlay is really up */
    while (load_catalog()) {
        static int warned;
        if (!warned++) LOGC("no %s/catalog.tsv - copy the new web folder to use the console overlay", UI_DIR);
        sleep(5);
    }
    probe_appearance();
    S.sort = vtp_sort_get();
    if (S.sort == 6) S.sort = SORT_DEFAULT;
    S.slot = 1;
    int mode_on = 0;
    uint64_t last_frame = 0;
    for (;;) {
        uint64_t now = ms_now();
        vtp_ext e;
        vtp_ext_snapshot(&e);
        S.linked = vtp_pad(S.used, S.bin, S.fid) == 0;

        if (g_fd < 0 && now >= g_next_connect) {
            g_next_connect = now + 2000;
#ifndef CUI_HOST_TEST
            if (ovl_ctl_online())
#endif
                if (!link_open()) g_last_ping = now;
        }
        if (g_fd >= 0) {
            link_poll();
            if (g_fd >= 0 && now - g_last_ping > 1000) { g_last_ping = now; send_msg2(OVL_PING, 0, 0, 0, 0); }
        }
        /* hand the controller to the overlay only while it is connected */
        int want = g_fd >= 0 && e.valid;
        if (want != mode_on || (e.valid && e.mode != (uint32_t)want && now - last_frame > 500)) {
            vtp_ext_request(want, 0);
            if (want != mode_on) LOGC("console overlay %s", want ? "active (L3+R3 opens the LegoToypad picker)" : "inactive, classic menu");
            mode_on = want;
        }
        if (g_fd >= 0) {
            g_tex_budget = 6;
            input_tick(&e);
            peek_tick(&e);
            act_collect();
            int animating = S.open || S.peek_held || S.peek_hiding || vtp_leds_animating() ||
                            (S.status[0] && now - S.status_t < 3200);
            if (animating || now - last_frame > 250) { layout_once(); last_frame = now; }
            tex_evict();
        } else if (S.open) {
            S.open = 0;
        }
        usleep(16 * 1000);
    }
    return 0;
}

int cui_start(void) {
    if (!C.enabled) { LOGC("disabled in config.ini (overlay=0)"); return -1; }
    define_types();
    pthread_t t, a;
    if (pthread_create(&a, 0, action_thread, 0)) return -1;
    return pthread_create(&t, 0, cui_thread, 0);
}
