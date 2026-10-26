/* In-game Toy Pad menu: opened with a configurable button combo (default L3+R3).
 * Drawn with the game's own debug-text printer (still present in the retail eboot).
 * Input is taken from scePadReadState and hidden from the game while the menu is open. */
#include "ldhook.h"
#include "figures.h"
#include "grid_nav.h"
#include "grid_input.h"

extern ld_header HDR;
void ld_lock(void);
void ld_unlock(void);

enum {
    B_L3 = 0x2, B_R3 = 0x4, B_OPTIONS = 0x8, B_UP = 0x10, B_RIGHT = 0x20, B_DOWN = 0x40,
    B_LEFT = 0x80, B_L2 = 0x100, B_R2 = 0x200, B_L1 = 0x400, B_R1 = 0x800,
    B_TRIANGLE = 0x1000, B_CIRCLE = 0x2000, B_CROSS = 0x4000, B_SQUARE = 0x8000,
};

enum { M_CLOSED, M_PAD, M_LIST, M_GRID };
enum { T_MINE, T_CHARS, T_VEHICLES, T_COUNT };
/* no pointer tables: the blob must stay free of absolute relocations */
static const char *tab_name(int t) { return t == T_MINE ? "MY FIGURES" : t == T_CHARS ? "CHARACTERS" : "VEHICLES / GADGETS"; }

static int mode = M_CLOSED;
static int slot_cur;          /* 0..6 cursor on the pad view (view order) */
static int moving = -1;       /* slot being moved, -1 none */
static int tab = T_MINE;
static int world_filter = -1; /* index into worlds[] or -1 = all */
static int list_cur[T_COUNT], list_top[T_COUNT];
static uint32_t prev_btn, repeat_frames, square_frames;
/* per-controller combo tracking (handles of up to 4 players) */
static int32_t  h_id[4] = {-1, -1, -1, -1};
static uint32_t h_hold[4];
static int32_t  owner = -1;
static int      combo_armed;  /* combo must be released before it can close the menu */
static uint32_t frame;
static char msg[64];
static uint32_t msg_frames;
static uint16_t use_stamp;    /* recency counter for "My figures" */

/* slot order shown in the menu: left 1-3, center, right 1-3 (matches the physical pad) */
static const int8_t VIEW_SLOT[7] = {1, 2, 3, 0, 4, 5, 6};
static int view_of_slot(int s) { for (int v = 0; v < 7; v++) if (VIEW_SLOT[v] == s) return v; return 0; }

