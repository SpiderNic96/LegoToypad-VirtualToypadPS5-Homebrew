/* Shared layout between the in-game hook module (blob) and the PS5 injector.
 * The blob is a flat, position independent image. It starts with ld_header. */
#pragma once
#include <stdint.h>
#include "../core/ldproto.h"

#if defined(__clang__) && !defined(LD_INJECTOR)
#pragma GCC visibility push(hidden)   /* blob: every symbol is local, no GOT */
#endif

#define LDH_MAGIC   0x444D444Cu /* "LDMD" */
#define LDH_VERSION 20
#define LDH_PING    0x1D0CA7u

/* Hooked imports, in this order (index into hook_off[] / orig[]) */
enum {
    H_GetDeviceList, H_FreeDeviceList, H_GetDeviceDescriptor, H_GetBusNumber,
    H_GetDeviceAddress, H_OpenDeviceWithVidPid, H_GetConfigDescriptor,
    H_FreeConfigDescriptor, H_KernelDriverActive, H_DetachKernelDriver,
    H_ClaimInterface, H_ResetDevice, H_CheckConnected, H_GetMaxPacketSize,
    H_SubmitTransfer, H_HandleEvents, H_PadReadState, H_FillInterruptTransfer,
    H_COUNT
};

/* Game functions/addresses (relative to eboot base) the blob may call. Filled by injector. */
typedef struct {
    uint64_t usleep;            /* sceKernelUsleep (from GOT) */
    uint64_t dbg_printf;        /* 0x606980 debug text printf */
    uint64_t dbg_flag_a;        /* 0x2ff3319 */
    uint64_t dbg_flag_b;        /* 0x2ff3318 */
    uint64_t dbg_cursor;        /* 0x2e6ba9c x,y floats */
    uint64_t dbg_lineh;         /* 0x2e6baa4 */
    uint64_t dbg_scale;         /* 0x2e6ba94 sx,sy */
    uint64_t dbg_color;         /* 0x2ff3300 */
    uint64_t pthread_create;    /* scePthreadCreate (libkernel) */
    uint64_t line_print;        /* 0x5a79b0 debug line printer: (ctx, argb, fmt, ...) */
    uint64_t flag_render;       /* 0x2ff3316 "inside debug render pass" */
    uint64_t orig_render;       /* 0xc9c870 original callee of the hooked render call */
    uint64_t draw_char_icon;    /* 0x171bfd0 DrawCharacterIcon(widget, charId, mat4*, mode, a, b, alpha) */
    uint64_t icon_widget;       /* 0x327ea30 global widget pointer used by the game for icons */
    uint64_t cam_mat;           /* 0x2fb2250 camera matrix (4x4 floats) */
    uint64_t cam_pos;           /* 0x2fb23f0 camera position */
    uint64_t dci_resume;        /* 0x171bfd6: DrawCharacterIcon after the displaced prologue */
    uint64_t hud_icon_ret;      /* 0x16e5eb0: return address of the HUD portrait DrawCharacterIcon call */
    uint64_t icon_holder;       /* 0x327e4e0: MechIconTextureHolder* (collection vector at +0xa8) */
    uint64_t cam_world;         /* 0x2ff2df0: active camera world matrix (rows: right, up, forward, position) */
    uint64_t icon_draw;         /* 0x16e4760: full UI icon draw (background, border, glow, portrait) */
    uint64_t icon_resume;       /* 0x16e4766: icon_draw after the displaced prologue */
    uint64_t sel_resume;        /* 0x14c9696: MechSelectionMenus icon-drawing routine */
    uint64_t draw_ui_sprite;    /* 0x1848b50: original UI sprite renderer */
    uint64_t selection_menu;    /* 0x327e638: pointer to MechSelectionMenus */
} ld_gamefn;

/* one captured call of the full UI icon drawer: xmm0-5, then the 6 GPR args, ret, and 0x80 bytes of stack args */
typedef struct {
    uint8_t blk[0x120];   /* +0x00 xmm0-5, +0x68 r9 r8 rcx rdx rsi rdi, +0x98 ret, +0xa0 stack args */
    uint32_t hits, pad_;
} ld_iconcap;
#define LD_ICAP_MAX 16

/* portrait grid layout, in the HUD portrait's coordinate space (live-tunable) */
typedef struct {
    volatile uint32_t preview;   /* draw the grid even when the menu is closed (calibration) */
    volatile uint32_t cols, rows;
    volatile float ox, oy;       /* top-left cell offset from the HUD portrait */
    volatile float dx, dy;       /* cell spacing */
    volatile float scale;        /* icon size (camera-plane units) */
    volatile float sel_scale;    /* extra size of the selected cell */
    volatile uint32_t drawn;     /* readback: cells drawn last frame */
    volatile float dist;         /* distance of the grid plane in front of the camera */
    volatile uint32_t mode, a, b;            /* DrawCharacterIcon style args, normal cells */
    volatile uint32_t sel_mode, sel_a, sel_b; /* style args, selected cell */
} ld_gridcfg;

/* Experimental border renderer: disabled until explicitly enabled using
 * the developer channel. Everything is tunable without patching live code.
 * enable 0=off, 1=selected-only, 2=selected + normal portraits. */
typedef struct {
    volatile uint32_t enable, drawn;
    volatile uint32_t resources_ok, last_error;
    volatile float x_mul, y_mul;
    volatile float x_add, y_add;
    volatile float normal_scale, selected_scale;
    volatile float alpha;
    /* Independent camera-plane offset for the SELECTED PORTRAIT only.
     * Positive x is right, negative y is down. This intentionally does not
     * move either the normal portraits or the native UI border sprites. */
    volatile float selected_icon_x, selected_icon_y;
} ld_bordercfg;

