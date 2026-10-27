/* LEGO Dimensions - Virtual Toy Pad, PS4 GoldHEN plugin.
 *
 * The PS5 build is an external payload that injects the hook module (src/hook) into the game
 * and patches it from outside. On a jailbroken PS4 GoldHEN's plugin loader loads this .prx
 * straight into LEGO Dimensions before the game's main() runs, so the same hook module is
 * linked in here and wired up from the inside:
 *
 *   - the game's libSceUsbd / scePadReadState imports (GOT slots) are pointed at the h_* hooks,
 *     the originals are reached through this plugin's own imports;
 *   - the render, DrawCharacterIcon and icon-draw hooks (Heitor's in-game menu) and the
 *     60 FPS / internal resolution patches go to the same eboot offsets as on PS5;
 *   - a monitor thread saves tags/upgrades to /data/ldmod/tags, keeps pad.txt up to date and
 *     services the LegoToypad phone remote on port 8765 (src/injector/webremote.c, shared).
 *
 * Supported: CUSA00935 v01.23 (US) and CUSA01176 v01.24 (EU), as on PS5.
 * Hardware status: built, not yet tested on a real PS4 - see README "PS4".
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "GoldHEN.h"

#include "../../src/hook/ldhook.h"
#include "../../src/injector/offsets.h"
#include "../../src/injector/webremote.h"

#define attr_public __attribute__((visibility("default")))
#define attr_module_hidden __attribute__((weak)) __attribute__((visibility("hidden")))

#define PLUGIN_NAME "VirtualToyPad"
#define PLUGIN_VER  0x00000200   /* 2.00, same feature level as the PS5 v2.0.0 release */

attr_public const char *g_pluginName = PLUGIN_NAME;
attr_public const char *g_pluginDesc = "LEGO Dimensions virtual Toy Pad (LegoToypad phone remote, in-game menu, 60 FPS)";
attr_public const char *g_pluginAuth = "SpiderNic96, Heitor Spectre, harrysof";
attr_public uint32_t g_pluginVersion = PLUGIN_VER;

#define DATA_DIR "/data/ldmod"
#define TAG_DIR  DATA_DIR "/tags"
#define LOG_FILE DATA_DIR "/vtp_ps4.log"

/* ------------------------------------------------------------ hook module */

extern ld_header HDR;   /* src/hook/hooks.c */
long h_GetDeviceList(); void h_FreeDeviceList(); int h_GetDeviceDescriptor(); uint8_t h_GetBusNumber();
uint8_t h_GetDeviceAddress(); void *h_OpenDeviceWithVidPid(); int h_GetConfigDescriptor();
void h_FreeConfigDescriptor(); int h_KernelDriverActive(); int h_DetachKernelDriver();
int h_ClaimInterface(); int h_ResetDevice(); int h_CheckConnected(); int h_GetMaxPacketSize();
int h_SubmitTransfer(); int h_HandleEvents(); int h_PadReadState(); void h_FillInterruptTransfer();
void h_RenderHook(void);
/* asm entry stubs in src/hook/menu.c */
void h_DCICapture(void); void h_IconCapture(void); void h_SelectionCapture(void);

/* the real system functions, reached through this plugin's own imports */
int sceUsbdGetDeviceList(); int sceUsbdFreeDeviceList(); int sceUsbdGetDeviceDescriptor();
int sceUsbdGetBusNumber(); int sceUsbdGetDeviceAddress(); int sceUsbdOpenDeviceWithVidPid();
int sceUsbdGetConfigDescriptor(); int sceUsbdFreeConfigDescriptor(); int sceUsbdKernelDriverActive();
int sceUsbdDetachKernelDriver(); int sceUsbdClaimInterface(); int sceUsbdResetDevice();
int sceUsbdCheckConnected(); int sceUsbdGetMaxPacketSize(); int sceUsbdSubmitTransfer();
int sceUsbdHandleEvents(); int scePadReadState(); int sceUsbdFillInterruptTransfer();
int sceKernelUsleep(unsigned int);
int scePthreadCreate(void *, const void *, void *(*)(void *), void *, const char *);
int sceKernelIsNeoMode(void);
int sceKernelMprotect(const void *addr, size_t len, int prot);