/* ------------------------------------------------------------ strings */

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static char *scat(char *d, const char *s) { while (*s) *d++ = *s++; *d = 0; return d; }
static char *sint(char *d, int v) {
    char t[12]; int n = 0;
    if (v < 0) { *d++ = '-'; v = -v; }
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *d++ = t[--n];
    *d = 0;
    return d;
}
static char *spad(char *d, char *start, int width) { while (d - start < width) *d++ = ' '; *d = 0; return d; }
static int seq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static char upper(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

static const ld_figinfo *fig_by_id(uint32_t id) {
    for (int i = 0; i < LD_NUM_FIGS; i++) if (LD_FIGS[i].id == id) return &LD_FIGS[i];
    return 0;
}
static const char *fig_name(uint32_t id) {
    const ld_figinfo *f = fig_by_id(id);
    return f ? LD_FIGNAMES + f->name : "Unknown";
}

static void toast(const char *s) {
    char *d = msg; *d = 0;
    scat(d, s);
    msg_frames = 90;
}

/* ------------------------------------------------------------ lists */

static uint16_t worlds[48];   /* name offsets of distinct franchises */
static int nworlds = -1;
static uint16_t items[LD_NUM_FIGS];
static int nitems;

static void build_worlds(void) {
    nworlds = 0;
    for (int i = 0; i < LD_NUM_FIGS && nworlds < 48; i++) {
        const char *w = LD_FIGNAMES + LD_FIGS[i].world;
        if (!*w) continue;
        int dup = 0;
        for (int k = 0; k < nworlds; k++) if (seq(LD_FIGNAMES + worlds[k], w)) { dup = 1; break; }
        if (!dup) worlds[nworlds++] = LD_FIGS[i].world;
    }
}

static int lib_index_of(uint32_t id) {
    for (int i = 0; i < LD_LIB_MAX; i++) if (HDR.lib[i].used && HDR.lib[i].id == id) return i;
    return -1;
}

static void build_list(void) {
    if (nworlds < 0) build_worlds();
    nitems = 0;
    if (tab == T_MINE) {
        /* every figure ever used (saved tags), most recent first */
        static int16_t order[LD_LIB_MAX];
        int n = 0;
        for (int i = 0; i < LD_LIB_MAX; i++) if (HDR.lib[i].used && fig_by_id(HDR.lib[i].id)) order[n++] = (int16_t)i;
        for (int a = 1; a < n; a++) {   /* insertion sort by stamp, newest first */
            int16_t x = order[a]; int b = a - 1;
            uint16_t sx = *(uint16_t *)HDR.lib[x].pad_;
            while (b >= 0 && *(uint16_t *)HDR.lib[order[b]].pad_ < sx) { order[b + 1] = order[b]; b--; }
            order[b + 1] = x;
        }
        for (int i = 0; i < n; i++) items[nitems++] = (uint16_t)(fig_by_id(HDR.lib[order[i]].id) - LD_FIGS);
    } else {
        int want_kind = tab == T_CHARS ? 0 : 1;
        for (int i = 0; i < LD_NUM_FIGS; i++) {
            if (LD_FIGS[i].kind != want_kind) continue;
            if (world_filter >= 0 && !seq(LD_FIGNAMES + LD_FIGS[i].world, LD_FIGNAMES + worlds[world_filter])) continue;
            items[nitems++] = (uint16_t)i;
        }
    }
    if (list_cur[tab] >= nitems) list_cur[tab] = nitems ? nitems - 1 : 0;
}

/* ------------------------------------------------------------ portrait grid data */

/* Built from the game's own collection (MechIconTextureHolder+0xa8): entries of 0x38 bytes,
 * +0 = internal character id (icon key), +8 = portal model id (the Toy Pad tag id). */
#define GRID_MAX 400
static uint16_t g_cid[2][GRID_MAX];   /* [0] characters, [1] vehicles/gadgets */
static uint16_t g_tag[2][GRID_MAX];
/* Smooth, per-portrait scale transitions like the original LEGO selector. */
static float g_zoom[2][GRID_MAX];
static int g_n[2] = {-1, -1};
static int g_tab, g_cur[2];
static int border_calibrating;

static void add_collection(const uint8_t *vecp) {
    const uint8_t *vec = *(const uint8_t *const *)vecp;
    uint32_t cnt = *(const uint32_t *)(vecp + 0xc);
    if (!vec || cnt > 2000) return;
    for (uint32_t i = 0; i < cnt; i++) {
        const uint8_t *e = vec + i * 0x38;
        int16_t cid = *(const int16_t *)e;
        int32_t tag = *(const int32_t *)(e + 8);
        if (cid < 0 || tag <= 0 || tag > 0xFFFF || !fig_by_id((uint32_t)tag)) continue;
        int t = tag >= 1000;
        int dup = 0;
        for (int k = 0; k < g_n[t]; k++) if (g_tag[t][k] == (uint16_t)tag) { dup = 1; break; }
        if (!dup && g_n[t] < GRID_MAX) { g_cid[t][g_n[t]] = (uint16_t)cid; g_tag[t][g_n[t]] = (uint16_t)tag; g_n[t]++; }
    }
}

static void build_grid(void) {
    g_n[0] = g_n[1] = 0;
    if (!HDR.fn.icon_holder) return;
    const uint8_t *holder = *(const uint8_t **)HDR.fn.icon_holder;
    if (!holder) return;
    add_collection(holder + 0xa8);                       /* base game */
    const uint8_t *node = *(const uint8_t *const *)(holder + 0xb8);   /* DLC packs: linked list */
    for (int guard = 0; node && guard < 64; guard++) {
        add_collection(node + 0x18);
        node = *(const uint8_t *const *)(node + 8);
    }
    /* The contents can change when a DLC is loaded after the first visit. */
    for (int t = 0; t < 2; t++)
        if (g_cur[t] >= g_n[t]) g_cur[t] = g_n[t] > 0 ? g_n[t] - 1 : 0;
}

/* ------------------------------------------------------------ drawing */

/* Text goes through the game's debug line printer (0x5a79b0), called from inside the frame's
 * render pass (h_RenderHook). Coordinates are normalised: x -1..1 left->right, y +1 top.
 * Colours are 0xAABBGGRR. */
typedef void (*lineprint_fn)(void *ctx, uint32_t argb, const char *fmt, ...);
typedef struct { float y, x, scale; int32_t line; } line_ctx;
static line_ctx LC;
#define C_TITLE 0xFFFFD040u
#define C_SEL   0xFF40E8FFu
#define C_TEXT  0xFFFFFFFFu
#define C_DIM   0xFFA09090u
#define C_HINT  0xFFC0C0C0u
#define C_MSG   0xFF80FF80u

static void line_color(const char *s, uint32_t abgr) {
    ((lineprint_fn)HDR.fn.line_print)(&LC, abgr, "%s", s);
}

static void slot_label(char *d, int slot) {
    char *s = d;
    if (slot == 0) d = scat(d, "Center   ");
    else if (slot <= 3) { d = scat(d, "Left   "); d = sint(d, slot); }
    else { d = scat(d, "Right  "); d = sint(d, slot - 3); }
    spad(d, s, 9);
}

static void set_menu_line(const char *s) {
    char *d = HDR.menu_line; int i = 0;
    while (s[i] && i < 95) { d[i] = s[i]; i++; }
    d[i] = 0;
    HDR.state_seq++;
}

static void draw(void) {
    char buf[128], *d;
    LC.y = 0.86f; LC.x = -0.88f; LC.scale = 0.62f; LC.line = 0;
    if (mode == M_PAD) {
        line_color("VIRTUAL TOY PAD - By HeitorSpectre", C_TITLE);
        line_color("", C_TEXT);
        for (int v = 0; v < 7; v++) {
            int s = VIEW_SLOT[v];
            d = buf; d = scat(d, v == slot_cur ? " > " : "   ");
            slot_label(d, s); d += slen(d);
            d = scat(d, ": ");
            ld_figure *f = &HDR.pad.fig[s];
            if (f->used) d = scat(d, fig_name(f->id)); else d = scat(d, "(empty)");
            if (moving == s) d = scat(d, "   <moving>");
            line_color(buf, v == slot_cur ? C_SEL : (f->used ? C_TEXT : C_DIM));
        }
        line_color("", C_TEXT);
        if (moving >= 0) line_color("CROSS: drop here   CIRCLE: cancel", C_HINT);
        else {
            line_color("CROSS: choose   SQUARE: remove (hold: clear all)   CIRCLE: move", C_HINT);
            line_color("Send selected:  L1 = left   R1 = right   L2/R2 = center (swaps if full)", C_HINT);
            line_color("TRIANGLE / OPTIONS: close", C_HINT);
        }
    } else if (mode == M_LIST) {
        d = buf;
        for (int t = 0; t < T_COUNT; t++) {
            d = scat(d, t == tab ? "[" : " ");
            d = scat(d, tab_name(t));
            d = scat(d, t == tab ? "] " : "  ");
        }
        line_color(buf, C_TITLE);
        d = buf; d = scat(d, "Target: "); slot_label(d, VIEW_SLOT[slot_cur]); d += slen(d);
        if (tab != T_MINE) {
            d = scat(d, "   Franchise: ");
            d = scat(d, world_filter < 0 ? "All" : LD_FIGNAMES + worlds[world_filter]);
        }
        d = scat(d, "   ("); d = sint(d, nitems ? list_cur[tab] + 1 : 0); d = scat(d, "/"); d = sint(d, nitems); d = scat(d, ")");
        line_color(buf, C_TEXT);
        line_color("", C_TEXT);
        int rows = 14;
        if (list_cur[tab] < list_top[tab]) list_top[tab] = list_cur[tab];
        if (list_cur[tab] >= list_top[tab] + rows) list_top[tab] = list_cur[tab] - rows + 1;
        if (!nitems) line_color(tab == T_MINE ? "   (nothing yet - figures you place show up here)" : "   (empty)", C_DIM);
        for (int r = 0; r < rows; r++) {
            int i = list_top[tab] + r;
            if (i >= nitems) { if (r || nitems) line_color("", C_TEXT); continue; }
            const ld_figinfo *f = &LD_FIGS[items[i]];
            d = buf; d = scat(d, i == list_cur[tab] ? " > " : "   ");
            if (f->rebuild && tab != T_MINE) d = scat(d, "   ");
            d = scat(d, LD_FIGNAMES + f->name);
            if (LD_FIGNAMES[f->world] && (tab == T_MINE || world_filter < 0)) { d = scat(d, "  - "); d = scat(d, LD_FIGNAMES + f->world); }
            line_color(buf, i == list_cur[tab] ? C_SEL : C_TEXT);
        }
        line_color("", C_TEXT);
        line_color("CROSS: place   CIRCLE/TRIANGLE: back   L1/R1: tab", C_HINT);
        line_color(tab == T_MINE ? "LEFT/RIGHT: page" : "L2/R2: franchise   LEFT/RIGHT: next letter", C_HINT);
    }
    if (mode == M_GRID) {
        int t = g_tab, n = g_n[t];
        /* Caption aligned with the picker rather than cropped at the left edge. */
        LC.x = -0.58f; LC.y = -0.37f; LC.scale = 0.53f; LC.line = 0;
        d = buf; d = scat(d, t == 0 ? "[CHARACTERS]  VEHICLES" : " CHARACTERS  [VEHICLES]");
        d = scat(d, "    Target: "); slot_label(d, VIEW_SLOT[slot_cur]);
        int cols = HDR.grid.cols ? (int)HDR.grid.cols : 24, rows = HDR.grid.rows ? (int)HDR.grid.rows : 4;
        int per = cols * rows, pages = n > 0 ? (n + per - 1) / per : 1, pg = n > 0 ? g_cur[t] / per : 0;
        if (pages > 1) {
            d = scat(d, "    ");
            d = scat(d, pg > 0 ? "<  " : "   ");
            d = scat(d, "Page "); d = sint(d, pg + 1); d = scat(d, "/"); d = sint(d, pages);
            d = scat(d, pg + 1 < pages ? "  >" : "");
        }
        line_color(buf, C_TITLE);
        if (n > 0) {
            d = buf; d = scat(d, "> "); d = scat(d, fig_name(g_tag[t][g_cur[t]]));
            const ld_figinfo *f = fig_by_id(g_tag[t][g_cur[t]]);
            if (f && LD_FIGNAMES[f->world]) { d = scat(d, "  - "); scat(d, LD_FIGNAMES + f->world); }
            line_color(buf, C_SEL);
        }
        if (border_calibrating) {
            line_color("SQ+ARROWS: move ring   L2/R2: ring size   TRIANGLE: rings off/one/all", C_HINT);
        } else {
            line_color("X: select  O: back  L1/R1: type  L2/R2: page  SQ: align rings", C_HINT);
        }
    }
    if (msg_frames) { msg_frames--; line_color("", C_TEXT); line_color(msg, C_MSG); }
}

/* ------------------------------------------------------------ DrawCharacterIcon capture */


uint64_t ld_dci_ret, ld_dci_resume;
static int in_grid;

void ld_dci_record(void *widget, uint32_t char_id, const float *mat, uint32_t mode, uint32_t a, uint32_t b, float alpha) {
    ld_dci_resume = HDR.fn.dci_resume;   /* where the stub continues */
    if (in_grid) return;   /* our own grid calls are not recorded */
    uint32_t n = HDR.dci_n;
    /* keep one entry per call site + character, refresh it every frame */
    uint32_t slot = n < LD_DCI_MAX ? n : LD_DCI_MAX;
    for (uint32_t i = 0; i < (n < LD_DCI_MAX ? n : LD_DCI_MAX); i++)
        if (HDR.dci[i].ret == ld_dci_ret && HDR.dci[i].char_id == char_id) { slot = i; break; }
    if (slot == LD_DCI_MAX) return;
    ld_dcicap *c = &HDR.dci[slot];
    c->ret = ld_dci_ret; c->widget = (uint64_t)widget; c->char_id = char_id;
    c->mode = mode; c->a = a; c->b = b; c->alpha = alpha;
    if (mat) for (int i = 0; i < 16; i++) c->mat[i] = mat[i];
    if (slot == n) HDR.dci_n = n + 1;
}

/* inline hook: the first 6 bytes of DrawCharacterIcon (push rbp; mov rbp,rsp; push r15) jump here */
__asm__(
    ".text\n"
    ".intel_syntax noprefix\n"
    ".globl h_DCICapture\n"
    ".hidden h_DCICapture\n"
    "h_DCICapture:\n"
    "  mov rax, [rsp]\n"
    "  mov [rip + ld_dci_ret], rax\n"
    "  push rdi\n  push rsi\n  push rdx\n  push rcx\n  push r8\n  push r9\n"
    "  sub rsp, 0x18\n"
    "  movss [rsp], xmm0\n"
    "  call ld_dci_record\n"
    "  movss xmm0, [rsp]\n"
    "  add rsp, 0x18\n"
    "  pop r9\n  pop r8\n  pop rcx\n  pop rdx\n  pop rsi\n  pop rdi\n"
    "  push rbp\n"
    "  mov rbp, rsp\n"
    "  push r15\n"
    "  jmp qword ptr [rip + ld_dci_resume]\n"
    ".att_syntax prefix\n");

/* ------------------------------------------------------------ portrait grid drawing */

typedef void (*dci_fn2)(void *widget, uint32_t char_id, float *mat, uint32_t mode, uint32_t a, uint32_t b, float alpha);

/* Drawn every frame from the render hook, in a plane just in front of the active camera, so it
 * behaves like a HUD overlay. Uses the game's own DrawCharacterIcon (official portraits). */
static void draw_grid(void) {
    ld_gridcfg *g = &HDR.grid;
    if (!HDR.fn.draw_char_icon || !HDR.fn.icon_widget || !HDR.fn.cam_world) return;
    void *widget = *(void **)HDR.fn.icon_widget;
    if (!widget) return;
    if (g_n[0] < 0) build_grid();
    int t = g_tab, n = g_n[t];
    int cols = g->cols ? (int)g->cols : 12, rows = g->rows ? (int)g->rows : 5;
    if (n <= 0) { g->drawn = 0; return; }
    const float *cm = (const float *)HDR.fn.cam_world;   /* row0 right, row1 up, row2 forward, row3 pos */
    int cur = g_cur[t];
    int page = cur / (cols * rows);
    int top_row[2]; top_row[t] = page * rows;
    uint32_t drawn = 0;
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) {
            int i = (top_row[t] + r) * cols + c;
            if (i >= n) break;
            int sel = i == cur;
            float target = sel ? g->sel_scale : 1.0f;
            float zoom = g_zoom[t][i];
            if (zoom < 0.7f || zoom > 2.0f) zoom = 1.0f;
            zoom += (target - zoom) * 0.28f;
            g_zoom[t][i] = zoom;
            float k = g->scale * zoom;  /* equal scaling on all axes */
            float x = g->ox + g->dx * (float)c, y = g->oy - g->dy * (float)r;
            if (sel) {
                /* Native border centers use the unmodified grid position.
                 * Adjust only the selected DrawCharacterIcon portrait, which
                 * otherwise cannot be independently aligned in the v17 blob.
                 * Both values are version-checked and live-tunable. */
                float sx = HDR.border.selected_icon_x;
                float sy = HDR.border.selected_icon_y;
                if (sx > -0.05f && sx < 0.05f && sy > -0.05f && sy < 0.05f) {
                    x += sx;
                    y += sy;
                }
            }
            float m[16];
            for (int q = 0; q < 3; q++) {
                m[q] = cm[q] * k;
                m[4 + q] = cm[4 + q] * k;
                m[8 + q] = cm[8 + q] * k;
                /* selected cell slightly closer to the camera so it draws on top */
                m[12 + q] = cm[12 + q] + cm[8 + q] * (g->dist - (sel ? 0.01f : 0.0f)) + cm[q] * x + cm[4 + q] * y;
            }
            m[3] = m[7] = m[11] = 0.0f; m[15] = 1.0f;
            ((dci_fn2)HDR.fn.draw_char_icon)(widget, g_cid[t][i], m,
                sel ? g->sel_mode : g->mode, sel ? g->sel_a : g->a, sel ? g->sel_b : g->b, 1.0f);
            drawn++;
        }
    g->drawn = drawn;
}

