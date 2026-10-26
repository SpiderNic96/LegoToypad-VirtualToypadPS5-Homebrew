/* LEGO Dimensions virtual Toy Pad - PS5 injector payload.
 *
 * Waits for LEGO Dimensions (PS4, CUSA00935 v01.23) to start, injects the hook module
 * (ldhook.bin) into the game process and redirects the game's libSceUsbd / scePadReadState
 * imports to it. Stays resident to persist the virtual tags in /data/ldmod.
 *
 * Send to etaHEN's ELF loader:  socat -u FILE:ldinject.elf TCP:<ps5-ip>:9021
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <ps5/kernel.h>


#include "../hook/ldhook.h"
#include "offsets.h"
#include "pt.h"
#include "blob.h"   /* LDHOOK_BIN / LDHOOK_BIN_len, generated from build/ldhook.bin */
#include "webremote.h"

#define DATA_DIR  "/data/ldmod"
#define TAG_DIR   DATA_DIR "/tags"
#define LOG_FILE  DATA_DIR "/ldinject.log"

static const char *TITLE_IDS[] = {"CUSA00935", "CUSA01176", 0}; /* US 1.23; EU 1.24 (same eboot layout per the 60 FPS patch data) */

/* ---------------------------------------------------------------- utils */

typedef struct { char useless1[45]; char message[3075]; } notify_request_t;
int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);
typedef struct { uint32_t app_id; uint64_t unknown1; uint32_t app_type; char title_id[10]; char unknown2[0x3c]; } app_info_t;
int sceKernelGetAppInfo(pid_t pid, app_info_t *info);

static void logf_(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    printf("[ldinject] %s\n", buf);
    FILE *f = fopen(LOG_FILE, "a");
    if (f) {
        time_t t = time(0);
        struct tm tm;
        localtime_r(&t, &tm);
        fprintf(f, "%02d:%02d:%02d %s\n", tm.tm_hour, tm.tm_min, tm.tm_sec, buf);
        fclose(f);
    }
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

/* ------------------------------------------------------------- config */

static struct {
    uint32_t hold_ms, draw_text, notify_menu, force_fake, restore_pad, capture_selection, fps60, resolution;
    uint32_t combo;
    char combo_txt[64];
} cfg = {250, 1, 0, 0, 0, 0, 1, 1080, 0x2 | 0x4, "L3+R3"};

#define CONFIG_VERSION 2

/* ScePad button names accepted in menu_buttons= */
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

static void write_config(void) {
    FILE *f = fopen(DATA_DIR "/config.ini", "w");
    if (!f) return;
    fprintf(f, "# LEGO Dimensions - Virtual Toy Pad\n"
               "config_version=%d\n\n"
               "# Buttons that open/close the in-game menu, joined with '+'.\n"
               "# Names: L1 R1 L2 R2 L3 R3 CROSS CIRCLE SQUARE TRIANGLE UP DOWN LEFT RIGHT OPTIONS TOUCHPAD\n"
               "menu_buttons=%s\n"
               "# How long the buttons must be held to open the menu (milliseconds)\nhold_ms=%u\n\n"
               "# Draw the menu on screen with the game's debug text renderer (1/0)\ndraw_text=%u\n"
               "# Also show the highlighted menu entry as a system notification (1/0)\nnotify_menu=%u\n"
               "# Use the virtual Toy Pad even when a real one is plugged in (1/0)\nforce_fake=%u\n"
               "# Put back the figures from the last session when the game starts (1/0)\nrestore_pad=%u\n"
               "# Diagnostic capture of Characters UI (1/0; disabled by default)\ncapture_selection=%u\n"
               "# Patch the engine to target 60 FPS instead of 30 (connorh315's patch; 1/0)\nfps60=%u\n"
               "# Internal render resolution: 1080 (game default), 1440 or 2160\nresolution=%u\n",
            CONFIG_VERSION, cfg.combo_txt, cfg.hold_ms, cfg.draw_text, cfg.notify_menu, cfg.force_fake,
            cfg.restore_pad, cfg.capture_selection, cfg.fps60, cfg.resolution);
    fclose(f);
}

static void load_config(void) {
    FILE *f = fopen(DATA_DIR "/config.ini", "r");
    if (!f) { write_config(); return; }
    char line[256];
    unsigned ver = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned v;
        char txt[64];
        if (line[0] == '#') continue;
        if (sscanf(line, "config_version=%u", &v) == 1) ver = v;
        else if (sscanf(line, "hold_ms=%u", &v) == 1) cfg.hold_ms = v;
        else if (sscanf(line, "draw_text=%u", &v) == 1) cfg.draw_text = v;
        else if (sscanf(line, "notify_menu=%u", &v) == 1) cfg.notify_menu = v;
        else if (sscanf(line, "force_fake=%u", &v) == 1) cfg.force_fake = v;
        else if (sscanf(line, "restore_pad=%u", &v) == 1) cfg.restore_pad = v;
        else if (sscanf(line, "capture_selection=%u", &v) == 1) cfg.capture_selection = v;
        else if (sscanf(line, "fps60=%u", &v) == 1) cfg.fps60 = v;
        else if (sscanf(line, "resolution=%u", &v) == 1) cfg.resolution = v;
        else if (sscanf(line, "menu_buttons=%63[^\r\n]", txt) == 1) {
            uint32_t m = parse_combo(txt);
            if (m) { cfg.combo = m; snprintf(cfg.combo_txt, sizeof cfg.combo_txt, "%s", txt); }
        }
    }
    fclose(f);
    if (ver < CONFIG_VERSION) {   /* old config (menu was on TRIANGLE): upgrade it */
        cfg.hold_ms = 250;
        cfg.combo = 0x2 | 0x4;
        snprintf(cfg.combo_txt, sizeof cfg.combo_txt, "L3+R3");
        write_config();
        LOG("config upgraded to version %d", CONFIG_VERSION);
    }
}

