/* In-game hook module: virtual Toy Pad behind the libSceUsbd imports of LEGO Dimensions (PS4).
 * Freestanding, position independent. Loaded and wired up by the PS5 injector, which
 * redirects the game's GOT entries for sceUsbd* / scePadReadState to the h_* functions. */
#include "ldhook.h"

__attribute__((section(".hdr"), used)) ld_header HDR = {
    .magic = LDH_MAGIC,
    .version = LDH_VERSION,
    .opt_hold_ms = 250,
    .opt_combo = 0x2 | 0x4,   /* L3 + R3 */
    .opt_flags = 1,
};

/* ------------------------------------------------------------- utilities */

void *memcpy(void *d, const void *s, unsigned long n) {
    uint8_t *dd = d; const uint8_t *ss = s;
    while (n--) *dd++ = *ss++;
    return d;
}
void *memset(void *d, int v, unsigned long n) {
    uint8_t *dd = d;
    while (n--) *dd++ = (uint8_t)v;
    return d;
}

float sqrtf(float x) {
    float r;
    __asm__("sqrtss %1, %0" : "=x"(r) : "x"(x));
    return r;
}

static void lock(void) {
    while (__atomic_exchange_n(&HDR.lock, 1, __ATOMIC_ACQUIRE))
        __builtin_ia32_pause();
}
static void unlock(void) { __atomic_store_n(&HDR.lock, 0, __ATOMIC_RELEASE); }

typedef int (*usleep_fn)(uint32_t);
static void sleep_us(uint32_t us) {
    if (HDR.fn.usleep) ((usleep_fn)HDR.fn.usleep)(us);
}

#define ORIG(i, type) ((type)HDR.orig[i])

/* ------------------------------------------------- libusb mirror structs */

typedef struct {
    uint8_t  bLength, bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass, bDeviceSubClass, bDeviceProtocol, bMaxPacketSize0;
    uint16_t idVendor, idProduct, bcdDevice;
    uint8_t  iManufacturer, iProduct, iSerialNumber, bNumConfigurations;
} usb_dev_desc;

typedef struct {
    uint8_t  bLength, bDescriptorType, bEndpointAddress, bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval, bRefresh, bSynchAddress;
    const void *extra; int extra_length;
} usb_ep_desc;            /* 32 bytes */

typedef struct {
    uint8_t  bLength, bDescriptorType, bInterfaceNumber, bAlternateSetting, bNumEndpoints;
    uint8_t  bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol, iInterface;
    const usb_ep_desc *endpoint;
    const void *extra; int extra_length;
} usb_if_desc;            /* 40 bytes */

typedef struct { const usb_if_desc *altsetting; int num_altsetting; } usb_if;

typedef struct {
    uint8_t  bLength, bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces, bConfigurationValue, iConfiguration, bmAttributes, MaxPower;
    const usb_if *interface;
    const void *extra; int extra_length;
} usb_cfg_desc;           /* 40 bytes */

typedef struct usb_xfer usb_xfer;
typedef void (*usb_cb)(usb_xfer *);
struct usb_xfer {
    void    *dev_handle;   /* 0x00 */
    uint8_t  flags;        /* 0x08 */
    uint8_t  endpoint;     /* 0x09 */
    uint8_t  type;         /* 0x0a */
    uint32_t timeout;      /* 0x0c */
    int32_t  status;       /* 0x10 */
    int32_t  length;       /* 0x14 */
    int32_t  actual_length;/* 0x18 */
    usb_cb   callback;     /* 0x20 */
    void    *user_data;    /* 0x28 */
    uint8_t *buffer;       /* 0x30 */
};

/* the virtual device; its address doubles as the libusb_device* and handle */
static struct { uint64_t tag; } FAKE_DEV = {0x4C44544F59504144ull}, FAKE_HANDLE = {0x4C4448414E444C45ull};
#define IS_FAKE_DEV(d) ((void *)(d) == (void *)&FAKE_DEV)
#define IS_FAKE_H(h)   ((void *)(h) == (void *)&FAKE_HANDLE)

static usb_ep_desc  EPS[2];
static usb_if_desc  IFD;
static usb_if       IFS;
static usb_cfg_desc CFG;