/* Draw the real Characters border resources through the retail UI sprite
 * renderer (0x1848b50), rather than drawing a replacement texture. Its native
 * callsite uses six integer registers, xmm0..xmm7 and one stack argument:
 *
 *   (0,0,0, IconDrawParams::border,0,0,
 *    icon.x, icon.y, icon.z, size, size, size, opacity, -1.0, NULL)
 *
 * The native resource wrapper is embedded in MechSelectionMenus and must be
 * looked up each frame, not copied across sessions. All the adjustments are
 * optional; the feature starts disabled in injector/main.c. */
typedef void (*ui_sprite_fn)(int, int, int, const void *, int, int,
                             float, float, float, float, float, float,
                             float, float, const void *);

static void draw_native_borders(void) {
    ld_bordercfg *b = &HDR.border;
    ld_gridcfg *g = &HDR.grid;
    b->drawn = 0;
    b->resources_ok = 0;
    b->last_error = 0;
    if (!b->enable) return;
    if (!(b->alpha >= 0.0f && b->alpha <= 1.0f)) {
        b->last_error = 5; return;
    }
    if (!HDR.fn.draw_ui_sprite || !HDR.fn.selection_menu) {
        b->last_error = 1; return;
    }
    const uint8_t *menu = *(const uint8_t *const *)HDR.fn.selection_menu;
    /* Address bounds protect against null, cleared, and obviously invalid
     * pointers. No assumptions about the console's ASLR base are made. */
    if ((uintptr_t)menu < 0x200000000ULL ||
        (uintptr_t)menu >= 0x400000000ULL) {
        b->last_error = 2; return;
    }
    /* In the original renderer, r14 = menu + 0xf0 (IconDrawParams).
     * DefaultSpecial is r14+0xbd0, first player's SelectedSpecial is
     * r14+0xcc0. These fields are 0x30-byte native resource wrappers. */
    const uint8_t *normal = menu + 0xcc0;
    const uint8_t *selected = menu + 0xdb0;
    uint64_t default_tex = *(const uint64_t *)normal;
    uint64_t selected_tex = *(const uint64_t *)selected;
    if (!selected_tex || !*(const uint64_t *)(selected + 0x18)) {
        b->last_error = 3; return;
    }
    if (b->enable >= 2 && (!default_tex || !*(const uint64_t *)(normal + 0x18))) {
        b->last_error = 4; return;
    }
    b->resources_ok = 1;
    if (g_n[0] < 0) build_grid();
    int tab_id = g_tab, count = g_n[tab_id];
    int cols = (int)g->cols, rows = (int)g->rows;
    if (cols <= 0 || cols > 32 || rows <= 0 || rows > 16 || count <= 0) return;
    int cur = g_cur[tab_id];
    int per_page = cols * rows, page = cur / per_page;
    int top_row = page * rows;
    ui_sprite_fn sprite = (ui_sprite_fn)HDR.fn.draw_ui_sprite;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int index = (top_row + r) * cols + c;
            if (index >= count || index >= GRID_MAX) break;
            int highlighted = index == cur;
            if (!highlighted && b->enable < 2) continue;
            float x = (g->ox + g->dx * (float)c) * b->x_mul + b->x_add;
            float y = (g->oy - g->dy * (float)r) * b->y_mul + b->y_add;
            float k = highlighted ? b->selected_scale : b->normal_scale;
            /* Track the portrait's existing zoom animation during selection
             * changes so the original border grows with its own icon. */
            if (highlighted && g->sel_scale > 0.5f)
                k *= g_zoom[tab_id][index] / g->sel_scale;
            /* Skip corrupt developer settings rather than pass NaN/Inf or
             * excessive scale into the game's original renderer. */
            if (!(k > 0.005f && k < 0.4f) ||
                !(x > -4.0f && x < 4.0f) || !(y > -4.0f && y < 4.0f))
                continue;
            sprite(0, 0, 0, highlighted ? selected : normal, 0, 0,
                   x, y, 0.0f, k, k, k, b->alpha, -1.0f, 0);
            b->drawn++;
        }
    }
}