/* ------------------------------------------------------- process lookup */

static pid_t find_game(char title[10]) {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
    size_t sz = 0;
    if (sysctl(mib, 4, 0, &sz, 0, 0)) return -1;
    void *buf = malloc(sz);
    if (!buf) return -1;
    pid_t found = -1;
    if (!sysctl(mib, 4, buf, &sz, 0, 0)) {
        for (char *p = buf; p < (char *)buf + sz;) {
            struct kinfo_proc *ki = (struct kinfo_proc *)p;
            p += ki->ki_structsize;
            if (strcmp(ki->ki_comm, "eboot.bin")) continue;
            app_info_t ai;
            memset(&ai, 0, sizeof ai);
            if (sceKernelGetAppInfo(ki->ki_pid, &ai)) continue;
            /* the title id offset differs between firmwares: search the whole record */
            const char *raw = (const char *)&ai;
            for (int i = 0; TITLE_IDS[i]; i++)
                for (size_t o = 0; o + 9 <= sizeof ai; o++)
                    if (!memcmp(raw + o, TITLE_IDS[i], 9)) {
                        found = ki->ki_pid;
                        memcpy(title, TITLE_IDS[i], 9);
                        title[9] = 0;
                        break;
                    }
        }
    }
    free(buf);
    return found;
}

static int proc_alive(pid_t pid) {
    char title[10];
    return kernel_get_proc(pid) != 0 && find_game(title) == pid;
}

/* ------------------------------------------------------ symbol resolving */

static int find_handle(pid_t pid, const char *lib, uint32_t *h) {
    char name[64];
    const char *suffix[] = {".sprx", ".prx", "_web.sprx", "_sys.sprx", 0};
    for (int i = 0; suffix[i]; i++) {
        snprintf(name, sizeof name, "%s%s", lib, suffix[i]);
        if (!kernel_dynlib_handle(pid, name, h)) return 0;
    }
    return -1;
}

static intptr_t resolve(pid_t pid, const char *lib, const char *nid) {
    uint32_t h;
    intptr_t a = 0;
    if (!find_handle(pid, lib, &h)) a = kernel_dynlib_resolve(pid, h, nid);
    if (!a && !strcmp(lib, "libkernel")) {
        a = kernel_dynlib_resolve(pid, 0x1, nid);
        if (!a) a = kernel_dynlib_resolve(pid, 0x2001, nid);
    }
    return a;
}

/* ------------------------------------------------------------ injection */

static int verify_build(pid_t pid, intptr_t base) {
    for (size_t i = 0; i < sizeof LD_SIGS / sizeof LD_SIGS[0]; i++) {
        uint8_t b[24];
        if (kernel_proc_copyout(pid, base + LD_SIGS[i].off, b, sizeof b)) return -1;
        if (memcmp(b, LD_SIGS[i].bytes, sizeof b)) {
            LOG("signature mismatch at +0x%lx (unsupported game version?)", (long)LD_SIGS[i].off);
            return -1;
        }
    }
    return 0;
}

static int write_got(pid_t pid, intptr_t addr, uint64_t val) {
    intptr_t page = addr & ~0x3FFFL;
    int prot = kernel_get_vmem_protection(pid, page, 0x4000);
    if (kernel_mprotect(pid, page, 0x4000, PROT_READ | PROT_WRITE)) LOG("mprotect GOT RW failed");
    int r = kernel_proc_copyin(pid, &val, addr, sizeof val);
    if (prot > 0) kernel_mprotect(pid, page, 0x4000, prot);
    uint64_t chk = 0;
    kernel_proc_copyout(pid, addr, &chk, sizeof chk);
    return (r || chk != val) ? -1 : 0;
}

/* tags saved on disk -> header library; returns count */
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
    /* the game reads the pad state after WAKE; drop the queued events, WAKE re-announces them */
    h->pad.q_head = h->pad.q_tail = 0;
}

long pt_call(pid_t pid, intptr_t addr, ...);

/* mmap RWX in the target. Must be called while attached. Temporarily raises the target's
 * privileges the same way elfldr does, then restores them. Returns 0 on failure. */