/* same order as the H_* enum / LD_IMPORTS */
static void *const HOOKS[H_COUNT] = {
    (void *)h_GetDeviceList, (void *)h_FreeDeviceList, (void *)h_GetDeviceDescriptor, (void *)h_GetBusNumber,
    (void *)h_GetDeviceAddress, (void *)h_OpenDeviceWithVidPid, (void *)h_GetConfigDescriptor,
    (void *)h_FreeConfigDescriptor, (void *)h_KernelDriverActive, (void *)h_DetachKernelDriver,
    (void *)h_ClaimInterface, (void *)h_ResetDevice, (void *)h_CheckConnected, (void *)h_GetMaxPacketSize,
    (void *)h_SubmitTransfer, (void *)h_HandleEvents, (void *)h_PadReadState, (void *)h_FillInterruptTransfer};
static void *const REAL[H_COUNT] = {
    (void *)sceUsbdGetDeviceList, (void *)sceUsbdFreeDeviceList, (void *)sceUsbdGetDeviceDescriptor,
    (void *)sceUsbdGetBusNumber, (void *)sceUsbdGetDeviceAddress, (void *)sceUsbdOpenDeviceWithVidPid,
    (void *)sceUsbdGetConfigDescriptor, (void *)sceUsbdFreeConfigDescriptor, (void *)sceUsbdKernelDriverActive,
    (void *)sceUsbdDetachKernelDriver, (void *)sceUsbdClaimInterface, (void *)sceUsbdResetDevice,
    (void *)sceUsbdCheckConnected, (void *)sceUsbdGetMaxPacketSize, (void *)sceUsbdSubmitTransfer,
    (void *)sceUsbdHandleEvents, (void *)scePadReadState, (void *)sceUsbdFillInterruptTransfer};

/* ------------------------------------------------------------------ utils */

/* OpenOrbis' headers expand errno to __errno_location() (musl); the system libc that GoldHEN
 * plugins link keeps it in libkernel's __error() */
#ifndef VTP_HOST   /* host test harness: glibc has its own */
int *__error(void);
int *__errno_location(void) { return __error(); }
#endif

typedef struct { char useless1[45]; char message[3075]; } notify_request_t;
int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

static pthread_mutex_t log_mx = PTHREAD_MUTEX_INITIALIZER;

static void logf_(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&log_mx);
    FILE *f = fopen(LOG_FILE, "a");
    if (f) {
        time_t t = time(0);
        struct tm *tm = localtime(&t);   /* log_mx serialises the shared buffer */
        if (tm) fprintf(f, "%02d:%02d:%02d %s\n", tm->tm_hour, tm->tm_min, tm->tm_sec, buf);
        else fprintf(f, "%s\n", buf);
        fclose(f);
    }
    pthread_mutex_unlock(&log_mx);
}
#define LOG(...) logf_(__VA_ARGS__)

void web_log(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    logf_("%s", buf);
}