/* ------------------------------------------------------------ full icon drawer capture */

uint64_t ld_icon_resume;
static uint32_t icap_seq;   /* monotonically increasing sample timestamp */
/* blk: xmm0..xmm5, then r9 r8 rcx rdx rsi rdi, return address, stack args.
 * Store one sample per original UI callsite AND character. Evict the oldest
 * sample when the diagnostic buffer fills, so opening Characters later still
 * exposes the authentic selected/normal renderer arguments. */
void ld_icon_record(const uint8_t *blk) {
    ld_icon_resume = HDR.fn.icon_resume;
    if (in_grid) return;
    uint64_t ret = *(const uint64_t *)(blk + 0x98);
    uint64_t figure = *(const uint64_t *)(blk + 0x90);  /* saved rdi */
    uint32_t n = HDR.icap_n, lim = n < LD_ICAP_MAX ? n : LD_ICAP_MAX;
    uint32_t slot = LD_ICAP_MAX;
    for (uint32_t i = 0; i < lim; i++) {
        if (*(const uint64_t *)(HDR.icap[i].blk + 0x98) == ret &&
            *(const uint64_t *)(HDR.icap[i].blk + 0x90) == figure) {
            slot = i; break;
        }
    }
    if (slot == LD_ICAP_MAX) {
        if (n < LD_ICAP_MAX) {
            slot = n;
            HDR.icap_n = n + 1;
        } else {
            slot = 0;
            for (uint32_t i = 1; i < LD_ICAP_MAX; i++)
                if (HDR.icap[i].pad_ < HDR.icap[slot].pad_) slot = i;
            HDR.icap[slot].hits = 0;
        }
    }
    ld_iconcap *c = &HDR.icap[slot];
    for (uint32_t k = 0; k < sizeof c->blk; k++) c->blk[k] = blk[k];
    c->hits++;
    c->pad_ = ++icap_seq;
}

__asm__(
    ".text\n"
    ".intel_syntax noprefix\n"
    ".globl h_IconCapture\n"
    ".hidden h_IconCapture\n"
    "h_IconCapture:\n"
    "  push rdi\n  push rsi\n  push rdx\n  push rcx\n  push r8\n  push r9\n"
    "  sub rsp, 0x68\n"
    "  movups [rsp + 0x00], xmm0\n  movups [rsp + 0x10], xmm1\n  movups [rsp + 0x20], xmm2\n"
    "  movups [rsp + 0x30], xmm3\n  movups [rsp + 0x40], xmm4\n  movups [rsp + 0x50], xmm5\n"
    "  lea rdi, [rsp]\n"
    "  call ld_icon_record\n"
    "  movups xmm0, [rsp + 0x00]\n  movups xmm1, [rsp + 0x10]\n  movups xmm2, [rsp + 0x20]\n"
    "  movups xmm3, [rsp + 0x30]\n  movups xmm4, [rsp + 0x40]\n  movups xmm5, [rsp + 0x50]\n"
    "  add rsp, 0x68\n"
    "  pop r9\n  pop r8\n  pop rcx\n  pop rdx\n  pop rsi\n  pop rdi\n"
    "  push rbp\n"
    "  mov rbp, rsp\n"
    "  push r15\n"
    "  jmp qword ptr [rip + ld_icon_resume]\n"
    ".att_syntax prefix\n");

