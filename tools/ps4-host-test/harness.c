/* Host test for the PS4 plugin glue: a synthetic eboot image with the real signature bytes,
 * prologues, render call, FPS/resolution sites and GOT slots; stubbed GoldHEN/system calls. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/mman.h>
#include "GoldHEN.h"
#include "../../src/hook/ldhook.h"
#include "../../src/injector/offsets.h"

static uint8_t *IMG;
#define IMG_SIZE 0x3400000

int sys_sdk_proc_info(struct proc_info *pi) {
    memset(pi, 0, sizeof *pi);
    pi->pid = getpid(); strcpy(pi->titleid, "CUSA01176"); strcpy(pi->version, "01.24");
    pi->base_address = (uint64_t)IMG; return 0;
}
int rw_calls;
int sys_sdk_proc_rw(struct proc_rw *rw) { rw_calls++; memcpy((void *)rw->address, rw->data, rw->length); return 0; }
int sceKernelIsNeoMode(void) { return getenv("NEO") ? 1 : 0; }
int sceKernelMprotect(const void *a, size_t l, int p) { return 0; }
int sceKernelUsleep(unsigned us) { return usleep(us); }
int scePthreadCreate(pthread_t *t, const void *a, void *(*e)(void *), void *arg, const char *n) { return pthread_create(t, 0, e, arg); }
int sceKernelSendNotificationRequest(int d, void *req, size_t sz, int b) { printf("NOTIFY: %s\n", (char *)req + 45); return 0; }
int *__error(void) { return &errno; }

/* "real" system functions: no USB devices, pad reads return a neutral state */
static void *empty_list[1];
int sceUsbdGetDeviceList(void ***l) { *l = empty_list; return 0; }
int sceUsbdFreeDeviceList(void **l, int u) { return 0; }
int sceUsbdGetDeviceDescriptor(void *d, void *o) { return -1; }
int sceUsbdGetBusNumber() { return 0; } int sceUsbdGetDeviceAddress() { return 0; }
int sceUsbdOpenDeviceWithVidPid() { return 0; } int sceUsbdGetConfigDescriptor() { return -1; }
int sceUsbdFreeConfigDescriptor() { return 0; } int sceUsbdKernelDriverActive() { return 0; }
int sceUsbdDetachKernelDriver() { return 0; } int sceUsbdClaimInterface() { return 0; }
int sceUsbdResetDevice() { return 0; } int sceUsbdCheckConnected() { return 0; }
int sceUsbdGetMaxPacketSize() { return 32; } int sceUsbdSubmitTransfer() { return -1; }
int sceUsbdHandleEvents() { return 0; } int sceUsbdFillInterruptTransfer() { return 0; }
int pad_reads;
int scePadReadState(int h, void *d) { pad_reads++; memset(d, 0, 0x40); return 0; }

int32_t plugin_load(int32_t, const char **);
extern ld_header HDR;

static void put(uint64_t off, const void *b, size_t n) { memcpy(IMG + off, b, n); }

static void *game_frames(void *a) {   /* the game's pad polling, through its (patched) GOT */
    typedef int (*padfn)(int, void *);
    uint8_t st[0x40];
    for (;;) { ((padfn) * (uint64_t *)(IMG + LD_IMPORTS[H_PadReadState].got))(1, st); usleep(16000); }
}