static intptr_t alloc_exec(pid_t pid, size_t size) {
    static const uint8_t allcaps[16] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    uint8_t caps[16];
    uint64_t authid = kernel_get_ucred_authid(pid);
    kernel_get_ucred_caps(pid, caps);
    kernel_set_ucred_caps(pid, allcaps);
    kernel_set_ucred_authid(pid, 0x4800000000010003L);

    const int rwx = PROT_READ | PROT_WRITE | PROT_EXEC;
    intptr_t mem = pt_mmap(pid, 0, size, rwx, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (mem == -1 || !mem) {
        LOG("mmap RWX failed (errno %d), trying RW + mprotect", pt_errno(pid));
        mem = pt_mmap(pid, 0, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        if (mem != -1 && mem) {
            if (pt_mprotect(pid, mem, size, rwx)) {
                LOG("mprotect RWX failed (errno %d), trying kernel vm_entry", pt_errno(pid));
                kernel_mprotect(pid, mem, size, rwx);
            }
        }
    }
    kernel_set_ucred_authid(pid, authid);
    kernel_set_ucred_caps(pid, caps);
    if (mem == -1 || !mem) { LOG("remote mmap failed"); return 0; }
    int prot = kernel_get_vmem_protection(pid, mem, size);
    /* the PS4 process silently drops PROT_EXEC: force it on the vm_map entry from the kernel */
    for (int i = 0; i < 5 && !(prot & PROT_EXEC); i++) {
        if (kernel_mprotect(pid, mem, size, rwx)) LOG("kernel vm_entry mprotect failed (try %d)", i);
        prot = kernel_get_vmem_protection(pid, mem, size);
    }
    if (prot >= 0 && !(prot & PROT_EXEC)) {
        LOG("memory at 0x%lx is not executable (prot=%d)", (long)mem, prot);
        pt_munmap(pid, mem, size);
        return 0;
    }
    return mem;
}

static int write_code(pid_t pid, intptr_t addr, const void *buf, size_t len) {
    intptr_t page = addr & ~0x3FFFL;
    size_t span = ((addr + len + 0x3FFF) & ~0x3FFFL) - page;
    int prot = kernel_get_vmem_protection(pid, page, span);
    kernel_mprotect(pid, page, span, PROT_READ | PROT_WRITE | PROT_EXEC);
    int r = kernel_proc_copyin(pid, buf, addr, len);
    if (prot > 0) kernel_mprotect(pid, page, span, prot);
    uint8_t chk[32];
    kernel_proc_copyout(pid, addr, chk, len);
    return (r || memcmp(chk, buf, len)) ? -1 : 0;
}

/* Code shift: EU CUSA01176 v01.24 is the same build layout as US v01.23 except that the code from
 * somewhere after the render target (0xc9c870) onward sits 0x10 bytes earlier. Every data offset and
 * GOT slot is unchanged. The shift is detected from the DrawCharacterIcon prologue and applied to the
 * five code offsets past that point (DrawCharacterIcon, icon draw, selection draw, UI sprite draw and
 * the HUD portrait return address). Verified against a CUSA01176 v01.24 memory image. */
static intptr_t g_code_shift;

static int detect_code_shift(pid_t pid, intptr_t base) {
    static const uint8_t pro[6] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};
    static const intptr_t fns[4] = {LD_OFF_DRAW_CHAR_ICON, LD_OFF_ICON_DRAW, LD_OFF_SELECTION_DRAW, LD_OFF_DRAW_UI_SPRITE};
    static const intptr_t shifts[2] = {0, -0x10};
    for (int s = 0; s < 2; s++) {
        int ok = 1;
        for (int i = 0; i < 4 && ok; i++) {
            uint8_t b[6];
            if (kernel_proc_copyout(pid, base + fns[i] + shifts[s], b, 6)) return -1;
            ok = !memcmp(b, pro, 6) || (i < 3 && b[0] == 0xE9);   /* E9 = already hooked by us */
        }
        /* the HUD portrait call must return into DrawCharacterIcon's caller */
        if (ok) {
            uint8_t c[5];
            int32_t rel;
            intptr_t ret = LD_OFF_HUD_ICON_RET + shifts[s];
            if (kernel_proc_copyout(pid, base + ret - 5, c, 5) || c[0] != 0xE8) ok = 0;
            else { memcpy(&rel, c + 1, 4); ok = ret + rel == LD_OFF_DRAW_CHAR_ICON + shifts[s]; }
        }
        if (ok) {
            g_code_shift = shifts[s];
            LOG("code layout: %s (shift %ld)", s ? "EU v01.24" : "US v01.23", (long)g_code_shift);
            return 0;
        }
    }
    LOG("code layout: icon functions not found at the known offsets; refusing to inject");
    return -1;
}

/* 60 FPS: connorh315's patch from shadPS4's ps4_cheats (LEGODimensions.xml), same site for
 * CUSA00935 v01.23 and CUSA01176 v01.24. shadPS4 patch addresses include the 0x400000 image base,
 * so the patch's 0xa966ad is +0x6966ad here. The site is the engine's SetFPSMode(edi):
 *   stock:   mov ebx,[rax] ; mov [rax],edi ; cmp edi,3 ; je ...
 *   patched: mov edi,2     ; mov [rax],edi ; nop ; nop      (always FPS mode 2 = 60 FPS)
 * Verified against a CUSA01176 v01.24 memory image. */
#define LD_OFF_FPS60 0x6966ad
static const uint8_t FPS60_STOCK[9] = {0x8b, 0x18, 0x89, 0x38, 0x83, 0xff, 0x03, 0x74, 0x11};
static const uint8_t FPS60_BYTES[9] = {0xbf, 0x02, 0x00, 0x00, 0x00, 0x89, 0x38, 0x90, 0x90};

static void apply_fps60(pid_t pid, intptr_t base) {
    if (!cfg.fps60) { LOG("60 FPS patch disabled in config"); return; }
    uint8_t cur[9];
    if (kernel_proc_copyout(pid, base + LD_OFF_FPS60, cur, sizeof cur)) { LOG("60 FPS: read failed"); return; }
    if (!memcmp(cur, FPS60_BYTES, sizeof cur)) { LOG("60 FPS patch already present"); return; }
    if (memcmp(cur, FPS60_STOCK, sizeof cur)) {
        LOG("60 FPS: unexpected bytes %02x %02x %02x %02x %02x %02x %02x %02x %02x; not patching", cur[0], cur[1],
            cur[2], cur[3], cur[4], cur[5], cur[6], cur[7], cur[8]);
        return;
    }
    LOG("60 FPS: original bytes %02x %02x %02x %02x %02x %02x %02x %02x %02x", cur[0], cur[1], cur[2], cur[3],
        cur[4], cur[5], cur[6], cur[7], cur[8]);
    if (write_code(pid, base + LD_OFF_FPS60, FPS60_BYTES, sizeof FPS60_BYTES)) LOG("60 FPS patch write failed");
    else LOG("60 FPS patch applied at +0x%x", LD_OFF_FPS60);
}

/* Internal resolution: connorh315's patch, same six sites for v01.23 and v01.24. Each site holds a
 * 16-bit width (0x0780 = 1920) or height (0x0438 = 1080) immediate; only rewritten if it still holds
 * the stock value, so an unexpected build is left alone. */
static const struct { uint32_t off; uint16_t stock; uint8_t is_w; } RES_SITES[6] = {
    {0x7148f8, 1920, 1}, {0x714902, 1080, 0}, {0x714edc, 1920, 1},   /* patch addresses - 0x400000 */
    {0x714ee3, 1080, 0}, {0x714f23, 1920, 1}, {0x714f28, 1080, 0}};

static void apply_resolution(pid_t pid, intptr_t base) {
    uint16_t w, h;
    switch (cfg.resolution) {
    case 1080: LOG("resolution: game default (1920x1080)"); return;
    case 1440: w = 2560; h = 1440; break;
    case 2160: w = 3840; h = 2160; break;
    default: LOG("resolution=%u not supported (use 1080, 1440 or 2160); keeping default", cfg.resolution); return;
    }
    uint16_t cur[6];
    for (int i = 0; i < 6; i++) {
        if (kernel_proc_copyout(pid, base + RES_SITES[i].off, &cur[i], 2)) { LOG("resolution: read failed"); return; }
        uint16_t want = RES_SITES[i].is_w ? w : h;
        if (cur[i] != RES_SITES[i].stock && cur[i] != want) {
            LOG("resolution: +0x%x holds 0x%04x, expected 0x%04x; not patching", RES_SITES[i].off, cur[i],
                RES_SITES[i].stock);
            return;
        }
    }
    for (int i = 0; i < 6; i++) {
        uint16_t want = RES_SITES[i].is_w ? w : h;
        if (cur[i] != want && write_code(pid, base + RES_SITES[i].off, &want, 2)) {
            LOG("resolution: write failed at +0x%x", RES_SITES[i].off);
            return;
        }
    }
    LOG("resolution: internal render set to %ux%u", w, h);
}

static int install_render_hook(pid_t pid, intptr_t base, intptr_t target) {
    uint8_t call[5], pad[16];
    kernel_proc_copyout(pid, base + LD_OFF_RENDER_CALL, call, 5);
    kernel_proc_copyout(pid, base + LD_OFF_TRAMPOLINE, pad, 16);
    int32_t rel = (int32_t)(LD_OFF_RENDER_TARGET - (LD_OFF_RENDER_CALL + 5));
    int32_t cur;
    memcpy(&cur, call + 1, 4);
    if (call[0] != 0xE8) { LOG("render call site changed: %02x", call[0]); return -1; }
    if (cur != rel && cur != (int32_t)(LD_OFF_TRAMPOLINE - (LD_OFF_RENDER_CALL + 5))) { LOG("render call target unexpected"); return -1; }
    /* trampoline: jmp qword [rip+0]; .quad target */
    uint8_t tr[14] = {0xFF, 0x25, 0, 0, 0, 0};
    memcpy(tr + 6, &target, 8);
    int is_zero = 1, is_ours = (pad[0] == 0xFF && pad[1] == 0x25);
    for (int i = 0; i < 14; i++) if (pad[i]) is_zero = 0;
    if (!is_zero && !is_ours) { LOG("trampoline area not free"); return -1; }
    if (write_code(pid, base + LD_OFF_TRAMPOLINE, tr, sizeof tr)) { LOG("trampoline write failed"); return -1; }
    int32_t nrel = (int32_t)(LD_OFF_TRAMPOLINE - (LD_OFF_RENDER_CALL + 5));
    uint8_t ncall[5] = {0xE8};
    memcpy(ncall + 1, &nrel, 4);
    if (write_code(pid, base + LD_OFF_RENDER_CALL, ncall, 5)) { LOG("render call patch failed"); return -1; }
    LOG("render hook installed (trampoline 0x%lx -> 0x%lx)", (long)(base + LD_OFF_TRAMPOLINE), (long)target);
    return 0;
}

/* inline hook on DrawCharacterIcon: replace "push rbp; mov rbp,rsp; push r15" with a jump to the capture stub */
static int install_dci_hook(pid_t pid, intptr_t base, intptr_t target) {
    static const uint8_t prologue[6] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};
    uint8_t cur[6], pad[16];
    kernel_proc_copyout(pid, base + (LD_OFF_DRAW_CHAR_ICON + g_code_shift), cur, 6);
    kernel_proc_copyout(pid, base + LD_OFF_TRAMPOLINE2, pad, 16);
    if (memcmp(cur, prologue, 6) && !(cur[0] == 0xE9)) { LOG("DrawCharacterIcon prologue changed"); return -1; }
    int is_zero = 1;
    for (int i = 0; i < 14; i++) if (pad[i]) is_zero = 0;
    if (!is_zero && !(pad[0] == 0xFF && pad[1] == 0x25)) { LOG("trampoline 2 area not free"); return -1; }
    uint8_t tr[14] = {0xFF, 0x25, 0, 0, 0, 0};
    memcpy(tr + 6, &target, 8);
    if (write_code(pid, base + LD_OFF_TRAMPOLINE2, tr, sizeof tr)) { LOG("trampoline 2 write failed"); return -1; }
    int32_t rel = (int32_t)(LD_OFF_TRAMPOLINE2 - ((LD_OFF_DRAW_CHAR_ICON + g_code_shift) + 5));
    uint8_t jmp[6] = {0xE9, 0, 0, 0, 0, 0x90};
    memcpy(jmp + 1, &rel, 4);
    if (write_code(pid, base + (LD_OFF_DRAW_CHAR_ICON + g_code_shift), jmp, 6)) { LOG("DrawCharacterIcon patch failed"); return -1; }
    LOG("icon capture hook installed");
    return 0;
}