/* --------------------------------------------------- Characters UI capture */
/* Sample the game's own MechSelectionMenus render code, not the unrelated
 * player HUD icon function. The hook is installed only when explicitly
 * enabled through capture_selection=1 in config.ini. */
uint64_t ld_sel_resume;
void ld_sel_record(const void *params, const void *item) {
    ld_sel_resume = HDR.fn.sel_resume;
    if (!item) return;
    uint32_t seq = ++HDR.sel_seq;
    if ((seq & 3u) != 0) return; /* limit overhead to 1 in 4 UI draws */
    uint32_t slot = HDR.sel_n++ % 64u;
    HDR.sel[slot].params = (uint64_t)params;
    HDR.sel[slot].item = (uint64_t)item;
    HDR.sel[slot].seq = seq;
    for (uint32_t i = 0; i < 96u; i++)
        HDR.sel[slot].item_data[i] = ((const uint8_t *)item)[i];
}
__asm__(
    ".text\n"
    ".intel_syntax noprefix\n"
    ".globl h_SelectionCapture\n"
    ".hidden h_SelectionCapture\n"
    "h_SelectionCapture:\n"
    "  push rdi\n  push rsi\n  push rdx\n  push rcx\n  push r8\n  push r9\n"
    "  push rax\n  push r10\n  push r11\n"
    "  sub rsp, 0x90\n"
    "  movups [rsp + 0x00], xmm0\n  movups [rsp + 0x10], xmm1\n"
    "  movups [rsp + 0x20], xmm2\n  movups [rsp + 0x30], xmm3\n"
    "  movups [rsp + 0x40], xmm4\n  movups [rsp + 0x50], xmm5\n"
    "  movups [rsp + 0x60], xmm6\n  movups [rsp + 0x70], xmm7\n"
    "  mov rdi, [rsp + 0xD0]\n"
    "  mov rsi, [rsp + 0xC8]\n"
    "  call ld_sel_record\n"
    "  movups xmm0, [rsp + 0x00]\n  movups xmm1, [rsp + 0x10]\n"
    "  movups xmm2, [rsp + 0x20]\n  movups xmm3, [rsp + 0x30]\n"
    "  movups xmm4, [rsp + 0x40]\n  movups xmm5, [rsp + 0x50]\n"
    "  movups xmm6, [rsp + 0x60]\n  movups xmm7, [rsp + 0x70]\n"
    "  add rsp, 0x90\n"
    "  pop r11\n  pop r10\n  pop rax\n"
    "  pop r9\n  pop r8\n  pop rcx\n  pop rdx\n  pop rsi\n  pop rdi\n"
    "  push rbp\n  mov rbp, rsp\n  push r15\n"
    "  jmp qword ptr [rip + ld_sel_resume]\n"
    ".att_syntax prefix\n");
/* ------------------------------------------------------------ icon test */

typedef void (*dci_fn)(void *widget, uint32_t char_id, float *mat, uint32_t mode, uint32_t a, uint32_t b, float alpha);

static void icon_test(void) {
    ld_icontest *t = &HDR.itest;
    const float *cm = (const float *)HDR.fn.cam_mat, *cp = (const float *)HDR.fn.cam_pos;
    t->frames++;
    for (int i = 0; i < 16; i++) t->cam_mat[i] = cm[i];
    for (int i = 0; i < 4; i++) t->cam_pos[i] = cp[i];
    if (!t->enable || !HDR.fn.draw_char_icon || !HDR.fn.icon_widget) return;
    void *widget = *(void **)HDR.fn.icon_widget;
    if (!widget) return;
    float r[3], u[3], f[3];
    for (int k = 0; k < 3; k++) {
        if (t->flags & 1) { r[k] = cm[k * 4 + 0]; u[k] = cm[k * 4 + 1]; f[k] = cm[k * 4 + 2]; }
        else { r[k] = cm[0 * 4 + k]; u[k] = cm[1 * 4 + k]; f[k] = cm[2 * 4 + k]; }
        if (t->flags & 2) f[k] = -f[k];
        if (t->flags & 4) r[k] = -r[k];
        if (t->flags & 8) u[k] = -u[k];
    }
    uint32_t n = t->count ? t->count : 1;
    if (n > 12) n = 12;
    for (uint32_t i = 0; i < n; i++) {
        float m[16];
        float ox = t->x + t->step * (float)i;
        for (int k = 0; k < 3; k++) {
            m[0 + k] = r[k] * t->scale;
            m[4 + k] = u[k] * t->scale;
            m[8 + k] = f[k] * t->scale;
            m[12 + k] = cp[k] + f[k] * t->dist + r[k] * ox + u[k] * t->y;
        }
        m[3] = m[7] = m[11] = 0.0f; m[15] = 1.0f;
        ((dci_fn)HDR.fn.draw_char_icon)(widget, t->char_id + i, m, 0, 0, 0, t->alpha);
    }
}

/* Called by the game's frame renderer (trampoline at the old "Context Stack" debug call). */
typedef void (*voidfn)(void);
void h_RenderHook(void) {
    icon_test();
    if (mode == M_GRID || HDR.grid.preview) {
        in_grid = 1;
        draw_grid();
        if (mode == M_GRID) draw_native_borders();
        in_grid = 0;
    }
    if (mode != M_CLOSED && HDR.fn.line_print && (HDR.opt_flags & 1)) {
        uint8_t *fa = (uint8_t *)HDR.fn.dbg_flag_a, *fb = (uint8_t *)HDR.fn.dbg_flag_b;
        uint8_t sa = *fa, sb = *fb;
        *fa = 1; *fb = 1;              /* master switches of the debug text system (off in retail) */
        draw();
        *fa = sa; *fb = sb;
    }
    if (HDR.fn.orig_render) ((voidfn)HDR.fn.orig_render)();
}

/* ------------------------------------------------------------ actions */

static void sync_library(void) {
    /* copy tags the game wrote to (vehicle upgrades etc.) into the library */
    for (int s = 0; s < LD_MAX_SLOTS; s++) {
        ld_figure *f = &HDR.pad.fig[s];
        if (!f->used || !f->dirty) continue;
        for (int i = 0; i < LD_LIB_MAX; i++) {
            ld_libtag *t = &HDR.lib[i];
            if (t->used && t->data[0] == f->data[0] && t->data[1] == f->data[1] && t->data[2] == f->data[2] &&
                t->data[4] == f->data[4] && t->data[5] == f->data[5] && t->data[6] == f->data[6]) {
                for (int k = 0; k < LD_TAG_SIZE; k++) t->data[k] = f->data[k];
                t->id = f->id; t->dirty = 1;
                break;
            }
        }
        f->dirty = 0;
        HDR.state_seq++;
    }
}