int main(int argc, char **argv) {
    IMG = mmap(0, IMG_SIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    int shift = argc > 1 ? atoi(argv[1]) : 0;   /* 0 = US layout, 1 = EU (-0x10) */
    intptr_t sh = shift ? -0x10 : 0;
    for (size_t i = 0; i < sizeof LD_SIGS / sizeof LD_SIGS[0]; i++) put(LD_SIGS[i].off, LD_SIGS[i].bytes, 24);
    static const uint8_t pro[6] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};
    put(LD_OFF_DRAW_CHAR_ICON + sh, pro, 6); put(LD_OFF_ICON_DRAW + sh, pro, 6);
    put(LD_OFF_SELECTION_DRAW + sh, pro, 6); put(LD_OFF_DRAW_UI_SPRITE + sh, pro, 6);
    { uint8_t c[5] = {0xE8}; int32_t r = (int32_t)(LD_OFF_DRAW_CHAR_ICON - LD_OFF_HUD_ICON_RET); memcpy(c + 1, &r, 4); put(LD_OFF_HUD_ICON_RET + sh - 5, c, 5); }
    { uint8_t c[5] = {0xE8}; int32_t r = (int32_t)(LD_OFF_RENDER_TARGET - (LD_OFF_RENDER_CALL + 5)); memcpy(c + 1, &r, 4); put(LD_OFF_RENDER_CALL, c, 5); }
    { static const uint8_t f[9] = {0x8b, 0x18, 0x89, 0x38, 0x83, 0xff, 0x03, 0x74, 0x11}; put(0x6966ad, f, 9); }
    { uint16_t w = 1920, h = 1080; put(0x7148f8, &w, 2); put(0x714902, &h, 2); put(0x714edc, &w, 2); put(0x714ee3, &h, 2); put(0x714f23, &w, 2); put(0x714f28, &h, 2); }
    for (int i = 0; i < H_COUNT; i++) { uint64_t v = 0x1111000 + i; put(LD_IMPORTS[i].got, &v, 8); }

    plugin_load(0, 0);

    int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } else printf("ok: " __VA_ARGS__), printf("\n"); } while (0)
    CHECK(IMG[0x6966ad] == 0xbf && IMG[0x6966ad + 1] == 2, "60 FPS patch written");
    uint16_t w; memcpy(&w, IMG + 0x7148f8, 2);
    CHECK(w == (getenv("RES1440") && getenv("NEO") ? 2560 : 1920), "resolution site 0 = %u", w);
    CHECK(IMG[LD_OFF_RENDER_CALL] == 0xE8, "render call still a call");
    { int32_t r; memcpy(&r, IMG + LD_OFF_RENDER_CALL + 1, 4); CHECK(LD_OFF_RENDER_CALL + 5 + r == LD_OFF_TRAMPOLINE, "render call -> trampoline"); }
    CHECK(IMG[LD_OFF_TRAMPOLINE] == 0xFF && *(uint64_t *)(IMG + LD_OFF_TRAMPOLINE + 6) != 0, "trampoline 1 (render) -> 0x%lx", (long)*(uint64_t *)(IMG + LD_OFF_TRAMPOLINE + 6));
    CHECK(IMG[LD_OFF_DRAW_CHAR_ICON + sh] == 0xE9, "DrawCharacterIcon hooked (%s)", shift ? "EU" : "US");
    CHECK(IMG[LD_OFF_ICON_DRAW + sh] == 0xE9, "icon draw hooked");
    CHECK(IMG[LD_OFF_SELECTION_DRAW + sh] == 0x55, "selection draw left alone");
    { int32_t r; memcpy(&r, IMG + LD_OFF_DRAW_CHAR_ICON + sh + 1, 4); CHECK(LD_OFF_DRAW_CHAR_ICON + sh + 5 + r == LD_OFF_TRAMPOLINE2, "DCI jmp -> trampoline 2"); }
    CHECK(HDR.fn.dci_resume == (uint64_t)IMG + LD_OFF_DRAW_CHAR_ICON + sh + 6, "dci_resume uses the code shift");
    int got_ok = 1;
    for (int i = 0; i < H_COUNT; i++) if (*(uint64_t *)(IMG + LD_IMPORTS[i].got) < 0x2000000) got_ok = 0;
    CHECK(got_ok, "all %d GOT slots redirected", H_COUNT);
    CHECK(HDR.orig[H_PadReadState] == (uint64_t)scePadReadState, "orig pad = real scePadReadState");

    /* the game enumerates USB: the virtual Toy Pad must show up */
    typedef long (*listfn)(void ***);
    void **list = 0;
    long n = ((listfn) * (uint64_t *)(IMG + LD_IMPORTS[H_GetDeviceList].got))(&list);
    CHECK(n == 1 && list && list[0], "virtual Toy Pad enumerated (n=%ld)", n);

    pthread_t t; pthread_create(&t, 0, game_frames, 0);
    sleep(1);
    CHECK(pad_reads > 10 && HDR.pad_calls == (uint32_t)pad_reads, "pad reads pass through the hook (%d)", pad_reads);
    /* GOT watchdog: simulate something rewriting one slot */
    uint64_t junk = 0x2222; memcpy(IMG + LD_IMPORTS[H_HandleEvents].got, &junk, 8);
    if (argc > 2) { printf("serving for %s s (web remote on :8765)\n", argv[2]); sleep(atoi(argv[2])); }
    else sleep(2);
    CHECK(*(uint64_t *)(IMG + LD_IMPORTS[H_HandleEvents].got) != 0x2222, "watchdog re-applied a reverted GOT slot");
    printf("figures on pad:");
    for (int s = 0; s < LD_MAX_SLOTS; s++) if (HDR.pad.fig[s].used) printf(" slot%d=%u", s, HDR.pad.fig[s].id);
    printf("\n%s (%d failures, %d kernel writes)\n", fails ? "FAILED" : "PASSED", fails, rw_calls);
    return fails != 0;
}