static void build_descriptors(void) {
    /* filled at runtime: no absolute pointers in initialised data */
    EPS[0] = (usb_ep_desc){7, 5, 0x81, 3, 32, 1, 0, 0, 0, 0};
    EPS[1] = (usb_ep_desc){7, 5, 0x01, 3, 32, 1, 0, 0, 0, 0};
    IFD = (usb_if_desc){9, 4, 0, 0, 2, 3, 0, 0, 0, EPS, 0, 0};
    IFS = (usb_if){&IFD, 1};
    CFG = (usb_cfg_desc){9, 2, 0x29, 1, 1, 0, 0x80, 0xFA, &IFS, 0, 0};
}

/* ------------------------------------------------------- transfer queues */

#define MAXX 64
static usb_xfer *in_q[MAXX];  static uint32_t in_n;   /* pending IN transfers  */
static usb_xfer *out_q[MAXX]; static uint32_t out_n;  /* completed OUT transfers */
static void **saved_list;          /* original list returned by the real GetDeviceList */
static void *my_list[72];
static int initialised;
static void start_pump(void);

static void init_once(void) {
    if (initialised) return;
    initialised = 1;
    build_descriptors();
    if (!HDR.pad.uid_counter) ld_pad_init(&HDR.pad, (uint32_t)(uintptr_t)&HDR ^ 0x9E3779B9u);
}

/* --------------------------------------------------------------- hooks */

typedef long (*fn_list)(void ***);
long h_GetDeviceList(void ***list) {
    init_once();
    long n = ORIG(H_GetDeviceList, fn_list)(list);
    if (n < 0) return n;
    if (!(HDR.opt_flags & 2)) {
        typedef int (*fn_desc)(void *, usb_dev_desc *);
        for (long i = 0; i < n; i++) {
            usb_dev_desc d;
            if (ORIG(H_GetDeviceDescriptor, fn_desc)((*list)[i], &d) == 0 &&
                d.idVendor == 0x0E6F && d.idProduct == 0x0241) {
                HDR.passthrough = 1;           /* a real Toy Pad is plugged in: stay out of the way */
                return n;
            }
        }
    }
    HDR.passthrough = 0;
    if (n > 70) return n;
    for (long i = 0; i < n; i++) my_list[i] = (*list)[i];
    my_list[n] = &FAKE_DEV;
    my_list[n + 1] = 0;
    saved_list = *list;
    *list = my_list;
    return n + 1;
}

typedef void (*fn_freelist)(void **, int);
void h_FreeDeviceList(void **list, int unref) {
    if (list == my_list) { list = saved_list; saved_list = 0; }
    if (list) ORIG(H_FreeDeviceList, fn_freelist)(list, unref);
}

typedef int (*fn_devdesc)(void *, usb_dev_desc *);
int h_GetDeviceDescriptor(void *dev, usb_dev_desc *d) {
    if (!IS_FAKE_DEV(dev)) return ORIG(H_GetDeviceDescriptor, fn_devdesc)(dev, d);
    *d = (usb_dev_desc){0x12, 1, 0x200, 0, 0, 0, 0x20, 0x0E6F, 0x0241, 0x200, 1, 2, 0, 1};
    return 0;
}

typedef uint8_t (*fn_u8dev)(void *);
uint8_t h_GetBusNumber(void *dev) {
    return IS_FAKE_DEV(dev) ? 0x7E : ORIG(H_GetBusNumber, fn_u8dev)(dev);
}
uint8_t h_GetDeviceAddress(void *dev) {
    return IS_FAKE_DEV(dev) ? 0x2A : ORIG(H_GetDeviceAddress, fn_u8dev)(dev);
}

typedef void *(*fn_open)(uint16_t, uint16_t);
void *h_OpenDeviceWithVidPid(uint16_t vid, uint16_t pid) {
    if (vid == 0x0E6F && pid == 0x0241 && !HDR.passthrough) {
        lock();
        in_n = out_n = 0;
        HDR.fake_opened++;
        unlock();
        start_pump();
        return &FAKE_HANDLE;
    }
    return ORIG(H_OpenDeviceWithVidPid, fn_open)(vid, pid);
}