static int on_pad(uint32_t id) {
    for (int s = 0; s < LD_MAX_SLOTS; s++) if (HDR.pad.fig[s].used && HDR.pad.fig[s].id == id) return s;
    return -1;
}

static void touch_lib(int li) {
    if (li < 0) return;
    if (++use_stamp == 0) use_stamp = 1;
    *(uint16_t *)HDR.lib[li].pad_ = use_stamp;
}

static void place(int slot, uint32_t id) {
    ld_lock();
    sync_library();
    int existing = on_pad(id);
    int li = lib_index_of(id);
    if (existing < 0 && li >= 0) ld_place_tag(&HDR.pad, slot, HDR.lib[li].data);
    else {
        ld_place_id(&HDR.pad, slot, id);
        if (existing < 0 && li < 0)
            for (int i = 0; i < LD_LIB_MAX; i++)
                if (!HDR.lib[i].used) {
                    ld_libtag *n = &HDR.lib[i];
                    for (int k = 0; k < LD_TAG_SIZE; k++) n->data[k] = HDR.pad.fig[slot].data[k];
                    n->id = id; n->used = 1; n->dirty = 1;
                    li = i;
                    break;
                }
    }
    touch_lib(li);
    HDR.state_seq++;
    ld_unlock();
}

/* move slot a to slot b; if b is occupied the two figures swap places. Caller holds the lock. */
static void move_or_swap(int a, int b) {
    if (a == b || !HDR.pad.fig[a].used) return;
    if (!HDR.pad.fig[b].used) { ld_move(&HDR.pad, a, b); return; }
    uint8_t ta[LD_TAG_SIZE], tb[LD_TAG_SIZE];
    for (int k = 0; k < LD_TAG_SIZE; k++) { ta[k] = HDR.pad.fig[a].data[k]; tb[k] = HDR.pad.fig[b].data[k]; }
    ld_remove(&HDR.pad, a);
    ld_remove(&HDR.pad, b);
    ld_place_tag(&HDR.pad, b, ta);
    ld_place_tag(&HDR.pad, a, tb);
}

/* quick send: zone 1 = center, 2 = left, 3 = right */
static void quick_send(int from, int zone) {
    if (!HDR.pad.fig[from].used) { toast("Nothing to send on this zone"); return; }
    if (ld_slot_pad(from) == zone) return;
    ld_lock();
    sync_library();
    int to = ld_find_free_slot(&HDR.pad, zone);
    if (to < 0) to = zone == 1 ? 0 : (zone == 2 ? 1 : 4);   /* zone full: swap with its first slot */
    move_or_swap(from, to);
    HDR.state_seq++;
    ld_unlock();
    slot_cur = view_of_slot(to);
    toast(zone == 1 ? "Sent to center" : (zone == 2 ? "Sent to left" : "Sent to right"));
}

static void act_pad(uint32_t press, uint32_t held) {
    int s = VIEW_SLOT[slot_cur];
    if (press & B_UP) slot_cur = (slot_cur + 6) % 7;
    if (press & B_DOWN) slot_cur = (slot_cur + 1) % 7;
    if (press & B_LEFT) slot_cur = slot_cur < 3 ? slot_cur : (slot_cur == 3 ? 0 : slot_cur - 4);
    if (press & B_RIGHT) slot_cur = slot_cur > 3 ? slot_cur : (slot_cur == 3 ? 4 : slot_cur + 4 > 6 ? 6 : slot_cur + 4);
    if (moving >= 0) {
        if (press & B_CROSS) {
            ld_lock(); sync_library(); move_or_swap(moving, s); HDR.state_seq++; ld_unlock();
            moving = -1; toast("Figure moved");
        }
        if (press & B_CIRCLE) moving = -1;
        return;
    }
    if (press & B_CROSS) {
        build_grid();   /* rescan late-loaded DLC collections on each entry */
        if (g_n[0] > 0 || g_n[1] > 0) {
            mode = M_GRID;
            if (HDR.pad.fig[s].used) {   /* start the grid on the figure already on this zone */
                uint32_t id = HDR.pad.fig[s].id;
                int tt = id >= 1000;
                for (int i = 0; i < g_n[tt]; i++) if (g_tag[tt][i] == id) { g_tab = tt; g_cur[tt] = i; break; }
            }
            return;
        }
        mode = M_LIST;
        build_list();
        if (HDR.pad.fig[s].used) {   /* start the list on the figure already on this zone */
            for (int i = 0; i < nitems; i++)
                if (LD_FIGS[items[i]].id == HDR.pad.fig[s].id) { list_cur[tab] = i; break; }
        }
        return;
    }
    /* SQUARE: tap removes, hold (~1 s) clears the whole pad */
    if (held & B_SQUARE) {
        if (++square_frames == 30) {
            ld_lock(); sync_library();
            for (int i = 0; i < LD_MAX_SLOTS; i++) ld_remove(&HDR.pad, i);
            HDR.state_seq++; ld_unlock();
            toast("Toy Pad cleared");
        }
    } else {
        if (square_frames && square_frames < 30 && HDR.pad.fig[s].used) {
            ld_lock(); sync_library(); ld_remove(&HDR.pad, s); HDR.state_seq++; ld_unlock();
            toast("Figure removed");
        }
        square_frames = 0;
    }
    if ((press & B_CIRCLE) && HDR.pad.fig[s].used) { moving = s; toast("Pick the target zone and press CROSS"); }
    if (press & B_L1) quick_send(s, LD_PAD_LEFT);
    if (press & B_R1) quick_send(s, LD_PAD_RIGHT);
    if (press & (B_L2 | B_R2)) quick_send(s, LD_PAD_CENTER);
}

/* first letter of an entry; rebuilds use the letter of their base vehicle */
static char letter_of(int i) {
    while (i > 0 && LD_FIGS[items[i]].rebuild) i--;
    return upper(LD_FIGNAMES[LD_FIGS[items[i]].name]);
}

static void jump_letter(int dir) {
    if (!nitems) return;
    char cur = letter_of(list_cur[tab]);
    int i = list_cur[tab];
    for (int n = 0; n < nitems; n++) {
        i = (i + dir + nitems) % nitems;
        if (LD_FIGS[items[i]].rebuild) continue;
        char c = letter_of(i);
        if (c != cur) {
            if (dir < 0) {   /* go to the first entry of that letter */
                for (;;) {
                    int p = i;
                    do p = (p - 1 + nitems) % nitems; while (LD_FIGS[items[p]].rebuild && p != i);
                    if (letter_of(p) != c || p == list_cur[tab] || p >= i) break;
                    i = p;
                }
            }
            list_cur[tab] = i;
            return;
        }
    }
}

