/* Virtual Toy Pad console overlay: wire protocol between the injector (ldinject) and the
 * renderer it loads into SceShellUI.  TCP on 127.0.0.1:OVL_PORT, little endian, every message
 * is an ovl_msg header followed by `len` payload bytes.
 *
 * The renderer is a dumb retained-mode compositor: the injector DEFINEs an ordered list of
 * widgets once (list order = z order), uploads textures, then sends FRAME deltas. All layout
 * is in a 1920x1080 logical canvas; the renderer scales it to the system UI scene.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>

#define OVL_PORT    39231
#define OVL_MAGIC   0x4F505456u   /* "VTPO" */
#define OVL_VERSION 1

enum {
    OVL_HELLO   = 1,   /* r->i  ovl_hello */
    OVL_TEX     = 2,   /* i->r  u32 id, u16 w, u16 h, w*h*4 bytes RGBA (straight alpha) */
    OVL_TEXPNG  = 3,   /* i->r  u32 id, u16 w, u16 h, PNG bytes (file fallback: /Temp) */
    OVL_TEXFREE = 4,   /* i->r  u32 id */
    OVL_DEFINE  = 5,   /* i->r  u16 count, then count x u8 widget type */
    OVL_FRAME   = 6,   /* i->r  u16 count, then count x item (see below) */
    OVL_STATUS  = 7,   /* r->i  text (renderer log line worth surfacing) */
    OVL_PING    = 8,   /* both  empty */
    OVL_CONFIG  = 9,   /* i->r  ovl_config */
};

enum { OVL_W_IMAGE = 1, OVL_W_PANEL = 2, OVL_W_LABEL = 3 };

/* renderer capabilities (HELLO) */
enum {
    OVL_CAP_PIXELS  = 1,    /* Texture2D + SetPixels + ImageAsset(Texture) */
    OVL_CAP_FILES   = 2,    /* ImageBox.LoadAsync(file:///Temp/...) */
    OVL_CAP_FILTER  = 4,    /* ImageBox.FilterColor (image tint) */
    OVL_CAP_PANEL   = 8,    /* UI2.Panel + BackgroundColor */
    OVL_CAP_TRIM    = 16,   /* Label width / trimming / line count */
    OVL_CAP_SCENE   = 32,   /* a game scene is up right now */
};

/* FRAME item: u16 id, u16 mask, then the fields of each set bit in this order */
enum {
    OVL_F_GEOM  = 1,    /* f32 x, y, w, h        (labels: w = box width, h ignored) */
    OVL_F_VIS   = 2,    /* u32 visible */
    OVL_F_ALPHA = 4,    /* f32 opacity 0..1 */
    OVL_F_COLOR = 8,    /* u8 r, g, b, a         image tint / panel fill / text colour */
    OVL_F_TEX   = 16,   /* u32 texture id        (image) */
    OVL_F_TEXT  = 32,   /* u16 font px, u8 align (0 left 1 centre 2 right), u8 lines, u16 len, utf8, pad to 4 */
};

#pragma pack(push, 1)
typedef struct { uint32_t magic; uint16_t type; uint16_t version; uint32_t len; } ovl_msg;
typedef struct { uint32_t caps; float scene_w, scene_h; int32_t pid; uint32_t textures, widgets; } ovl_hello;
typedef struct { uint32_t tex_mode; uint32_t reserved[3]; } ovl_config;   /* 0 auto, 1 pixels, 2 files */
#pragma pack(pop)

#define OVL_MAX_WIDGETS 512
#define OVL_MAX_MSG     (4u << 20)