static void notify(const char *fmt, ...) {
    notify_request_t req;
    memset(&req, 0, sizeof req);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(req.message, sizeof req.message, fmt, ap);
    va_end(ap);
    sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ----------------------------------------------------------------- config */

/* Same file and keys as the PS5 payload (the console overlay keys are PS5-only and are kept
 * as they are if present). */
static struct {
    uint32_t hold_ms, draw_text, notify_menu, force_fake, restore_pad, fps60, resolution;
    uint32_t combo;
    char combo_txt[64];
} cfg = {250, 1, 0, 0, 0, 1, 1080, 0x2 | 0x4, "L3+R3"};

static const struct { const char *name; uint32_t bit; } BUTTONS[] = {
    {"L3", 0x2}, {"R3", 0x4}, {"OPTIONS", 0x8}, {"UP", 0x10}, {"RIGHT", 0x20}, {"DOWN", 0x40},
    {"LEFT", 0x80}, {"L2", 0x100}, {"R2", 0x200}, {"L1", 0x400}, {"R1", 0x800},
    {"TRIANGLE", 0x1000}, {"CIRCLE", 0x2000}, {"CROSS", 0x4000}, {"SQUARE", 0x8000},
    {"TOUCHPAD", 0x100000}, {0, 0}};

static uint32_t parse_combo(const char *txt) {
    uint32_t mask = 0;
    char tok[16];
    while (*txt) {
        int n = 0;
        while (*txt && *txt != '+' && *txt != '\r' && *txt != '\n' && n < 15) {
            char c = *txt++;
            if (c != ' ') tok[n++] = (c >= 'a' && c <= 'z') ? c - 32 : c;
        }
        tok[n] = 0;
        if (*txt == '+') txt++;
        else if (*txt) txt++;
        if (!n) continue;
        int found = 0;
        for (int i = 0; BUTTONS[i].name; i++)
            if (!strcmp(tok, BUTTONS[i].name)) { mask |= BUTTONS[i].bit; found = 1; }
        if (!found) { LOG("config: unknown button '%s' in menu_buttons", tok); return 0; }
    }
    return mask;
}

static void write_default_config(void) {
    FILE *f = fopen(DATA_DIR "/config.ini", "w");
    if (!f) return;
    fprintf(f, "# LEGO Dimensions - Virtual Toy Pad (PS4 GoldHEN plugin)\n"
               "config_version=6\n\n"
               "# Buttons that open/close the in-game menu, joined with '+'.\n"
               "# Names: L1 R1 L2 R2 L3 R3 CROSS CIRCLE SQUARE TRIANGLE UP DOWN LEFT RIGHT OPTIONS TOUCHPAD\n"
               "menu_buttons=%s\n"
               "# How long the buttons must be held to open the menu (milliseconds)\nhold_ms=%u\n\n"
               "# Draw the menu on screen with the game's debug text renderer (1/0)\ndraw_text=%u\n"
               "# Also show the highlighted menu entry as a system notification (1/0)\nnotify_menu=%u\n"
               "# Use the virtual Toy Pad even when a real one is plugged in (1/0)\nforce_fake=%u\n"
               "# Put back the figures from the last session when the game starts (1/0)\nrestore_pad=%u\n"
               "# Patch the engine to target 60 FPS instead of 30 (connorh315's patch; 1/0).\n"
               "# PS4 Pro holds it best; a base PS4 will dip below 60 in busy scenes.\nfps60=%u\n"
               "# Internal render resolution: 1080 (game default), 1440 or 2160.\n"
               "# PS4: base PS4 is limited to 1080, PS4 Pro to 1440.\nresolution=%u\n",
            cfg.combo_txt, cfg.hold_ms, cfg.draw_text, cfg.notify_menu, cfg.force_fake, cfg.restore_pad,
            cfg.fps60, cfg.resolution);
    fclose(f);
}

static void load_config(void) {
    FILE *f = fopen(DATA_DIR "/config.ini", "r");
    if (!f) { write_default_config(); return; }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        unsigned v;
        char txt[64];
        if (line[0] == '#') continue;
        if (sscanf(line, "hold_ms=%u", &v) == 1) cfg.hold_ms = v;
        else if (sscanf(line, "draw_text=%u", &v) == 1) cfg.draw_text = v;
        else if (sscanf(line, "notify_menu=%u", &v) == 1) cfg.notify_menu = v;
        else if (sscanf(line, "force_fake=%u", &v) == 1) cfg.force_fake = v;
        else if (sscanf(line, "restore_pad=%u", &v) == 1) cfg.restore_pad = v;
        else if (sscanf(line, "fps60=%u", &v) == 1) cfg.fps60 = v;
        else if (sscanf(line, "resolution=%u", &v) == 1) cfg.resolution = v;
        else if (sscanf(line, "menu_buttons=%63[^\r\n]", txt) == 1) {
            uint32_t m = parse_combo(txt);
            if (m) { cfg.combo = m; snprintf(cfg.combo_txt, sizeof cfg.combo_txt, "%s", txt); }
        }
    }
    fclose(f);
}

/* -------------------------------------------------------- memory patching */

/* Write into the game's code or GOT. GoldHEN's process write goes through the kernel and
 * ignores page protection; if it is not available, fall back to mprotect + memcpy. */