static void act_list(uint32_t press, uint32_t held) {
    int step = 0;
    if (press & B_UP) step = -1;
    if (press & B_DOWN) step = 1;
    /* auto-repeat */
    if (held & (B_UP | B_DOWN)) {
        repeat_frames++;
        if (repeat_frames > 12 && (repeat_frames & 1)) step = (held & B_UP) ? -1 : 1;
    } else repeat_frames = 0;
    if (tab == T_MINE) {
        if (press & B_LEFT) step = -10;
        if (press & B_RIGHT) step = 10;
    } else {
        if (press & B_LEFT) jump_letter(-1);
        if (press & B_RIGHT) jump_letter(1);
        if (press & (B_L2 | B_R2)) {
            if (nworlds < 0) build_worlds();
            if (press & B_R2) world_filter = world_filter + 1 >= nworlds ? -1 : world_filter + 1;
            else world_filter = world_filter < 0 ? nworlds - 1 : world_filter - 1;
            list_cur[tab] = 0; list_top[tab] = 0;
            build_list();
        }
    }
    if (step && nitems) {
        int c = list_cur[tab] + step;
        if (c < 0) c = (step == -1) ? nitems - 1 : 0;
        if (c >= nitems) c = (step == 1) ? 0 : nitems - 1;
        list_cur[tab] = c;
    }
    if (press & (B_L1 | B_R1)) {
        tab = (tab + ((press & B_R1) ? 1 : T_COUNT - 1)) % T_COUNT;
        build_list();
    }
    if (press & (B_CIRCLE | B_TRIANGLE)) mode = M_PAD;
    if ((press & B_CROSS) && nitems) {
        const ld_figinfo *f = &LD_FIGS[items[list_cur[tab]]];
        place(VIEW_SLOT[slot_cur], f->id);
        mode = M_PAD;
        char t[64], *d = t; d = scat(d, "Placed: "); scat(d, LD_FIGNAMES + f->name);
        toast(t);
    }
}

static void act_grid(uint32_t press, uint32_t held) {
    int t = g_tab, n = g_n[t];
    int cols = HDR.grid.cols ? (int)HDR.grid.cols : 12;
    border_calibrating = !!(held & B_SQUARE);
    if (border_calibrating) {
        /* The native UI uses a 2D layout while the portraits use a camera
         * plane. Keep calibration accessible in-game until exact projection
         * conversion is validated across the different game cameras.
         * This shortcut never changes the Toy Pad state or selected figure. */
        ld_bordercfg *b = &HDR.border;
        int dir = 0;
        if (press & B_LEFT) dir = -1;
        else if (press & B_RIGHT) dir = 1;
        else if (press & B_UP) dir = 2;
        else if (press & B_DOWN) dir = -2;
        if (held & (B_UP | B_DOWN | B_LEFT | B_RIGHT)) {
            if (++repeat_frames > 10 && (repeat_frames & 1)) {
                if (held & B_LEFT) dir = -1;
                else if (held & B_RIGHT) dir = 1;
                else if (held & B_UP) dir = 2;
                else if (held & B_DOWN) dir = -2;
            }
        } else repeat_frames = 0;
        if (dir == -1 && b->x_add > -1.8f) b->x_add -= 0.02f;
        if (dir ==  1 && b->x_add <  1.8f) b->x_add += 0.02f;
        if (dir == -2 && b->y_add > -1.8f) b->y_add -= 0.02f;
        if (dir ==  2 && b->y_add <  1.8f) b->y_add += 0.02f;
        if ((press & B_L2) && b->selected_scale > 0.01f)
            b->selected_scale -= 0.004f;
        if ((press & B_R2) && b->selected_scale < 0.25f)
            b->selected_scale += 0.004f;
        if (press & B_TRIANGLE) {
            b->enable = (b->enable + 1) % 3;
            toast(b->enable == 0 ? "Rings disabled" :
                  b->enable == 1 ? "Selected ring only" : "Rings for all characters");
        }
        return;
    }
    int step = 0;
    if (press & B_LEFT) step = -1;
    if (press & B_RIGHT) step = 1;
    if (press & B_UP) step = -cols;
    if (press & B_DOWN) step = cols;
    if (held & (B_UP | B_DOWN | B_LEFT | B_RIGHT)) {     /* auto-repeat */
        if (++repeat_frames > 12 && (repeat_frames & 1))
            step = (held & B_LEFT) ? -1 : (held & B_RIGHT) ? 1 : (held & B_UP) ? -cols : cols;
    } else repeat_frames = 0;
    if (step && n > 0) {
        g_cur[t] = ld_grid_step(g_cur[t], n, step);
    }
    int per_page = cols * (HDR.grid.rows ? (int)HDR.grid.rows : 4);
    if ((press & B_R2) && n > 0)
        g_cur[t] = ld_grid_page_move(g_cur[t], n, per_page, 1);
    if ((press & B_L2) && n > 0)
        g_cur[t] = ld_grid_page_move(g_cur[t], n, per_page, -1);
    if (press & (B_L1 | B_R1)) { g_tab ^= 1; return; }
    if (press & B_CIRCLE) mode = M_PAD;
    if (press & B_TRIANGLE) { mode = M_LIST; tab = g_tab == 0 ? T_CHARS : T_VEHICLES; build_list(); }
    if ((press & B_CROSS) && n > 0) {
        uint32_t id = g_tag[t][g_cur[t]];
        place(VIEW_SLOT[slot_cur], id);
        mode = M_PAD;
        char tt[64], *d = tt; d = scat(d, "Placed: "); scat(d, fig_name(id));
        toast(tt);
    }
}

static void update_line(void) {
    char buf[96], *d = buf;
    if (mode == M_PAD) {
        int s = VIEW_SLOT[slot_cur];
        slot_label(d, s); d += slen(d); d = scat(d, ": ");
        scat(d, HDR.pad.fig[s].used ? fig_name(HDR.pad.fig[s].id) : "(empty)");
    } else if (mode == M_GRID && g_n[g_tab] > 0) {
        scat(d, fig_name(g_tag[g_tab][g_cur[g_tab]]));
    } else if (mode == M_LIST && nitems) {
        scat(d, LD_FIGNAMES + LD_FIGS[items[list_cur[tab]]].name);
    } else buf[0] = 0;
    int changed = 0;
    for (int i = 0; i < 96; i++) { if (HDR.menu_line[i] != buf[i]) changed = 1; if (!buf[i]) break; }
    if (changed) set_menu_line(buf);
}

/* ------------------------------------------------------------ web remote mailbox */

/* Runs one pending request from the injector (see ld_header.mbox). Placement goes through place(),
 * so web-loaded figures use and update the same tag library as the in-game menu (upgrades persist).
 * The injector sequences remove-then-place and lift-then-place with real delays between requests,
 * so the game always sees each change as its own event. */