/* generic version of install_dci_hook for any function starting with push rbp; mov rbp,rsp; push r15 */
static int install_prologue_hook(pid_t pid, intptr_t base, intptr_t fn_off, intptr_t tramp_off, intptr_t target) {
    static const uint8_t prologue[6] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};
    uint8_t cur[6], pad[16];
    kernel_proc_copyout(pid, base + fn_off, cur, 6);
    kernel_proc_copyout(pid, base + tramp_off, pad, 16);
    if (memcmp(cur, prologue, 6) && cur[0] != 0xE9) { LOG("prologue at +0x%lx changed", (long)fn_off); return -1; }
    int is_zero = 1;
    for (int i = 0; i < 14; i++) if (pad[i]) is_zero = 0;
    if (!is_zero && !(pad[0] == 0xFF && pad[1] == 0x25)) { LOG("trampoline at +0x%lx not free", (long)tramp_off); return -1; }
    uint8_t tr[14] = {0xFF, 0x25, 0, 0, 0, 0};
    memcpy(tr + 6, &target, 8);
    if (write_code(pid, base + tramp_off, tr, sizeof tr)) return -1;
    int32_t rel = (int32_t)(tramp_off - (fn_off + 5));
    uint8_t jmp[6] = {0xE9, 0, 0, 0, 0, 0x90};
    memcpy(jmp + 1, &rel, 4);
    if (write_code(pid, base + fn_off, jmp, 6)) return -1;
    LOG("prologue hook installed at +0x%lx", (long)fn_off);
    return 0;
}