static int patch_mem(intptr_t addr, const void *buf, size_t len) {
    struct proc_rw rw = {(uint64_t)addr, (void *)buf, len, 1};
    if (sys_sdk_proc_rw(&rw) < 0 || memcmp((const void *)addr, buf, len)) {
        intptr_t page = addr & ~0x3FFFL;
        size_t span = ((addr + len + 0x3FFF) & ~0x3FFFL) - page;
        if (sceKernelMprotect((void *)page, span, 7 /* RWX */) < 0) LOG("mprotect 0x%lx failed", (long)page);
        memcpy((void *)addr, buf, len);
    }
    return memcmp((const void *)addr, buf, len) ? -1 : 0;
}

static intptr_t g_base, g_code_shift;

static int verify_build(intptr_t base) {
    for (size_t i = 0; i < sizeof LD_SIGS / sizeof LD_SIGS[0]; i++)
        if (memcmp((const void *)(base + LD_SIGS[i].off), LD_SIGS[i].bytes, sizeof LD_SIGS[i].bytes)) {
            LOG("signature mismatch at +0x%lx (unsupported game version?)", (long)LD_SIGS[i].off);
            return -1;
        }
    return 0;
}

/* EU v01.24: the code past the render target sits 0x10 earlier (see src/injector/main.c) */
static int detect_code_shift(intptr_t base) {
    static const uint8_t pro[6] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};
    static const intptr_t fns[4] = {LD_OFF_DRAW_CHAR_ICON, LD_OFF_ICON_DRAW, LD_OFF_SELECTION_DRAW, LD_OFF_DRAW_UI_SPRITE};
    static const intptr_t shifts[2] = {0, -0x10};
    for (int s = 0; s < 2; s++) {
        int ok = 1;
        for (int i = 0; i < 4 && ok; i++) {
            const uint8_t *b = (const uint8_t *)(base + fns[i] + shifts[s]);
            ok = !memcmp(b, pro, 6) || (i < 3 && b[0] == 0xE9);
        }
        if (ok) {
            intptr_t ret = LD_OFF_HUD_ICON_RET + shifts[s];
            const uint8_t *c = (const uint8_t *)(base + ret - 5);
            int32_t rel;
            if (c[0] != 0xE8) ok = 0;
            else { memcpy(&rel, c + 1, 4); ok = ret + rel == LD_OFF_DRAW_CHAR_ICON + shifts[s]; }
        }
        if (ok) {
            g_code_shift = shifts[s];
            LOG("code layout: %s (shift %ld)", s ? "EU v01.24" : "US v01.23", (long)g_code_shift);
            return 0;
        }
    }
    LOG("code layout: icon functions not found at the known offsets");
    return -1;
}

#define LD_OFF_FPS60 0x6966ad
static const uint8_t FPS60_STOCK[9] = {0x8b, 0x18, 0x89, 0x38, 0x83, 0xff, 0x03, 0x74, 0x11};
static const uint8_t FPS60_BYTES[9] = {0xbf, 0x02, 0x00, 0x00, 0x00, 0x89, 0x38, 0x90, 0x90};

static void apply_fps60(intptr_t base) {
    if (!cfg.fps60) { LOG("60 FPS patch disabled in config"); return; }
    const uint8_t *cur = (const uint8_t *)(base + LD_OFF_FPS60);
    if (!memcmp(cur, FPS60_BYTES, 9)) { LOG("60 FPS patch already present (GoldHEN cheat/patch?)"); return; }
    if (memcmp(cur, FPS60_STOCK, 9)) { LOG("60 FPS: unexpected bytes at the patch site; not patching"); return; }
    if (patch_mem(base + LD_OFF_FPS60, FPS60_BYTES, 9)) LOG("60 FPS patch write failed");
    else LOG("60 FPS patch applied");
}

static const struct { uint32_t off; uint16_t stock; uint8_t is_w; } RES_SITES[6] = {
    {0x7148f8, 1920, 1}, {0x714902, 1080, 0}, {0x714edc, 1920, 1},
    {0x714ee3, 1080, 0}, {0x714f23, 1920, 1}, {0x714f28, 1080, 0}};