static void run_mailbox(void) {
    uint32_t r = HDR.mbox.req;
    if (r == HDR.mbox.done) return;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    uint32_t op = HDR.mbox.op, a = HDR.mbox.a, b = HDR.mbox.b;
    int32_t res = -1;
    if (op == LD_MB_PLACE_ID && a < LD_MAX_SLOTS && b != 0 && !HDR.pad.fig[a].used) {
        place((int)a, b);
        res = HDR.pad.fig[a].used ? 0 : -1;
    } else if (op == LD_MB_REMOVE && a < LD_MAX_SLOTS) {
        ld_lock(); sync_library(); res = ld_remove(&HDR.pad, (int)a); HDR.state_seq++; ld_unlock();
    } else if (op == LD_MB_CLEAR_ALL) {
        ld_lock(); sync_library();
        for (int i = 0; i < LD_MAX_SLOTS; i++) ld_remove(&HDR.pad, i);
        HDR.state_seq++; ld_unlock();
        res = 0;
    }
    HDR.mbox.result = res;
    __atomic_store_n(&HDR.mbox.done, r, __ATOMIC_RELEASE);
}

/* ------------------------------------------------------------ per pad read */

static void close_menu(void) {
    mode = M_CLOSED; owner = -1; moving = -1; square_frames = 0;
    HDR.menu_open = 0; HDR.state_seq++;
}

/* ------------------------------------------------------------ console overlay input (ext mode) */

static int32_t ext_owner = -1;
static uint32_t ext_last = 0xFFFFFFFFu;
static int ext_armed;

static void swallow(uint32_t *buttons, uint8_t *sticks) {
    *buttons = 0;
    sticks[0] = sticks[1] = sticks[2] = sticks[3] = 0x80;
    sticks[4] = sticks[5] = 0;  /* analog L2/R2 */
}

/* The overlay is drawn by the injector in the system UI; this only owns the controller. */
static void ext_frame(int k, int32_t handle, uint32_t *buttons, uint8_t *sticks, uint32_t combo) {
    uint32_t b = *buttons;
    HDR.ext.heartbeat++;
    if (!HDR.ext.open) {
        if (k == 0) HDR.ext.raw = b;
        uint32_t hold_frames = HDR.opt_hold_ms / 33 + 1;
        if ((b & combo) == combo) {
            if (++h_hold[k] >= hold_frames) {
                h_hold[k] = 0;
                ext_owner = handle; ext_armed = 0; ext_last = 0xFFFFFFFFu;
                HDR.ext.raw = 0;
                HDR.ext.close_ack = HDR.ext.close_req;
                HDR.ext.open_seq++;
                __atomic_store_n(&HDR.ext.open, 1, __ATOMIC_RELEASE);
                swallow(buttons, sticks);
            }
        } else h_hold[k] = 0;
        return;
    }
    if (HDR.ext.close_req != HDR.ext.close_ack) {
        HDR.ext.close_ack = HDR.ext.close_req;
        __atomic_store_n(&HDR.ext.open, 0, __ATOMIC_RELEASE);
        swallow(buttons, sticks);
        return;
    }
    if (handle != ext_owner) { swallow(buttons, sticks); return; }   /* other players frozen too */
    /* the same combo closes the overlay once it has been released after opening */
    if ((b & combo) != combo) ext_armed = 1;
    else if (ext_armed) {
        __atomic_store_n(&HDR.ext.open, 0, __ATOMIC_RELEASE);
        swallow(buttons, sticks);
        return;
    }
    uint32_t nav = b | ld_grid_stick_direction(sticks[0], sticks[1]);
    if (nav != ext_last) {
        uint32_t w = HDR.ext.wr;
        HDR.ext.ring[w & 31] = nav;
        __atomic_store_n(&HDR.ext.wr, w + 1, __ATOMIC_RELEASE);
        ext_last = nav;
    }
    swallow(buttons, sticks);
}

static uint32_t lease_seen, lease_frames;

/* ext mode only while the injector keeps renewing its lease (it died / was replaced otherwise) */
static int ext_alive(void) {
    if (!HDR.ext.mode) return 0;
    uint32_t l = HDR.ext.lease;
    if (l != lease_seen) { lease_seen = l; lease_frames = 0; return 1; }
    return ++lease_frames < 200;
}

void menu_frame(int32_t handle, uint32_t *buttons, uint8_t *sticks) {
    run_mailbox();
    if (mode == M_CLOSED && ext_alive()) {
        int kk = 0;
        while (kk < 4 && h_id[kk] != handle && h_id[kk] != -1) kk++;
        if (kk == 4) kk = 3;
        h_id[kk] = handle;
        ext_frame(kk, handle, buttons, sticks, HDR.opt_combo ? HDR.opt_combo : (B_L3 | B_R3));
        return;
    }
    if (HDR.ext.open) __atomic_store_n(&HDR.ext.open, 0, __ATOMIC_RELEASE);   /* overlay switched off */
    uint32_t b = *buttons;
    uint32_t combo = HDR.opt_combo ? HDR.opt_combo : (B_L3 | B_R3);
    /* The left stick and D-pad share the menu navigation path. Read input
       BEFORE centering analog sticks for the game. Only while the mod is open. */
    if (mode != M_CLOSED) b |= ld_grid_stick_direction(sticks[0], sticks[1]);
    int k = 0;
    while (k < 4 && h_id[k] != handle && h_id[k] != -1) k++;
    if (k == 4) k = 3;
    h_id[k] = handle;
    if (mode != M_CLOSED && handle != owner) {   /* other players are frozen while the menu is open */
        *buttons = 0;
        sticks[0] = sticks[1] = sticks[2] = sticks[3] = 0x80;
        sticks[4] = sticks[5] = 0;
        return;
    }
    if (handle == owner || owner == -1) frame++;
    if ((frame & 63) == 0) { ld_lock(); sync_library(); ld_unlock(); }

    if (mode == M_CLOSED) {
        /* open when every button of the combo is held for opt_hold_ms */
        uint32_t hold_frames = HDR.opt_hold_ms / 33 + 1;
        if ((b & combo) == combo) {
            if (++h_hold[k] >= hold_frames) {
                h_hold[k] = 0;
                mode = M_PAD; moving = -1; owner = handle; combo_armed = 0; square_frames = 0;
                HDR.menu_open = 1; HDR.state_seq++;
                prev_btn = b;
            }
        } else h_hold[k] = 0;
        if (mode == M_CLOSED) return;
    }

    uint32_t press = b & ~prev_btn;
    prev_btn = b;
    /* the same combo closes the menu once it has been released after opening */
    if ((b & combo) != combo) combo_armed = 1;
    else if (combo_armed) { close_menu(); press = 0; }

    if (mode == M_PAD) {
        if (press & (B_TRIANGLE | B_OPTIONS)) close_menu();
        else act_pad(press, b);
    } else if (mode == M_LIST) {
        if (press & B_OPTIONS) close_menu();
        else act_list(press, b);
    } else if (mode == M_GRID) {
        if (press & B_OPTIONS) close_menu();
        else act_grid(press, b);
    }
    if (mode != M_CLOSED) update_line();

    /* hide all input from the game while the menu is open (and on the closing frame) */
    *buttons = 0;
    sticks[0] = sticks[1] = sticks[2] = sticks[3] = 0x80;
    sticks[4] = sticks[5] = 0;  /* analog L2/R2 */
}