static intptr_t inject(pid_t pid, intptr_t base) {
    ld_header *h = (ld_header *)LDHOOK_BIN;
    if (h->magic != LDH_MAGIC || h->version != LDH_VERSION) { LOG("bad blob"); return 0; }
    size_t size = (h->size + 0x3FFF) & ~0x3FFFUL;
    uint8_t *img = calloc(1, size);
    memcpy(img, LDHOOK_BIN, LDHOOK_BIN_len);
    h = (ld_header *)img;

    /* resolve the real import targets */
    for (int i = 0; i < H_COUNT; i++) {
        const ld_import *im = &LD_IMPORTS[i];
        intptr_t a = resolve(pid, im->lib, im->nid);
        uint64_t got = 0;
        kernel_proc_copyout(pid, base + im->got, &got, sizeof got);
        if (!a) { LOG("cannot resolve %s (GOT=0x%lx)", im->name, (long)got); free(img); return 0; }
        if (got != (uint64_t)a) LOG("%s: GOT 0x%lx != resolved 0x%lx (lazy binding?)", im->name, (long)got, (long)a);
        h->orig[i] = a;
    }
    h->fn.usleep = resolve(pid, LD_USLEEP.lib, LD_USLEEP.nid);
    h->fn.dbg_printf = base + LD_OFF_DBG_PRINTF;
    h->fn.dbg_flag_a = base + LD_OFF_DBG_FLAG_A;
    h->fn.dbg_flag_b = base + LD_OFF_DBG_FLAG_B;
    h->fn.dbg_cursor = base + LD_OFF_DBG_CURSOR;
    h->fn.dbg_lineh = base + LD_OFF_DBG_LINEH;
    h->fn.dbg_scale = base + LD_OFF_DBG_SCALE;
    h->fn.dbg_color = base + LD_OFF_DBG_COLOR;
    h->fn.pthread_create = resolve(pid, "libkernel", "6UgtwV+0zb4");
    h->fn.line_print = base + LD_OFF_LINE_PRINT;
    h->fn.flag_render = base + LD_OFF_FLAG_RENDER;
    h->fn.orig_render = base + LD_OFF_RENDER_TARGET;
    h->fn.draw_char_icon = base + (LD_OFF_DRAW_CHAR_ICON + g_code_shift);
    h->fn.icon_widget = base + LD_OFF_ICON_WIDGET;
    h->fn.cam_mat = base + LD_OFF_CAM_MAT;
    h->fn.cam_pos = base + LD_OFF_CAM_POS;
    h->fn.dci_resume = base + (LD_OFF_DRAW_CHAR_ICON + g_code_shift) + 6;
    h->fn.icon_draw = base + (LD_OFF_ICON_DRAW + g_code_shift);
    h->fn.icon_resume = base + (LD_OFF_ICON_DRAW + g_code_shift) + 6;
    h->fn.sel_resume = base + (LD_OFF_SELECTION_DRAW + g_code_shift) + 6;
    h->fn.draw_ui_sprite = base + (LD_OFF_DRAW_UI_SPRITE + g_code_shift);
    h->fn.selection_menu = base + LD_OFF_SELECTION_MENU_PTR;
    h->fn.hud_icon_ret = base + (LD_OFF_HUD_ICON_RET + g_code_shift);
    h->fn.icon_holder = base + LD_OFF_ICON_HOLDER;
    h->fn.cam_world = base + LD_OFF_CAM_WORLD;
    /* Centered portrait picker. Normal and selected icons use a uniform
       3D transform, avoiding the stretching caused by experimental styles. */
    h->grid.cols = 12; h->grid.rows = 5;
    h->grid.dist = 1.2f; h->grid.scale = 0.046f; h->grid.sel_scale = 1.14f;
    h->grid.ox = -0.51f; h->grid.oy = 0.267f; h->grid.dx = 0.093f; h->grid.dy = 0.096f;
    /* Test just one ring first; SQUARE shortcuts in the picker calibrate or
       disable native UI borders without modifying the game's Toy Pad. */
    h->border.enable = 2;
    h->border.x_mul = 1.19f; h->border.y_mul = 2.15f;
    h->border.x_add = 0.0f; h->border.y_add = 0.0057f;
    h->border.normal_scale = 0.080f; h->border.selected_scale = 0.0912f;
    h->border.alpha = 1.0f;
    /* Selected characters need extra spacing relative to the ring, without
       shifting the whole 12-by-5 layout. Positive x = right; negative y = down. */
    h->border.selected_icon_x = 0.006f;
    h->border.selected_icon_y = -0.005f;
    h->itest.scale = 0.23f; h->itest.dist = 3.0f; h->itest.alpha = 1.0f; h->itest.step = 0.5f;
    h->itest.char_id = 1; h->itest.count = 1;
    LOG("scePthreadCreate=0x%lx usleep=0x%lx", (long)h->fn.pthread_create, (long)h->fn.usleep);
    h->opt_hold_ms = cfg.hold_ms;
    h->opt_combo = cfg.combo;
    h->opt_flags = (cfg.draw_text ? 1 : 0) | (cfg.force_fake ? 2 : 0);
    ld_pad_init(&h->pad, (uint32_t)time(0) * 2654435761u);
    int nlib = load_library(h);
    if (cfg.restore_pad) restore_pad(h);
    LOG("library: %d saved tags", nlib);

    /* allocate executable memory inside the game and prove it runs before touching the GOT */
    if (pt_attach(pid)) { LOG("pt_attach failed"); free(img); return 0; }
    intptr_t mem = alloc_exec(pid, size);
    int ok = 0;
    if (mem) {
        if (kernel_proc_copyin(pid, img, mem, size)) LOG("copyin blob failed");
        else {
            long r = pt_call(pid, mem + h->ping_off);
            LOG("blob at 0x%lx (%zu bytes) prot=%d ping=0x%lx", (long)mem, size,
                kernel_get_vmem_protection(pid, mem, size), r);
            ok = (r == LDH_PING);
        }
        if (!ok) pt_munmap(pid, mem, size);
    }
    if (ok) {
        /* code patches while the game is stopped by ptrace, so no thread runs half-written code */
        if (install_render_hook(pid, base, mem + h->render_off)) LOG("render hook not installed (menu will be invisible)");
        if (install_dci_hook(pid, base, mem + h->dci_off)) LOG("icon capture hook not installed");
        if (install_prologue_hook(pid, base, (LD_OFF_ICON_DRAW + g_code_shift), LD_OFF_TRAMPOLINE3, mem + h->icap_off))
            LOG("full icon capture hook not installed");
        if (cfg.capture_selection) {
            if (install_prologue_hook(pid, base, (LD_OFF_SELECTION_DRAW + g_code_shift),
                                      LD_OFF_TRAMPOLINE4, mem + h->sel_off))
                LOG("Characters selection diagnostic hook not installed");
            else LOG("Characters selection diagnostic capture enabled");
        }
    }
    pt_detach(pid, 0);
    if (!ok) { LOG("aborting: blob is not executable in the game process (game left untouched)"); free(img); return 0; }

    /* redirect the imports */
    for (int i = 0; i < H_COUNT; i++)
        if (write_got(pid, base + LD_IMPORTS[i].got, mem + h->hook_off[i]))
            LOG("GOT patch failed for %s", LD_IMPORTS[i].name);
    free(img);
    return mem;
}