/* one captured DrawCharacterIcon call */
typedef struct {
    uint64_t ret, widget;
    uint32_t char_id, mode, a, b;
    float alpha, pad_;
    float mat[16];
} ld_dcicap;
#define LD_DCI_MAX 32

/* live-tunable icon test (poked by the PC tools while the game runs) */
typedef struct {
    volatile uint32_t enable;    /* 1 = draw test icons */
    volatile uint32_t char_id;   /* first character id */
    volatile uint32_t count;     /* icons in a row */
    volatile uint32_t flags;     /* bit0: basis from columns, bit1: negate forward, bit2: negate right, bit3: negate up */
    volatile float scale, dist, x, y, step, alpha;
    volatile uint32_t frames;    /* render hook calls (readback) */
    float cam_mat[16];           /* readback */
    float cam_pos[4];            /* readback */
} ld_icontest;

#define LD_LIB_MAX 160
#define LD_SNIFF_MAX 1024   /* saved tag images (keeps vehicle upgrades / names) */

typedef struct {
    uint32_t id;
    uint8_t  used, dirty, pad_[2];
    uint8_t  data[LD_TAG_SIZE];
} ld_libtag;

typedef struct {
    uint32_t magic, version;
    uint32_t size;               /* total image size incl. bss */
    uint32_t hook_off[H_COUNT];  /* entry points, relative to image start */
    uint32_t ping_off;           /* ld_ping(): returns LDH_PING, used to test that the image is executable */
    uint32_t render_off;         /* h_RenderHook(): called from the game's frame render via a trampoline */
    uint32_t dci_off;            /* h_DCICapture: inline hook target for DrawCharacterIcon */
    uint32_t icap_off;           /* h_IconCapture: inline hook target for the full icon drawer */
    uint32_t sel_off;            /* h_SelectionCapture: optional Characters UI hook */
    uint64_t orig[H_COUNT];      /* original import targets (filled by injector) */
    ld_gamefn fn;                /* filled by injector */
    /* options (injector may override) */
    uint32_t opt_hold_ms;        /* how long the combo must be held to open the menu */
    uint32_t opt_combo;          /* ScePad button mask that opens/closes the menu (0 = L3+R3) */
    uint32_t opt_flags;          /* bit0: draw with game debug text; bit1: force fake pad even if real exists */
    /* live state, read by the injector */
    volatile uint32_t state_seq; /* bumped whenever tags/menu change */
    volatile uint32_t menu_open;
    char     menu_line[96];      /* current highlighted line (for notification fallback) */
    volatile uint32_t passthrough;   /* 1 = real Toy Pad detected, emulation off */
    volatile uint32_t fake_opened;   /* game opened the virtual pad */
    volatile uint32_t usb_cmds;      /* commands received */
    volatile uint32_t pad_calls;     /* scePadReadState calls seen */
    volatile uint32_t pump_running;  /* delivery thread started */
    volatile uint32_t delivered;     /* IN reports delivered to the game */
    ld_pad   pad;
    ld_libtag lib[LD_LIB_MAX];
    uint32_t lock;
    /* USB sniffer ring (both real and virtual pad): [0]=dir 0 out/1 in, [1]=1 real, [2]=len, [4..35]=data */
    volatile uint32_t sniff_n;
    uint8_t  sniff[LD_SNIFF_MAX][36];
    ld_icontest itest;
    ld_gridcfg grid;
    volatile uint32_t dci_n;
    ld_dcicap dci[LD_DCI_MAX];
    volatile uint32_t icap_n;
    ld_iconcap icap[LD_ICAP_MAX];
    volatile uint32_t sel_seq;  /* observed calls, including sampled-out calls */
    volatile uint32_t sel_n;    /* sample write index (ring buffer) */
    struct {
        uint64_t params, item; /* rdi=MechSelectionMenus, rsi=item */
        uint32_t seq, pad_;
        uint8_t item_data[96]; /* contents of item at time of draw */
    } sel[64];
    ld_bordercfg border;
    /* command mailbox (v19): the injector fills op/a/b, then bumps req; the game handles it on
     * its next pad read and sets done = req. Used by the PS5-hosted LegoToypad web remote. */
    struct {
        volatile uint32_t req, done;
        volatile uint32_t op, a, b;
        volatile int32_t result;
    } mbox;
    /* console overlay UI (v20). While ext.mode != 0 the in-game text menu is replaced by the
     * LegoToypad-style overlay the injector draws in the system UI. The open/close combo is still
     * handled here so input is swallowed with no latency; while the overlay is open every change of
     * the owner's buttons (plus the left stick as D-pad bits) is pushed into ring[]. */
    struct {
        volatile uint32_t mode;        /* injector: 1 = overlay UI active */
        volatile uint32_t open;        /* hook: 1 while the overlay owns the controller */
        volatile uint32_t open_seq;    /* hook: bumped on every open */
        volatile uint32_t close_req;   /* injector: bump to ask the hook to close */
        volatile uint32_t close_ack;   /* hook: = close_req once handled */
        volatile uint32_t raw;         /* hook: player 1's live buttons while closed (sneak peek) */
        volatile uint32_t wr;          /* hook: ring write counter */
        volatile uint32_t heartbeat;   /* hook: pad reads seen in ext mode */
        volatile uint32_t lease;       /* injector: bumped while the overlay is alive; if it stops
                                          for ~200 pad reads the hook falls back to the text menu */
        volatile uint32_t ring[32];    /* hook: buttons | stick-as-dpad on each change while open */
    } ext;
} ld_header;

enum { LD_MB_PLACE_ID = 1, LD_MB_REMOVE = 2, LD_MB_CLEAR_ALL = 3 };