static void apply_resolution(intptr_t base) {
    unsigned want_res = cfg.resolution, pro = sceKernelIsNeoMode() ? 1 : 0;
    unsigned cap = pro ? 1440 : 1080;
    LOG("console: %s", pro ? "PS4 Pro" : "PS4");
    if (want_res > cap) {
        LOG("resolution=%u is above the %s limit; using %u", want_res, pro ? "PS4 Pro" : "PS4", cap);
        notify("Virtual Toy Pad: resolution %u is too heavy for %s.\nUsing %u.", want_res, pro ? "PS4 Pro" : "PS4", cap);
        want_res = cap;
    }
    uint16_t w, h;
    switch (want_res) {
    case 1080: LOG("resolution: game default (1920x1080)"); return;
    case 1440: w = 2560; h = 1440; break;
    default: LOG("resolution=%u not supported (use 1080 or 1440); keeping default", want_res); return;
    }
    for (int i = 0; i < 6; i++) {
        uint16_t cur;
        memcpy(&cur, (const void *)(base + RES_SITES[i].off), 2);
        if (cur != RES_SITES[i].stock && cur != (RES_SITES[i].is_w ? w : h)) {
            LOG("resolution: +0x%x holds 0x%04x; not patching", RES_SITES[i].off, cur);
            return;
        }
    }
    for (int i = 0; i < 6; i++) {
        uint16_t v = RES_SITES[i].is_w ? w : h;
        if (patch_mem(base + RES_SITES[i].off, &v, 2)) { LOG("resolution: write failed"); return; }
    }
    LOG("resolution: internal render set to %ux%u", w, h);
}

/* 14-byte absolute jump (jmp qword [rip+0]; .quad target) placed in the zero padding after .text */
static int write_trampoline(intptr_t at, uint64_t target) {
    const uint8_t *pad = (const uint8_t *)at;
    int is_zero = 1;
    for (int i = 0; i < 14; i++) if (pad[i]) is_zero = 0;
    if (!is_zero && !(pad[0] == 0xFF && pad[1] == 0x25)) { LOG("trampoline at 0x%lx not free", (long)at); return -1; }
    uint8_t tr[14] = {0xFF, 0x25, 0, 0, 0, 0};
    memcpy(tr + 6, &target, 8);
    return patch_mem(at, tr, sizeof tr);
}

static int install_render_hook(intptr_t base) {
    const uint8_t *call = (const uint8_t *)(base + LD_OFF_RENDER_CALL);
    int32_t cur, rel = (int32_t)(LD_OFF_RENDER_TARGET - (LD_OFF_RENDER_CALL + 5));
    int32_t nrel = (int32_t)(LD_OFF_TRAMPOLINE - (LD_OFF_RENDER_CALL + 5));
    memcpy(&cur, call + 1, 4);
    if (call[0] != 0xE8 || (cur != rel && cur != nrel)) { LOG("render call site changed"); return -1; }
    if (write_trampoline(base + LD_OFF_TRAMPOLINE, (uint64_t)h_RenderHook)) return -1;
    uint8_t ncall[5] = {0xE8};
    memcpy(ncall + 1, &nrel, 4);
    if (patch_mem(base + LD_OFF_RENDER_CALL, ncall, 5)) { LOG("render call patch failed"); return -1; }
    LOG("render hook installed");
    return 0;
}

/* functions starting with push rbp; mov rbp,rsp; push r15 -> jmp trampoline (stub resumes at +6) */
static int install_prologue_hook(intptr_t base, intptr_t fn_off, intptr_t tramp_off, void *target) {
    static const uint8_t prologue[6] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};
    const uint8_t *cur = (const uint8_t *)(base + fn_off);
    if (memcmp(cur, prologue, 6) && cur[0] != 0xE9) { LOG("prologue at +0x%lx changed", (long)fn_off); return -1; }
    if (write_trampoline(base + tramp_off, (uint64_t)target)) return -1;
    int32_t rel = (int32_t)(tramp_off - (fn_off + 5));
    uint8_t jmp[6] = {0xE9, 0, 0, 0, 0, 0x90};
    memcpy(jmp + 1, &rel, 4);
    if (patch_mem(base + fn_off, jmp, 6)) return -1;
    LOG("prologue hook installed at +0x%lx", (long)fn_off);
    return 0;
}