/* already injected in this process? (payload re-sent while the game runs) */
static intptr_t find_existing(pid_t pid, intptr_t base) {
    uint64_t got = 0;
    kernel_proc_copyout(pid, base + LD_IMPORTS[H_GetDeviceList].got, &got, sizeof got);
    const ld_header *h = (const ld_header *)LDHOOK_BIN;
    intptr_t mem = (intptr_t)got - h->hook_off[H_GetDeviceList];
    uint32_t hdr[2] = {0, 0};
    if (kernel_proc_copyout(pid, mem, hdr, sizeof hdr) || hdr[0] != LDH_MAGIC) return 0;
    /* A previous hook layout must never be monitored with the current struct offsets. */
    if (hdr[1] != LDH_VERSION) return -1;
    return mem;
}

/* --------------------------------------------------------- persistence */

static void save_state(pid_t pid, intptr_t mem) {
    static ld_header h; /* big: keep off the stack */
    if (kernel_proc_copyout(pid, mem, &h, sizeof h)) return;
    mkdir(DATA_DIR, 0777);
    mkdir(TAG_DIR, 0777);
    for (int i = 0; i < LD_LIB_MAX; i++) {
        ld_libtag *t = &h.lib[i];
        if (!t->used || !t->dirty) continue;
        char path[128];
        snprintf(path, sizeof path, TAG_DIR "/%u.bin", t->id);
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd >= 0) { write(fd, t->data, LD_TAG_SIZE); close(fd); }
        uint8_t zero = 0;
        kernel_proc_copyin(pid, &zero, mem + offsetof(ld_header, lib) + i * sizeof(ld_libtag) + offsetof(ld_libtag, dirty), 1);
    }
    FILE *f = fopen(DATA_DIR "/pad.txt", "w");
    if (f) {
        for (int s = 0; s < LD_MAX_SLOTS; s++)
            if (h.pad.fig[s].used) fprintf(f, "%d=%u\n", s, h.pad.fig[s].id);
        fclose(f);
    }
}