typedef int (*fn_cfg)(void *, uint8_t, usb_cfg_desc **);
int h_GetConfigDescriptor(void *dev, uint8_t idx, usb_cfg_desc **cfg) {
    if (!IS_FAKE_DEV(dev)) return ORIG(H_GetConfigDescriptor, fn_cfg)(dev, idx, cfg);
    init_once();
    *cfg = &CFG;
    return 0;
}

typedef void (*fn_freecfg)(usb_cfg_desc *);
void h_FreeConfigDescriptor(usb_cfg_desc *cfg) {
    if (cfg != &CFG) ORIG(H_FreeConfigDescriptor, fn_freecfg)(cfg);
}

typedef int (*fn_hi)(void *, int);
int h_KernelDriverActive(void *h, int i) {
    return IS_FAKE_H(h) ? 1 : ORIG(H_KernelDriverActive, fn_hi)(h, i);
}
int h_DetachKernelDriver(void *h, int i) {
    return IS_FAKE_H(h) ? 0 : ORIG(H_DetachKernelDriver, fn_hi)(h, i);
}
int h_ClaimInterface(void *h, int i) {
    return IS_FAKE_H(h) ? 0 : ORIG(H_ClaimInterface, fn_hi)(h, i);
}
typedef int (*fn_h)(void *);
int h_ResetDevice(void *h) {
    return IS_FAKE_H(h) ? 0 : ORIG(H_ResetDevice, fn_h)(h);
}
int h_CheckConnected(void *h) {
    return IS_FAKE_H(h) ? 0 : ORIG(H_CheckConnected, fn_h)(h);
}
typedef int (*fn_mps)(void *, uint8_t);
int h_GetMaxPacketSize(void *dev, uint8_t ep) {
    return IS_FAKE_DEV(dev) ? 32 : ORIG(H_GetMaxPacketSize, fn_mps)(dev, ep);
}

/* ---------------------------------------------------------------- sniffer */

static void sniff(int dir, int real, const uint8_t *data, int len) {
    uint32_t i = __atomic_fetch_add(&HDR.sniff_n, 1, __ATOMIC_RELAXED) % LD_SNIFF_MAX;
    uint8_t *e = HDR.sniff[i];
    if (len > 32) len = 32;
    if (len < 0) len = 0;
    e[0] = (uint8_t)dir; e[1] = (uint8_t)real; e[2] = (uint8_t)len; e[3] = 0;
    for (int k = 0; k < len; k++) e[4 + k] = data[k];
}

/* real-device IN transfers: wrap the game's callback to see the data */
static struct { usb_xfer *t; usb_cb cb; } wraps[MAXX];
static void sniff_in_cb(usb_xfer *t) {
    usb_cb cb = 0;
    for (int i = 0; i < MAXX; i++) if (wraps[i].t == t) { cb = wraps[i].cb; break; }
    if (t->status == 0 && t->actual_length > 0) sniff(1, 1, t->buffer, t->actual_length);
    if (cb) { t->callback = cb; cb(t); }
}

/* Sony's fill helper does not fill the struct for our fake handle: do it ourselves */
typedef void (*fn_fill)(usb_xfer *, void *, uint8_t, uint8_t *, int, usb_cb, void *, uint32_t);
void h_FillInterruptTransfer(usb_xfer *t, void *h, uint8_t ep, uint8_t *buf, int len, usb_cb cb, void *ud, uint32_t timeout) {
    if (!IS_FAKE_H(h)) { ORIG(H_FillInterruptTransfer, fn_fill)(t, h, ep, buf, len, cb, ud, timeout); return; }
    t->dev_handle = h;
    t->endpoint = ep;
    t->type = 3;            /* LIBUSB_TRANSFER_TYPE_INTERRUPT */
    t->timeout = timeout;
    t->buffer = buf;
    t->length = len;
    t->user_data = ud;
    t->callback = cb;
}