/* point one game GOT slot at our hook; returns 1 if it had to be (re)written */
static int hook_got(int i) {
    uint64_t *slot = (uint64_t *)(g_base + LD_IMPORTS[i].got);
    uint64_t want = (uint64_t)HOOKS[i];
    if (*slot == want) return 0;
    uint64_t was = *slot;
    if (patch_mem((intptr_t)slot, &want, 8)) { LOG("GOT patch failed for %s", LD_IMPORTS[i].name); return -1; }
    LOG("%s: GOT 0x%lx -> hook", LD_IMPORTS[i].name, (long)was);
    return 1;
}

/* ------------------------------------------------------------ tag library */

static int load_library(ld_header *h) {
    int n = 0;
    char path[128];
    for (uint32_t id = 1; id < 1300 && n < LD_LIB_MAX; id++) {
        snprintf(path, sizeof path, TAG_DIR "/%u.bin", id);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        if (read(fd, h->lib[n].data, LD_TAG_SIZE) == LD_TAG_SIZE) {
            h->lib[n].id = id;
            h->lib[n].used = 1;
            n++;
        }
        close(fd);
    }
    return n;
}

static void restore_pad(ld_header *h) {
    FILE *f = fopen(DATA_DIR "/pad.txt", "r");
    if (!f) return;
    int slot;
    unsigned id;
    while (fscanf(f, "%d=%u", &slot, &id) == 2) {
        if (slot < 0 || slot >= LD_MAX_SLOTS || !id) continue;
        int li = -1;
        for (int i = 0; i < LD_LIB_MAX; i++) if (h->lib[i].used && h->lib[i].id == id) { li = i; break; }
        if (li >= 0) ld_place_tag(&h->pad, slot, h->lib[li].data);
        else ld_place_id(&h->pad, slot, id);
    }
    fclose(f);
    h->pad.q_head = h->pad.q_tail = 0;
}

void ld_lock(void);
void ld_unlock(void);

static void save_state(void) {
    static ld_libtag tags[LD_LIB_MAX];   /* big: keep off the stack */
    static ld_figure fig[LD_MAX_SLOTS];
    int dirty[LD_LIB_MAX];
    ld_lock();
    for (int i = 0; i < LD_LIB_MAX; i++) {
        dirty[i] = HDR.lib[i].used && HDR.lib[i].dirty;
        if (dirty[i]) { tags[i] = HDR.lib[i]; HDR.lib[i].dirty = 0; }
    }
    memcpy(fig, HDR.pad.fig, sizeof fig);
    ld_unlock();
    mkdir(DATA_DIR, 0777);
    mkdir(TAG_DIR, 0777);
    for (int i = 0; i < LD_LIB_MAX; i++) {
        if (!dirty[i]) continue;
        char path[128];
        snprintf(path, sizeof path, TAG_DIR "/%u.bin", tags[i].id);
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd >= 0) { write(fd, tags[i].data, LD_TAG_SIZE); close(fd); }
    }
    FILE *f = fopen(DATA_DIR "/pad.txt", "w");
    if (f) {
        for (int s = 0; s < LD_MAX_SLOTS; s++)
            if (fig[s].used) fprintf(f, "%d=%u\n", s, fig[s].id);
        fclose(f);
    }
}

/* ---------------------------------------------------------------- monitor */