static void monitor(pid_t pid, intptr_t mem, intptr_t base) {
    uint32_t seq = 0, last_open = 0, last_cmds = 0;
    uint64_t last_notify = 0, last_save = 0;
    char last_line[96] = "";
    int reported = 0;
    uint64_t last_status = 0;
    uint32_t sniff_done = 0;
    kernel_proc_copyout(pid, mem + offsetof(ld_header, sniff_n), &sniff_done, 4);
    sniff_done = 0;
    while (proc_alive(pid)) {
        struct {
            uint32_t seq, open;
            char line[96];
            uint32_t passthrough, fake_opened, usb_cmds, pad_calls, pump, delivered;
        } s;
        if (kernel_proc_copyout(pid, mem + offsetof(ld_header, state_seq), &s, sizeof s)) break;
        uint64_t t = now_ms();
        if (t - last_status > 10000) {
            last_status = t;
            LOG("status: opened=%u cmds=%u delivered=%u pump=%u pad_calls=%u passthrough=%u menu=%u seq=%u", s.fake_opened,
                s.usb_cmds, s.delivered, s.pump, s.pad_calls, s.passthrough, s.open, s.seq);
        }
        if (!reported && s.usb_cmds > 0) {
            reported = 1;
            LOG("game is talking to the virtual Toy Pad (fake_opened=%u)", s.fake_opened);
            notify("Virtual Toy Pad connected!\nPress %s to open the menu.", cfg.combo_txt);
        }
        if (s.passthrough && !last_cmds) { LOG("real Toy Pad detected: emulation disabled"); last_cmds = 1; }
        if (cfg.notify_menu) {
            if (s.open && !last_open) last_line[0] = 0;
            if (s.open && strncmp(s.line, last_line, sizeof last_line) && t - last_notify > 600) {
                memcpy(last_line, s.line, sizeof last_line);
                last_notify = t;
                notify("%s", s.line);
            }
        }
        last_open = s.open;
        if (s.seq != seq && t - last_save > 1500) {
            seq = s.seq;
            last_save = t;
            save_state(pid, mem);
        }
        /* developer channel: poke.cmd = "<blob offset hex> <hex bytes>" per line, peek.cmd = "<off> <len>" */
        {
            FILE *pc = fopen(DATA_DIR "/poke.cmd", "r");
            if (pc) {
                char ln[512];
                while (fgets(ln, sizeof ln, pc)) {
                    unsigned long off; char hex[400];
                    if (sscanf(ln, "%lx %399s", &off, hex) != 2) continue;
                    uint8_t b[200]; size_t n = 0;
                    for (size_t i = 0; hex[i] && hex[i + 1] && n < sizeof b; i += 2) {
                        unsigned v; sscanf(hex + i, "%2x", &v); b[n++] = (uint8_t)v;
                    }
                    if (off + n <= LDHOOK_BIN_len + 0x20000) kernel_proc_copyin(pid, b, mem + off, n);
                }
                fclose(pc);
                unlink(DATA_DIR "/poke.cmd");
            }
            pc = fopen(DATA_DIR "/peek.cmd", "r");
            if (pc) {
                unsigned long off, len;
                char kind[4] = "R";
                /* "<off> <len>" = blob relative, "A <addr> <len>" = absolute, "B <off> <len>" = eboot relative */
                long pos = ftell(pc);
                if (fscanf(pc, "%1[AB]", kind) != 1) { fseek(pc, pos, SEEK_SET); kind[0] = 'R'; }
                if (fscanf(pc, "%lx %lx", &off, &len) == 2 && len <= 0x10000) {
                    static uint8_t pb[0x10000];
                    intptr_t at = kind[0] == 'A' ? (intptr_t)off : kind[0] == 'B' ? base + off : mem + off;
                    memset(pb, 0xEE, len);
                    kernel_proc_copyout(pid, at, pb, len);
                    FILE *po = fopen(DATA_DIR "/peek.bin", "wb");
                    if (po) { fwrite(pb, 1, len, po); fclose(po); }
                }
                fclose(pc);
                unlink(DATA_DIR "/peek.cmd");
            }
        }
        /* drain the USB sniffer ring */
        uint32_t sn = 0;
        kernel_proc_copyout(pid, mem + offsetof(ld_header, sniff_n), &sn, 4);
        if (sn != sniff_done) {
            struct stat sst;
            int big = !stat(DATA_DIR "/usb_sniff.log", &sst) && sst.st_size > 2 * 1024 * 1024;
            FILE *f = fopen(DATA_DIR "/usb_sniff.log", big ? "w" : "a");
            if (sn - sniff_done > LD_SNIFF_MAX) { if (f) fprintf(f, "... %u entries lost\n", sn - sniff_done - LD_SNIFF_MAX); sniff_done = sn - LD_SNIFF_MAX; }
            for (; sniff_done != sn; sniff_done++) {
                uint8_t e[36];
                kernel_proc_copyout(pid, mem + offsetof(ld_header, sniff) + (sniff_done % LD_SNIFF_MAX) * 36, e, 36);
                if (!e[0]) web_feed_out(e + 4, e[2] > 32 ? 32 : e[2]);
                if (!f) continue;
                fprintf(f, "%llu %s %s ", (unsigned long long)t, e[1] ? "REAL" : "VIRT", e[0] ? "IN " : "OUT");
                for (int k = 0; k < e[2]; k++) fprintf(f, "%02x", e[4 + k]);
                fprintf(f, "\n");
            }
            if (f) fclose(f);
        }
        usleep(100 * 1000);
    }
    LOG("game exited");
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    mkdir(DATA_DIR, 0777);
    mkdir(TAG_DIR, 0777);
    load_config();
    {
        FILE *pf = fopen(DATA_DIR "/ldinject.pid", "r");
        int old = 0;
        if (pf) { if (fscanf(pf, "%d", &old) != 1) old = 0; fclose(pf); }
        if (old > 0 && old != getpid() && kernel_get_proc(old)) {
            LOG("stopping previous injector pid %d", old);
            kill(old, SIGKILL);
        }
        pf = fopen(DATA_DIR "/ldinject.pid", "w");
        if (pf) { fprintf(pf, "%d\n", getpid()); fclose(pf); }
    }
    LOG("started (blob %u bytes)", (unsigned)LDHOOK_BIN_len);
    web_start();
    notify("LEGO Dimensions: Virtual Toy Pad is active.\nStart the game.");
    for (;;) {
        char title[10];
        pid_t pid = find_game(title);
        if (pid <= 0) { usleep(250 * 1000); continue; }
        LOG("found %s pid %d", title, pid);
        intptr_t base = kernel_dynlib_mapbase_addr(pid, 0);
        /* wait until the executable is mapped and relocated */
        for (int i = 0; i < 100 && (!base || verify_build(pid, base)); i++) {
            usleep(100 * 1000);
            base = kernel_dynlib_mapbase_addr(pid, 0);
        }
        if (!base || verify_build(pid, base)) {
            LOG("unsupported build or base not found (base=0x%lx)", (long)base);
            notify("LEGO Dimensions: unsupported game version (US v1.23 or EU v1.24 required).");
            while (proc_alive(pid)) sleep(1);
            continue;
        }
#ifndef PROBE_ONLY
        apply_fps60(pid, base);
        apply_resolution(pid, base);
#endif
        /* wait for the dynamic linker to bind the imports we are going to replace */
        for (int i = 0; i < 200; i++) {
            uint64_t got = 0;
            intptr_t want = resolve(pid, LD_IMPORTS[H_GetDeviceList].lib, LD_IMPORTS[H_GetDeviceList].nid);
            kernel_proc_copyout(pid, base + LD_IMPORTS[H_GetDeviceList].got, &got, sizeof got);
            if (want && (got == (uint64_t)want || find_existing(pid, base))) break;
            usleep(50 * 1000);
        }
#ifdef PROBE_ONLY
        LOG("PROBE: base=0x%lx build verified", (long)base);
        for (int i = 0; i < H_COUNT; i++) {
            uint64_t got = 0;
            kernel_proc_copyout(pid, base + LD_IMPORTS[i].got, &got, sizeof got);
            LOG("PROBE: %-28s GOT=0x%lx resolved=0x%lx", LD_IMPORTS[i].name, (long)got,
                (long)resolve(pid, LD_IMPORTS[i].lib, LD_IMPORTS[i].nid));
        }
        LOG("PROBE: usleep=0x%lx existing=0x%lx", (long)resolve(pid, LD_USLEEP.lib, LD_USLEEP.nid), (long)find_existing(pid, base));
        { uint32_t hh; for (const char **l = (const char *[]){"libkernel", "libSceUsbd", "libScePad", 0}; *l; l++)
            LOG("PROBE: handle %s -> %d (%u)", *l, find_handle(pid, *l, &hh), hh); }
        notify("LD probe finished, see %s", LOG_FILE);
        return 0;
#endif
        load_config();
        intptr_t mem = find_existing(pid, base);
        if (mem < 0) {
            LOG("incompatible Virtual Toy Pad already active; restart game before upgrading");
            notify("LEGO Dimensions: close and restart the game to update the mod.");
            while (proc_alive(pid)) sleep(1);
            continue;
        }
        if (mem) { detect_code_shift(pid, base); LOG("already injected at 0x%lx, monitoring", (long)mem); }
        else {
            uint64_t got = 0;
            intptr_t original = resolve(pid, LD_IMPORTS[H_GetDeviceList].lib,
                                        LD_IMPORTS[H_GetDeviceList].nid);
            kernel_proc_copyout(pid, base + LD_IMPORTS[H_GetDeviceList].got, &got, sizeof got);
            if (!original || got != (uint64_t)original) {
                LOG("USB GOT already modified or unresolved (0x%lx != 0x%lx); refusing duplicate injection",
                    (long)got, (long)original);
                notify("LEGO Dimensions: restart game before changing Virtual Toy Pad versions.");
                while (proc_alive(pid)) sleep(1);
                continue;
            }
            mem = detect_code_shift(pid, base) ? 0 : inject(pid, base);
        }
        if (!mem) {
            notify("LEGO Dimensions: failed to inject the Virtual Toy Pad (see %s)", LOG_FILE);
            while (proc_alive(pid)) sleep(1);
            continue;
        }
        web_attach(pid, mem);
        monitor(pid, mem, base);
        web_detach();
    }
    return 0;
}