typedef int (*fn_xfer)(usb_xfer *);
int h_SubmitTransfer(usb_xfer *t) {
    if (!t || !IS_FAKE_H(t->dev_handle)) {
        if (t && t->buffer) {
            if (t->endpoint & 0x80) {
                if (t->callback != sniff_in_cb) {
                    int slot = -1;
                    for (int i = 0; i < MAXX; i++) if (wraps[i].t == t || (!wraps[i].t && slot < 0)) { slot = i; if (wraps[i].t == t) break; }
                    if (slot >= 0) { wraps[slot].t = t; wraps[slot].cb = t->callback; t->callback = sniff_in_cb; }
                }
            } else sniff(0, 1, t->buffer, t->length);
        }
        return ORIG(H_SubmitTransfer, fn_xfer)(t);
    }
    if (!(t->endpoint & 0x80)) sniff(0, 0, t->buffer, t->length);
    lock();
    if (t->endpoint & 0x80) {
        if (in_n < MAXX) in_q[in_n++] = t;
    } else {
        ld_pad_write(&HDR.pad, t->buffer, (uint32_t)t->length);
        HDR.usb_cmds++;
        if (out_n < MAXX) out_q[out_n++] = t;
    }
    unlock();
    return 0;
}

/* Completes pending virtual transfers. Returns 1 if anything was done.
 * Runs on our own pump thread: the game's USB event thread may be parked forever inside the
 * real sceUsbdHandleEvents (no real device -> no event), so it cannot be relied upon. */
static uint32_t pump_busy;
static int deliver(void) {
    if (__atomic_exchange_n(&pump_busy, 1, __ATOMIC_ACQUIRE)) return 0;  /* one deliverer at a time */
    int did = 0;
    for (;;) {
        usb_xfer *t = 0;
        lock();
        if (out_n) {
            t = out_q[--out_n];
            t->status = 0; t->actual_length = t->length;
        } else if (in_n && ld_pad_pending(&HDR.pad)) {
            t = in_q[0];
            for (uint32_t i = 1; i < in_n; i++) in_q[i - 1] = in_q[i];
            in_n--;
            ld_pad_read(&HDR.pad, t->buffer);
            t->status = 0; t->actual_length = 32; t->length = 32;
        }
        unlock();
        if (!t) break;
        if (t->endpoint & 0x80) { sniff(1, 0, t->buffer, 32); HDR.delivered++; }
        did = 1;
        if (t->callback) t->callback(t);   /* game copies data and resubmits */
    }
    __atomic_store_n(&pump_busy, 0, __ATOMIC_RELEASE);
    return did;
}

static void *pump(void *arg) {
    (void)arg;
    for (;;) {
        if (!HDR.passthrough && HDR.fake_opened) { if (!deliver()) sleep_us(1000); }
        else sleep_us(20000);
    }
    return 0;
}

typedef int (*fn_pcreate)(void *thr, const void *attr, void *(*entry)(void *), void *arg, const char *name);
static void start_pump(void) {
    if (__atomic_exchange_n(&HDR.pump_running, 1, __ATOMIC_ACQ_REL)) return;
    static uint64_t thr;
    if (!HDR.fn.pthread_create || ((fn_pcreate)HDR.fn.pthread_create)(&thr, 0, pump, 0, "LDToyPadPump") != 0)
        HDR.pump_running = 0;
}

typedef int (*fn_void)(void);
int h_HandleEvents(void) {
    if (HDR.passthrough || !HDR.fake_opened) return ORIG(H_HandleEvents, fn_void)();
    if (!HDR.pump_running && !deliver()) sleep_us(2000);
    else sleep_us(2000);
    return 0;
}

/* ------------------------------------------------------------- pad hook */

typedef int (*fn_pad)(int32_t, void *);
void menu_frame(int32_t handle, uint32_t *buttons, uint8_t *sticks);

int h_PadReadState(int32_t handle, void *data) {
    int r = ORIG(H_PadReadState, fn_pad)(handle, data);
    HDR.pad_calls++;
    if (r == 0 && data) {
        uint8_t *d = (uint8_t *)data;
        menu_frame(handle, (uint32_t *)d, d + 4);
    }
    return r;
}

uint32_t ld_ping(void) { return LDH_PING; }

/* used by menu.c */
void ld_lock(void) { lock(); }
void ld_unlock(void) { unlock(); }