static void *monitor(void *arg) {
    (void)arg;
    pid_t self = getpid();
    intptr_t mem = (intptr_t)&HDR;
    uint32_t seq = HDR.state_seq, last_open = 0, sniff_done = HDR.sniff_n;
    uint64_t last_notify = 0, last_save = 0, last_status = 0, last_got = 0;
    char last_line[96] = "";
    int reported = 0, passthrough_logged = 0;
    web_attach(self, mem);
    for (;;) {
        uint64_t t = now_ms();
        if (t - last_status > 10000) {
            last_status = t;
            LOG("status: opened=%u cmds=%u delivered=%u pump=%u pad_calls=%u passthrough=%u menu=%u seq=%u",
                HDR.fake_opened, HDR.usb_cmds, HDR.delivered, HDR.pump_running, HDR.pad_calls, HDR.passthrough,
                HDR.menu_open, HDR.state_seq);
        }
        /* the dynamic linker or the game itself must not undo the import redirection */
        if (t - last_got > 1000) {
            last_got = t;
            for (int i = 0; i < H_COUNT; i++)
                if (hook_got(i) > 0) LOG("re-applied the %s redirection", LD_IMPORTS[i].name);
        }
        if (!reported && HDR.usb_cmds > 0) {
            reported = 1;
            LOG("game is talking to the virtual Toy Pad (fake_opened=%u)", HDR.fake_opened);
            notify("Virtual Toy Pad connected!\nPress %s to open the menu.", cfg.combo_txt);
        }
        if (HDR.passthrough && !passthrough_logged) { LOG("real Toy Pad detected: emulation disabled"); passthrough_logged = 1; }
        if (cfg.notify_menu) {
            if (HDR.menu_open && !last_open) last_line[0] = 0;
            if (HDR.menu_open && strncmp((const char *)HDR.menu_line, last_line, sizeof last_line) && t - last_notify > 600) {
                memcpy(last_line, (const char *)HDR.menu_line, sizeof last_line);
                last_line[sizeof last_line - 1] = 0;
                last_notify = t;
                notify("%s", last_line);
            }
        }
        last_open = HDR.menu_open;
        if (HDR.state_seq != seq && t - last_save > 1500) {
            seq = HDR.state_seq;
            last_save = t;
            save_state();
        }
        /* OUT reports the game sent to the virtual pad -> phone remote LED mirror */
        uint32_t sn = HDR.sniff_n;
        if (sn - sniff_done > LD_SNIFF_MAX) sniff_done = sn - LD_SNIFF_MAX;
        for (; sniff_done != sn; sniff_done++) {
            const uint8_t *e = HDR.sniff[sniff_done % LD_SNIFF_MAX];
            if (!e[0]) web_feed_out(e + 4, e[2] > 32 ? 32 : e[2]);
        }
        for (int i = 0; i < 4; i++) {
            web_service(self, mem);
            usleep(25 * 1000);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ setup */

static int setup(void) {
    struct proc_info pi;
    memset(&pi, 0, sizeof pi);
    if (sys_sdk_proc_info(&pi)) { LOG("sys_sdk_proc_info failed (GoldHEN too old?)"); return -1; }
    LOG("plugin %x.%02x in %s %s (pid %d) base 0x%lx", PLUGIN_VER >> 8, PLUGIN_VER & 0xff, pi.titleid,
        pi.version, pi.pid, (long)pi.base_address);
    if (strncmp(pi.titleid, "CUSA00935", 9) && strncmp(pi.titleid, "CUSA01176", 9)) {
        LOG("not LEGO Dimensions US/EU; staying idle");
        return -1;
    }
    g_base = (intptr_t)pi.base_address;
    if (!g_base || verify_build(g_base) || detect_code_shift(g_base)) {
        notify("LEGO Dimensions: unsupported game version.\nVirtual Toy Pad needs US v1.23 or EU v1.24.");
        return -1;
    }
    apply_fps60(g_base);
    apply_resolution(g_base);

    ld_header *h = &HDR;
    for (int i = 0; i < H_COUNT; i++) h->orig[i] = (uint64_t)REAL[i];
    intptr_t base = g_base, sh = g_code_shift;
    h->fn.usleep = (uint64_t)sceKernelUsleep;
    h->fn.pthread_create = (uint64_t)scePthreadCreate;
    h->fn.dbg_printf = base + LD_OFF_DBG_PRINTF;
    h->fn.dbg_flag_a = base + LD_OFF_DBG_FLAG_A;
    h->fn.dbg_flag_b = base + LD_OFF_DBG_FLAG_B;
    h->fn.dbg_cursor = base + LD_OFF_DBG_CURSOR;
    h->fn.dbg_lineh = base + LD_OFF_DBG_LINEH;
    h->fn.dbg_scale = base + LD_OFF_DBG_SCALE;
    h->fn.dbg_color = base + LD_OFF_DBG_COLOR;
    h->fn.line_print = base + LD_OFF_LINE_PRINT;
    h->fn.flag_render = base + LD_OFF_FLAG_RENDER;
    h->fn.orig_render = base + LD_OFF_RENDER_TARGET;
    h->fn.draw_char_icon = base + LD_OFF_DRAW_CHAR_ICON + sh;
    h->fn.icon_widget = base + LD_OFF_ICON_WIDGET;
    h->fn.cam_mat = base + LD_OFF_CAM_MAT;
    h->fn.cam_pos = base + LD_OFF_CAM_POS;
    h->fn.dci_resume = base + LD_OFF_DRAW_CHAR_ICON + sh + 6;
    h->fn.icon_draw = base + LD_OFF_ICON_DRAW + sh;
    h->fn.icon_resume = base + LD_OFF_ICON_DRAW + sh + 6;
    h->fn.sel_resume = base + LD_OFF_SELECTION_DRAW + sh + 6;
    h->fn.draw_ui_sprite = base + LD_OFF_DRAW_UI_SPRITE + sh;
    h->fn.selection_menu = base + LD_OFF_SELECTION_MENU_PTR;
    h->fn.hud_icon_ret = base + LD_OFF_HUD_ICON_RET + sh;
    h->fn.icon_holder = base + LD_OFF_ICON_HOLDER;
    h->fn.cam_world = base + LD_OFF_CAM_WORLD;
    /* menu layout defaults: identical to the PS5 injector */
    h->grid.cols = 12; h->grid.rows = 5;
    h->grid.dist = 1.2f; h->grid.scale = 0.046f; h->grid.sel_scale = 1.14f;
    h->grid.ox = -0.51f; h->grid.oy = 0.267f; h->grid.dx = 0.093f; h->grid.dy = 0.096f;
    h->border.enable = 2;
    h->border.x_mul = 1.19f; h->border.y_mul = 2.15f;
    h->border.x_add = 0.0f; h->border.y_add = 0.0057f;
    h->border.normal_scale = 0.080f; h->border.selected_scale = 0.0912f;
    h->border.alpha = 1.0f;
    h->border.selected_icon_x = 0.006f;
    h->border.selected_icon_y = -0.005f;
    h->itest.scale = 0.23f; h->itest.dist = 3.0f; h->itest.alpha = 1.0f; h->itest.step = 0.5f;
    h->itest.char_id = 1; h->itest.count = 1;
    h->opt_hold_ms = cfg.hold_ms;
    h->opt_combo = cfg.combo;
    h->opt_flags = (cfg.draw_text ? 1 : 0) | (cfg.force_fake ? 2 : 0);
    ld_pad_init(&h->pad, (uint32_t)time(0) * 2654435761u);
    int nlib = load_library(h);
    if (cfg.restore_pad) restore_pad(h);
    LOG("library: %d saved tags", nlib);

    /* the game has not run yet (plugins load before main), so no thread can be inside the code
     * being patched */
    if (install_render_hook(base)) LOG("render hook not installed (menu will be invisible)");
    if (install_prologue_hook(base, LD_OFF_DRAW_CHAR_ICON + sh, LD_OFF_TRAMPOLINE2, (void *)h_DCICapture))
        LOG("icon capture hook not installed");
    if (install_prologue_hook(base, LD_OFF_ICON_DRAW + sh, LD_OFF_TRAMPOLINE3, (void *)h_IconCapture))
        LOG("full icon capture hook not installed");
    for (int i = 0; i < H_COUNT; i++) hook_got(i);
    return 0;
}

int32_t attr_public plugin_load(int32_t argc, const char *argv[]) {
    (void)argc; (void)argv;
    /* folders copied over FTP can arrive without write access for the game's user */
    mkdir(DATA_DIR, 0777);
    mkdir(TAG_DIR, 0777);
    chmod(DATA_DIR, 0777);
    chmod(TAG_DIR, 0777);
    load_config();
    if (setup()) return 0;
    if (web_start() == 0) LOG("phone remote: http://<PS4 IP>:8765");
    pthread_t thr;
    if (pthread_create(&thr, 0, monitor, 0)) LOG("monitor thread failed to start (tags will not be saved)");
    notify("LEGO Dimensions: Virtual Toy Pad is active.\nHold %s in game for the menu.", cfg.combo_txt);
    return 0;
}

int32_t attr_public plugin_unload(int32_t argc, const char *argv[]) {
    (void)argc; (void)argv;
    return 0;
}

int32_t attr_module_hidden module_start(int64_t argc, const void *args) { (void)argc; (void)args; return 0; }
int32_t attr_module_hidden module_stop(int64_t argc, const void *args) { (void)argc; (void)args; return 0; }
