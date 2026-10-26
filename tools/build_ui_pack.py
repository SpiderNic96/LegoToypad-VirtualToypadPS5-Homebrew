#!/usr/bin/env python3
"""Build the on-console overlay art for the PS5 Virtual Toy Pad ("ui" pack).

The console overlay is drawn by the PS5 system UI from pre-rendered textures, so every
LegoToypad surface that the desktop app paints with GDI+ (pads, glows, outlines, portraits,
world tiles, buttons...) is rendered here once, with the same geometry and colours as
LegoToypad's main.cpp (RenderPad, RenderFocusGlow, RenderPortrait, RenderFranchiseTile,
RenderLedHalo, DrawCircularFocusHalo, DrawFavoriteStar, RenderBackground).

Textures are rendered at K = 1.5 x LegoToypad's 900x610 window units, which is 1:1 on the
1920x1080 system-UI canvas the overlay is laid out on.

usage: build_ui_pack.py <LegoToypad repo> <web bundle dir>     (writes <web>/ui/)
The web bundle is the one built by build_bundle.py; bin ids and portraits come from it, so the
pack always matches the bundle it sits in.
"""
import json, math, shutil, sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

K = 1.5          # texture scale: LegoToypad window units -> 1080p canvas pixels
SS = 4           # supersampling for anti-aliased shapes

# ---------------------------------------------------------------- LegoToypad constants (main.cpp)
PAD_CELLS = [(100, 220, 240, 356), (356, 103, 544, 287), (660, 220, 800, 356),
             (100, 364, 236, 500), (244, 364, 408, 500), (492, 364, 656, 500), (664, 364, 800, 500)]
LED_REGION_SLOTS = [[0, 3, 4], [1], [2, 5, 6]]
PAD_GLOW_MARGIN = 8
FOCUS_GLOW_MARGIN = 20
PORTRAIT_MARGIN = 10
LED_GLOW_RADIUS = 14
LED_GLOW_MARGIN = LED_GLOW_RADIUS + 8
TILE_W, TILE_H, TILE_MARGIN = 190, 100, 6
SELECTION = (255, 72, 72)         # kSelectionGlow
MOVE_SOURCE = (66, 157, 255)      # kMoveSourceGlow
GOLD = (255, 204, 51)             # kGlowGold
PAD_BORDER_IDLE = (106, 90, 82)   # kPadBorderIdle 0x00525A6A (BGR) -> RGB(106,90,82)? see below
# kPadBorderIdle = 0x00525A6A is a COLORREF (0x00BBGGRR): R=0x6A G=0x5A B=0x52
PAD_BORDER_IDLE = (0x6A, 0x5A, 0x52)
# The sneak-peek HUD (ComputePeekCells) on a 1920x1080 screen at the Medium size.
PEEK_W, PEEK_H, PEEK_FRACTION, PEEK_SCALE = 1920, 1080, 0.145, 1.0


def S(v):
    return int(round(v * K))


# ---------------------------------------------------------------- drawing helpers

def shape_of(slot):
    """('A'|'B'|'C'|'D', w, h): the four pad silhouettes (D is the round centre pad)."""
    l, t, r, b = PAD_CELLS[slot]
    w, h = r - l, b - t
    return {(140, 136): "A", (136, 136): "B", (164, 136): "C", (188, 184): "D"}[(w, h)], w, h


SHAPES = {"A": (140, 136), "B": (136, 136), "C": (164, 136), "D": (188, 184)}


def path_mask(size, rects, scale=1.0):
    """Supersampled silhouette: rects = [(x, y, w, h, circular)] in pixels -> 'L' mask."""
    W, H = size
    m = Image.new("L", (W * SS, H * SS), 0)
    d = ImageDraw.Draw(m)
    for x, y, w, h, circ in rects:
        box = [x * SS, y * SS, (x + w) * SS - 1, (y + h) * SS - 1]
        if circ:
            d.ellipse(box, fill=255)
        else:
            d.rounded_rectangle(box, radius=14 * scale * SS, fill=255)
    return m.resize((W, H), Image.LANCZOS)


def stroke_mask(size, rect, circ, width, radius):
    """Centred stroke of a rounded rect / ellipse, like GDI+ DrawPath with a pen of `width`."""
    W, H = size
    x, y, w, h = rect
    m = Image.new("L", (W * SS, H * SS), 0)
    d = ImageDraw.Draw(m)
    half = width / 2.0
    outer = [(x - half) * SS, (y - half) * SS, (x + w + half) * SS - 1, (y + h + half) * SS - 1]
    inner = [(x + half) * SS, (y + half) * SS, (x + w - half) * SS - 1, (y + h - half) * SS - 1]
    if circ:
        d.ellipse(outer, fill=255)
        d.ellipse(inner, fill=0)
    else:
        d.rounded_rectangle(outer, radius=(radius + half) * SS, fill=255)
        d.rounded_rectangle(inner, radius=max(0.0, radius - half) * SS, fill=0)
    return m.resize((W, H), Image.LANCZOS)


def solid(size, rgb, mask, alpha=255):
    im = Image.new("RGBA", size, rgb + (0,))
    a = np.asarray(mask, dtype=np.float32) * (alpha / 255.0)
    im.putalpha(Image.fromarray(np.clip(a, 0, 255).astype(np.uint8)))
    return im


def box_blur_alpha(a, radius):
    """LegoToypad CompositeGlow: 2 x (horizontal + vertical) clamped box blur."""
    a = a.astype(np.float32)

    def blur1(arr, axis):
        n = arr.shape[axis]
        c = np.cumsum(arr, axis=axis)
        c = np.concatenate([np.zeros_like(np.take(c, [0], axis=axis)), c], axis=axis)
        idx = np.arange(n)
        lo = np.clip(idx - radius, 0, n - 1)
        hi = np.clip(idx + radius, 0, n - 1)
        s = np.take(c, hi + 1, axis=axis) - np.take(c, lo, axis=axis)
        cnt = (hi - lo + 1).astype(np.float32)
        shape = [1] * arr.ndim
        shape[axis] = n
        return s / cnt.reshape(shape)

    for _ in range(2):
        a = blur1(a, 1)
        a = blur1(a, 0)
    return a


def glow(size, mask, rgb, radius):
    """DrawGlow: blurred opaque silhouette in `rgb` (premultiplied in GDI+, straight alpha here)."""
    a = box_blur_alpha(np.asarray(mask), radius)
    im = Image.new("RGBA", size, rgb + (0,))
    im.putalpha(Image.fromarray(np.clip(a, 0, 255).astype(np.uint8)))
    return im


def over(dst, src, xy=(0, 0)):
    dst.alpha_composite(src, xy)
    return dst


def load(path):
    return Image.open(path).convert("RGBA")


def fit(im, bw, bh):
    s = min(bw / im.width, bh / im.height)
    return im.resize((max(1, round(im.width * s)), max(1, round(im.height * s))), Image.LANCZOS)


def cover(im, bw, bh):
    s = max(bw / im.width, bh / im.height)
    r = im.resize((max(1, round(im.width * s)), max(1, round(im.height * s))), Image.LANCZOS)
    x, y = (r.width - bw) // 2, (r.height - bh) // 2
    return r.crop((x, y, x + bw, y + bh))


def top_gloss(size, mask, a0=70):
    """DrawTopGloss: white vertical gradient alpha a0 -> 0 clipped to the shape."""
    W, H = size
    grad = np.linspace(a0, 0, H, dtype=np.float32)[:, None] * np.ones((1, W), np.float32)
    a = grad * (np.asarray(mask, np.float32) / 255.0)
    im = Image.new("RGBA", size, (255, 255, 255, 0))
    im.putalpha(Image.fromarray(np.clip(a, 0, 255).astype(np.uint8)))
    return im


def opaque_bounds(im):
    a = np.asarray(im.split()[-1])
    ys, xs = np.nonzero(a > 8)
    if len(xs) == 0:
        return (0, 0, im.width, im.height)
    l, t, r, b = max(0, xs.min() - 2), max(0, ys.min() - 2), min(im.width - 1, xs.max() + 2), min(im.height - 1, ys.max() + 2)
    return (l, t, r + 1, b + 1)


def hexrgb(h):
    h = h.lstrip("#")
    return (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16))


# ---------------------------------------------------------------- surfaces

def render_pad_art(art, slot):
    kind, w, h = shape_of(slot)
    W, H = S(w), S(h)
    m = path_mask((W, H), [(0, 0, W, H, kind == "D")], K)
    a = art.resize((W, H), Image.LANCZOS)
    a.putalpha(Image.fromarray((np.asarray(a.split()[-1], np.float32) * np.asarray(m, np.float32) / 255).astype(np.uint8)))
    return a


def render_mask(kind):
    w, h = SHAPES[kind]
    W, H = S(w), S(h)
    return solid((W, H), (255, 255, 255), path_mask((W, H), [(0, 0, W, H, kind == "D")], K))


def render_outline(kind, width, rgb=(255, 255, 255), alpha=255):
    w, h = SHAPES[kind]
    M = S(PAD_GLOW_MARGIN)
    W, H = S(w) + 2 * M, S(h) + 2 * M
    return solid((W, H), rgb, stroke_mask((W, H), (M, M, S(w), S(h)), kind == "D", width * K, 14 * K), alpha)


def render_selected_outline(kind, wide=7.5, crisp=2.4, margin=PAD_GLOW_MARGIN, radius=14, circ=None, w=None, h=None):
    if w is None:
        w, h = SHAPES[kind]
        circ = kind == "D"
    M = S(margin)
    W, H = S(w) + 2 * M, S(h) + 2 * M
    im = solid((W, H), SELECTION, stroke_mask((W, H), (M, M, S(w), S(h)), circ, wide * K, radius * K), 80)
    over(im, solid((W, H), SELECTION, stroke_mask((W, H), (M, M, S(w), S(h)), circ, crisp * K, radius * K), 225))
    return im


def render_focus_glow(w, h, circ, radius, rgb, hollow=False):
    """RenderFocusGlow: wide bloom (13) + tight core (5), margin 20. `hollow` clears the shape
    itself: on the console a lit pad is drawn as a translucent LED tint without the opaque pad
    art, and a filled glow would bleed through it."""
    M = S(FOCUS_GLOW_MARGIN)
    W, H = S(w) + 2 * M, S(h) + 2 * M
    m = Image.new("L", (W * SS, H * SS), 0)
    d = ImageDraw.Draw(m)
    box = [M * SS, M * SS, (M + S(w)) * SS - 1, (M + S(h)) * SS - 1]
    if circ:
        d.ellipse(box, fill=255)
    else:
        d.rounded_rectangle(box, radius=radius * K * SS, fill=255)
    m = m.resize((W, H), Image.LANCZOS)
    im = glow((W, H), m, rgb, round(13 * K))
    im = over(im, glow((W, H), m, rgb, round(5 * K)))
    if hollow:
        inner = np.asarray(m.filter(ImageFilter.MinFilter(5)), np.float32) / 255.0
        a = np.asarray(im.split()[-1], np.float32) * (1.0 - inner)
        im.putalpha(Image.fromarray(np.clip(a, 0, 255).astype(np.uint8)))
    return im


def led_halo(cells, region, scale):
    """RenderLedHalo silhouette in white (tinted at runtime), margin kLedGlowMargin."""
    slots = LED_REGION_SLOTS[region]
    l = min(cells[s][0] for s in slots); t = min(cells[s][1] for s in slots)
    r = max(cells[s][2] for s in slots); b = max(cells[s][3] for s in slots)
    M = round(LED_GLOW_MARGIN * scale)
    W, H = round((r - l) * scale) + 2 * M, round((b - t) * scale) + 2 * M
    rects = [((cells[s][0] - l) * scale + M, (cells[s][1] - t) * scale + M,
              (cells[s][2] - cells[s][0]) * scale, (cells[s][3] - cells[s][1]) * scale, s == 1) for s in slots]
    m = path_mask((W, H), rects, scale)
    return glow((W, H), m, (255, 255, 255), round(LED_GLOW_RADIUS * scale)), (l, t, r, b)


def peek_cells(width=PEEK_W, height=PEEK_H):
    """ComputePeekCells for the Medium size."""
    s = (height * PEEK_FRACTION * PEEK_SCALE) / 136.0
    mx, my = width * 0.022, height * 0.030
    gutter, vgap = 10 * s, 24 * s
    lower = height - my - 136 * s
    upper = lower - vgap - 136 * s
    R = lambda x, y, w, h: (round(x), round(y), round(x + w), round(y + h))
    c = [None] * 7
    c[3] = R(mx, lower, 136 * s, 136 * s)
    c[4] = R(mx + 136 * s + gutter, lower, 164 * s, 136 * s)
    c[0] = R(mx, upper, 140 * s, 136 * s)
    c[6] = R(width - mx - 136 * s, lower, 136 * s, 136 * s)
    c[5] = R(width - mx - 136 * s - gutter - 164 * s, lower, 164 * s, 136 * s)
    c[2] = R(width - mx - 140 * s, upper, 140 * s, 136 * s)
    c[1] = R((width - 188 * s) / 2, my, 188 * s, 184 * s)
    return c, s


PORTRAIT_D = 120   # texture circle diameter in px (displayed at 84..148 x K)


def render_portrait(photo, ring_rgb):
    """RenderPortrait (normal state): circle-clipped art at 92%, top gloss, ring alpha 170, dark rim."""
    d = PORTRAIT_D
    M = round(PORTRAIT_MARGIN * d / 94)
    W = d + 2 * M
    im = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    circle = path_mask((W, W), [(M, M, d, d, True)])
    if photo is not None:
        l, t, r, b = opaque_bounds(photo)
        src = photo.crop((l, t, r, b))
        td = d * 0.92
        s = min(td / src.width, td / src.height)
        src = src.resize((max(1, round(src.width * s)), max(1, round(src.height * s))), Image.LANCZOS)
        layer = Image.new("RGBA", (W, W), (0, 0, 0, 0))
        layer.alpha_composite(src, (M + (d - src.width) // 2, M + (d - src.height) // 2))
        la = np.asarray(layer.split()[-1], np.float32) * np.asarray(circle, np.float32) / 255
        layer.putalpha(Image.fromarray(la.astype(np.uint8)))
        over(im, layer)
    else:
        g = Image.new("RGBA", (W, W), (0, 0, 0, 0))
        top, bot = np.array([46, 52, 64], np.float32), np.array([22, 26, 36], np.float32)
        t = np.linspace(0, 1, W, dtype=np.float32)[:, None, None]
        rgb = (top * (1 - t) + bot * t) * np.ones((1, W, 1), np.float32)
        g = Image.fromarray(np.concatenate([rgb, np.asarray(circle, np.float32)[:, :, None]], 2).astype(np.uint8), "RGBA")
        over(im, g)
    over(im, top_gloss((W, W), circle))
    sc = d / 94.0
    over(im, solid((W, W), ring_rgb, stroke_mask((W, W), (M, M, d, d), True, 2.0 * sc, 0), 170))
    over(im, solid((W, W), (0, 0, 0), stroke_mask((W, W), (M, M, d, d), True, 1.0 * sc, 0), 70))
    return im


def render_portrait_focus():
    """Focused-portrait extras in white (tinted with the ring colour at runtime):
    DrawCircularFocusHalo (soft 7px a80 + crisp 3px a130) and the 3.5px full-alpha ring."""
    d = PORTRAIT_D
    M = round(PORTRAIT_MARGIN * d / 94)
    W = d + 2 * M
    sc = d / 94.0
    im = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    o = 3.5 * sc
    over(im, solid((W, W), (255, 255, 255), stroke_mask((W, W), (M - o, M - o, d + 2 * o, d + 2 * o), True, 7 * sc, 0), 80))
    o = 1.5 * sc
    over(im, solid((W, W), (255, 255, 255), stroke_mask((W, W), (M - o, M - o, d + 2 * o, d + 2 * o), True, 3 * sc, 0), 130))
    over(im, solid((W, W), (255, 255, 255), stroke_mask((W, W), (M, M, d, d), True, 3.5 * sc, 0), 255))
    return im


def render_plus():
    """RenderPlusTile body (ring added by the focus/ring overlay at runtime)."""
    d = PORTRAIT_D
    M = round(PORTRAIT_MARGIN * d / 94)
    W = d + 2 * M
    circle = path_mask((W, W), [(M, M, d, d, True)])
    top, bot = np.array([40, 46, 58], np.float32), np.array([18, 22, 30], np.float32)
    t = np.linspace(0, 1, W, dtype=np.float32)[:, None, None]
    rgb = (top * (1 - t) + bot * t) * np.ones((1, W, 1), np.float32)
    im = Image.fromarray(np.concatenate([rgb, np.asarray(circle, np.float32)[:, :, None]], 2).astype(np.uint8), "RGBA")
    over(im, top_gloss((W, W), circle))
    dr = ImageDraw.Draw(im)
    cx = M + d / 2.0
    bar = d * 0.22
    th = 3.6 * d / 94
    dr.rectangle([cx - th / 2, M + (d - bar) / 2, cx + th / 2, M + (d + bar) / 2], fill=(235, 238, 244, 255))
    dr.rectangle([M + (d - bar) / 2, cx - th / 2, M + (d + bar) / 2, cx + th / 2], fill=(235, 238, 244, 255))
    return im


def render_ring():
    """Idle ring (2px, alpha 170) alone, tinted with the group colour - used on the plus tile."""
    d = PORTRAIT_D
    M = round(PORTRAIT_MARGIN * d / 94)
    W = d + 2 * M
    return solid((W, W), (255, 255, 255), stroke_mask((W, W), (M, M, d, d), True, 2.0 * d / 94, 0), 170)


def render_world_tile(tile_base, logo, label=None, grow=1.0):
    """RenderFranchiseTile (unfocused, border drawn separately), margin 6."""
    M = S(TILE_MARGIN)
    W, H = S(TILE_W) + 2 * M, S(TILE_H) + 2 * M
    mask = path_mask((W, H), [(M, M, S(TILE_W), S(TILE_H), False)], K * 12 / 14)
    im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    base = cover(tile_base, S(TILE_W), S(TILE_H))
    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    layer.alpha_composite(base, (M, M))
    if logo is not None:
        bw, bh = (TILE_W - 16) * grow, 84.0 * grow
        bx = (TILE_MARGIN + 8) - (bw - (TILE_W - 16)) / 2
        by = (TILE_MARGIN + 8) - (bh - 84.0) / 2
        lg = fit(logo, S(bw), S(bh))
        layer.alpha_composite(lg, (S(bx) + (S(bw) - lg.width) // 2, S(by) + (S(bh) - lg.height) // 2))
    elif label:
        dr = ImageDraw.Draw(layer)
        f = ImageFont.truetype(FONT, S(22))
        tw = dr.textlength(label, font=f)
        dr.text((M + (S(TILE_W) - tw) / 2, M + S(TILE_H) / 2 - S(14)), label, font=f, fill=(230, 236, 246, 255))
    la = np.asarray(layer.split()[-1], np.float32) * np.asarray(mask, np.float32) / 255
    layer.putalpha(Image.fromarray(la.astype(np.uint8)))
    return over(im, layer)


def render_tile_border(focused):
    M = S(TILE_MARGIN)
    W, H = S(TILE_W) + 2 * M, S(TILE_H) + 2 * M
    r = (M, M, S(TILE_W), S(TILE_H))
    if focused:
        im = solid((W, H), SELECTION, stroke_mask((W, H), r, False, 6.5 * K, 12 * K), 80)
        return over(im, solid((W, H), SELECTION, stroke_mask((W, H), r, False, 2.2 * K, 12 * K), 225))
    return solid((W, H), PAD_BORDER_IDLE, stroke_mask((W, H), r, False, 1.5 * K, 12 * K), 200)


def render_star(outer=11):
    R = S(outer) + 2
    W = 2 * R
    big = Image.new("RGBA", (W * SS, W * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)

    def pts(scale):
        out = []
        for i in range(10):
            rad = (S(outer) if i % 2 == 0 else S(outer) * 0.42) * scale
            ang = -math.pi / 2 + i * math.pi / 5
            out.append(((R + rad * math.cos(ang)) * SS, (R + rad * math.sin(ang)) * SS))
        return out
    d.polygon(pts(1.0), fill=(20, 22, 28, 220))
    d.polygon(pts(0.78), fill=(255, 205, 60, 255))
    return big.resize((W, W), Image.LANCZOS)


def render_background(bg, w=900, h=610):
    im = cover(bg, w, h)
    over(im, Image.new("RGBA", (w, h), (8, 10, 16, 40)))
    return im


def render_disc(size, rgb=(228, 238, 255), alpha=64):
    W = size
    return solid((W, W), rgb, path_mask((W, W), [(0, 0, W, W, True)]), alpha)


def render_pill_parts(h=44, rgb_border=(255, 255, 255)):
    """Status toast (DrawStatusToast): dark pill a214 with a 1.5px border (tinted at runtime),
    cut into left cap / stretchable middle / right cap."""
    H = S(h)
    W = H * 3
    m = path_mask((W, H), [(0, 0, W, H, False)], 0)
    m = Image.new("L", (W * SS, H * SS), 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, W * SS - 1, H * SS - 1], radius=H * SS / 2, fill=255)
    m = m.resize((W, H), Image.LANCZOS)
    fill = solid((W, H), (14, 20, 28), m, 214)
    border = solid((W, H), rgb_border, stroke_mask((W, H), (1, 1, W - 2, H - 2), False, 1.5 * K, H / 2 - 1), 150)
    left, mid, right = fill.crop((0, 0, H, H)), fill.crop((H, 0, H + 4, H)), fill.crop((W - H, 0, W, H))
    bl, bm, br = border.crop((0, 0, H, H)), border.crop((H, 0, H + 4, H)), border.crop((W - H, 0, W, H))
    return left, mid, right, bl, bm, br


def render_separator(w=180, h=6):
    W, H = S(w), max(2, S(h))
    m = Image.new("L", (W * SS, H * SS), 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, W * SS - 1, H * SS - 1], radius=3 * K * SS, fill=255)
    m = m.resize((W, H), Image.LANCZOS)
    a = np.asarray(m, np.float32) * np.linspace(225, 135, H, dtype=np.float32)[:, None] / 255
    im = Image.new("RGBA", (W, H), (255, 255, 255, 0))
    im.putalpha(Image.fromarray(a.astype(np.uint8)))
    return im


def rounded_panel(art, w, h, radius):
    im = cover(art, w, h)
    m = Image.new("L", (w * SS, h * SS), 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, w * SS - 1, h * SS - 1], radius=radius * SS, fill=255)
    m = m.resize((w, h), Image.LANCZOS)
    im.putalpha(Image.fromarray((np.asarray(im.split()[-1], np.float32) * np.asarray(m, np.float32) / 255).astype(np.uint8)))
    return im


def render_settings_row(w=824, h=30):
    """Settings focus row: rounded 8, vertical gradient (58,110,176) -> (32,74,130)."""
    W, H = S(w), S(h)
    m = Image.new("L", (W * SS, H * SS), 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, W * SS - 1, H * SS - 1], radius=8 * K * SS, fill=255)
    m = m.resize((W, H), Image.LANCZOS)
    t = np.linspace(0, 1, H, dtype=np.float32)[:, None]
    rgb = np.zeros((H, W, 4), np.uint8)
    for c, (a, b) in enumerate(((58, 32), (110, 74), (176, 130))):
        rgb[..., c] = (a + (b - a) * t).astype(np.uint8)
    rgb[..., 3] = np.asarray(m)
    return Image.fromarray(rgb, "RGBA")



def _rrect(w, h, radius, fill_top, fill_bottom, border=None, border_w=1.5, alpha=255):
    W, H = S(w), S(h)
    m = Image.new("L", (W * SS, H * SS), 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, W * SS - 1, H * SS - 1], radius=radius * K * SS, fill=alpha)
    m = m.resize((W, H), Image.LANCZOS)
    im = _shape_fill(m, _vgrad(W, H, fill_top, fill_bottom))
    if border:
        over(im, solid((W, H), border[:3], stroke_mask((W, H), (0, 0, W, H), False, border_w * K, radius * K), border[3]))
    return im


def render_key(focused):
    """On-screen keyboard key (Settings row colours when focused)."""
    if focused:
        return _rrect(56, 44, 9, (58, 110, 176), (32, 74, 130), (255, 204, 51, 255), 2.2)
    return _rrect(56, 44, 9, (30, 40, 58), (18, 25, 38), (255, 255, 255, 60), 1.2, 225)


def render_qbox():
    return _rrect(560, 46, 12, (12, 18, 28), (20, 28, 42), (96, 200, 255, 200), 1.8, 235)


def render_hint_backing():
    """Dark strip behind the button hints when the Clear background shows the game."""
    return _rrect(300, 34, 17, (8, 12, 18), (8, 12, 18), None, 0, 170)


# ---------------------------------------------------------------- "Toypad" skin (a real LEGO Dimensions toypad)
# Drawn in LegoToypad window units ("toypad space": the same coordinates as PAD_CELLS); the
# overlay shrinks the whole toypad a little and moves it below the wordmark band.
TP_PLATE = (56, 46, 844, 558)
TP_RING_C = (450, 195)
TP_DISC_R, TP_RING_R = 92, 108
TP_REGION_BOX = [(100, 220, 408, 500), (356, 103, 544, 287), (492, 220, 800, 500)]


def _ss_canvas(w, h):
    return Image.new("RGBA", (int(round(w * K * SS)), int(round(h * K * SS))), (0, 0, 0, 0))


def _down(im, w, h):
    return im.resize((int(round(w * K)), int(round(h * K))), Image.LANCZOS)


def _vgrad(w, h, top, bottom):
    t = np.linspace(0, 1, h, dtype=np.float32)[:, None, None]
    a = np.asarray(top, np.float32)[None, None, :] * (1 - t) + np.asarray(bottom, np.float32)[None, None, :] * t
    return Image.fromarray(np.repeat(a, w, axis=1).astype(np.uint8), "RGB").convert("RGBA")


def _shape_fill(mask_l, fill_rgba_img):
    out = fill_rgba_img.copy()
    out.putalpha(Image.fromarray((np.asarray(out.split()[-1], np.float32) * np.asarray(mask_l, np.float32) / 255).astype(np.uint8)))
    return out


def _left_pad_rects():
    """The left pad: one white piece covering slots 0 (upper), 3 and 4 (lower row)."""
    l0, t0, r0, _ = PAD_CELLS[0]
    _, t3, _, b3 = PAD_CELLS[3]
    r4 = PAD_CELLS[4][2]
    return [(l0, t0, r0, b3), (l0, t3, r4, b3)]


def render_tp_region(region):
    """White pad piece for an LED region, in its bounding box (right = mirror of left)."""
    if region == 1:
        d = TP_DISC_R * 2
        W, H = d, d
        c = _ss_canvas(W, H)
        m = Image.new("L", c.size, 0)
        ImageDraw.Draw(m).ellipse([0, 0, c.width - 1, c.height - 1], fill=255)
        m = m.resize((S(W), S(H)), Image.LANCZOS)
        g = np.asarray(_vgrad(S(W), S(H), (246, 247, 249), (214, 218, 223)), np.float32)
        yy, xx = np.mgrid[0:S(H), 0:S(W)]
        rr = np.hypot(xx - S(W) / 2, yy - S(H) / 2) / (S(W) / 2)
        g[..., :3] *= (1.0 - 0.06 * np.clip(rr, 0, 1) ** 2)[..., None]
        im = _shape_fill(m, Image.fromarray(g.astype(np.uint8), "RGBA"))
        ring = stroke_mask((S(W), S(H)), (S(W) * 0.16, S(H) * 0.16, S(W) * 0.68, S(H) * 0.68), True, 1.6 * K, 0)
        over(im, solid((S(W), S(H)), (196, 201, 207), ring, 150))
        rim = stroke_mask((S(W), S(H)), (1, 1, S(W) - 2, S(H) - 2), True, 2.5 * K, 0)
        over(im, solid((S(W), S(H)), (200, 204, 210), rim, 200))
        return im
    l, t, r, b = TP_REGION_BOX[0]
    W, H = r - l, b - t
    m = Image.new("L", (S(W) * SS, S(H) * SS), 0)
    d = ImageDraw.Draw(m)
    for (x0, y0, x1, y1) in _left_pad_rects():
        d.rounded_rectangle([(x0 - l) * K * SS, (y0 - t) * K * SS, (x1 - l) * K * SS - 1, (y1 - t) * K * SS - 1],
                            radius=16 * K * SS, fill=255)
    # soften the inner corner where the upper slot meets the lower row
    ix, iy = (PAD_CELLS[0][2] - l) * K * SS, (PAD_CELLS[3][1] - t) * K * SS
    rad = 10 * K * SS
    d.rectangle([ix, iy - rad, ix + rad, iy], fill=255)
    d.ellipse([ix, iy - 2 * rad, ix + 2 * rad, iy], fill=0)
    m = m.resize((S(W), S(H)), Image.LANCZOS)
    im = _shape_fill(m, _vgrad(S(W), S(H), (243, 244, 246), (214, 218, 223)))
    edge = np.asarray(m, np.float32) - np.asarray(m.filter(ImageFilter.MinFilter(5)), np.float32)
    over(im, solid((S(W), S(H)), (196, 200, 206), Image.fromarray(np.clip(edge, 0, 255).astype(np.uint8)), 210))
    # faint seams marking the three figure spots
    seams = Image.new("RGBA", im.size, (0, 0, 0, 0))
    sd = ImageDraw.Draw(seams)
    seam = (196, 200, 207, 170)
    yh = (PAD_CELLS[3][1] - 4 - t) * K
    sd.line([((PAD_CELLS[0][0] + 12 - l) * K, yh), ((PAD_CELLS[0][2] - 12 - l) * K, yh)], fill=seam, width=max(2, int(1.6 * K)))
    xv = (PAD_CELLS[3][2] + 4 - l) * K
    sd.line([(xv, (PAD_CELLS[3][1] + 12 - t) * K), (xv, (PAD_CELLS[3][3] - 12 - t) * K)], fill=seam, width=max(2, int(1.6 * K)))
    im.alpha_composite(seams)
    return im if region == 0 else im.transpose(Image.FLIP_LEFT_RIGHT)


# ---------------------------------------------------------------- realistic LED light (Toypad skin)
# From the iFixit teardown: each side pad is a white L-shaped diffuser over an L-shaped LED board
# with one RGB LED under each of its three figure spots; the centre is a clear "illumination
# stand" over a single LED in the middle of the motherboard. Lit, a side pad glows all over with
# a bright, near-white hot spot over each LED; the clear centre glows brightest in the middle and
# along its rim. tp_lit* is the light's brightness across the piece (tinted with the LED colour),
# tp_core* the hot spots (tinted with a whitened LED colour).
TP_SPOTS_LEFT = [((PAD_CELLS[s][0] + PAD_CELLS[s][2]) / 2, (PAD_CELLS[s][1] + PAD_CELLS[s][3]) / 2) for s in (0, 3, 4)]


def _gauss(W, H, pts, sigma):
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
    g = np.zeros((H, W), np.float32)
    for (x, y) in pts:
        g = np.maximum(g, np.exp(-((xx - x) ** 2 + (yy - y) ** 2) / (2 * sigma * sigma)))
    return g


def render_tp_light(region, core):
    piece = render_tp_region(1 if region == 1 else 0)   # the right side is the left one mirrored
    W, H = piece.size
    alpha = np.asarray(piece.split()[-1], np.float32) / 255.0
    if region == 1:
        r = W / 2
        yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
        d = np.hypot(xx - r, yy - r) / r
        if core:
            a = np.exp(-(d / 0.16) ** 2) + 0.35 * np.exp(-((d - 0.9) / 0.05) ** 2)
            lum = np.ones_like(d)
        else:
            lum = 0.55 + 0.45 * np.exp(-(d / 0.35) ** 2) + 0.35 * np.exp(-((d - 0.88) / 0.08) ** 2)
            a = alpha
    else:
        l, t, _, _ = TP_REGION_BOX[0]
        pts = [((x - l) * K, (y - t) * K) for (x, y) in TP_SPOTS_LEFT]
        if core:
            a = np.clip(1.5 * _gauss(W, H, pts, 14 * K), 0, 1) * alpha
            lum = np.ones_like(a)
        else:
            glow = _gauss(W, H, pts, 30 * K)
            edge = np.asarray(Image.fromarray((alpha * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(10 * K)), np.float32) / 255
            lum = (0.74 + 0.26 * glow) * (0.82 + 0.18 * edge)
            a = alpha
    v = np.clip(lum, 0, 1) * 255
    rgba = np.dstack([v, v, v, np.clip(a, 0, 1) * 255]).astype(np.uint8)
    im = Image.fromarray(rgba, "RGBA")
    if region == 2:
        im = im.transpose(Image.FLIP_LEFT_RIGHT)
    return im


def render_tp_plate(badge):
    """Grey base plate: recessed bed around the pads, the dark ring round the centre disc,
    2x2 studs in the top corners and the LEGO badge at the foot of the centre channel."""
    pl, pt, pr, pb = TP_PLATE
    W, H = pr - pl, pb - pt
    X = lambda x: (x - pl) * K * SS
    Y = lambda y: (y - pt) * K * SS
    m = Image.new("L", (int(W * K * SS), int(H * K * SS)), 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, m.width - 1, m.height - 1], radius=28 * K * SS, fill=255)
    m = m.resize((S(W), S(H)), Image.LANCZOS)
    im = _shape_fill(m, _vgrad(S(W), S(H), (170, 173, 178), (122, 125, 131)))
    # bevel: light top edge, dark outline
    edge = np.asarray(m, np.float32) - np.asarray(m.filter(ImageFilter.MinFilter(7)), np.float32)
    over(im, solid((S(W), S(H)), (84, 87, 92), Image.fromarray(np.clip(edge, 0, 255).astype(np.uint8)), 230))
    hl = Image.new("L", m.size, 0)
    ImageDraw.Draw(hl).rounded_rectangle([S(6), S(4), S(W) - S(6), S(40)], radius=S(22), fill=60)
    over(im, solid((S(W), S(H)), (255, 255, 255), hl.filter(ImageFilter.GaussianBlur(S(6))), 255))
    # recessed bed
    bed = Image.new("L", (int(W * K * SS), int(H * K * SS)), 0)
    d = ImageDraw.Draw(bed)
    g = 9
    for side in (0, 1):
        for (x0, y0, x1, y1) in _left_pad_rects():
            if side:
                x0, x1 = 900 - x1, 900 - x0
            d.rounded_rectangle([X(x0 - g), Y(y0 - g), X(x1 + g), Y(y1 + g)], radius=22 * K * SS, fill=255)
    cx, cy = TP_RING_C
    d.ellipse([X(cx - TP_RING_R - 6), Y(cy - TP_RING_R - 6), X(cx + TP_RING_R + 6), Y(cy + TP_RING_R + 6)], fill=255)
    d.rounded_rectangle([X(PAD_CELLS[0][2] - 4), Y(cy + 40), X(PAD_CELLS[2][0] + 4), Y(PAD_CELLS[3][1] + 30)], radius=18 * K * SS, fill=255)
    d.rounded_rectangle([X(PAD_CELLS[4][2] - 4), Y(PAD_CELLS[3][1]), X(PAD_CELLS[5][0] + 4), Y(pb - 14)], radius=14 * K * SS, fill=255)
    bed = bed.resize((S(W), S(H)), Image.LANCZOS)
    over(im, solid((S(W), S(H)), (98, 101, 107), bed, 255))
    inner = np.asarray(bed, np.float32) - np.asarray(bed.filter(ImageFilter.MinFilter(5)), np.float32)
    over(im, solid((S(W), S(H)), (64, 66, 71), Image.fromarray(np.clip(inner, 0, 255).astype(np.uint8)), 200))
    # dark ring around the centre disc
    ring = Image.new("L", bed.size, 0)
    rx, ry = (cx - pl) * K, (cy - pt) * K
    ImageDraw.Draw(ring).ellipse([rx - TP_RING_R * K, ry - TP_RING_R * K, rx + TP_RING_R * K, ry + TP_RING_R * K], fill=255)
    over(im, solid((S(W), S(H)), (52, 54, 58), ring, 255))
    rim = stroke_mask((S(W), S(H)), (rx - TP_RING_R * K, ry - TP_RING_R * K, 2 * TP_RING_R * K, 2 * TP_RING_R * K), True, 2.0 * K, 0)
    over(im, solid((S(W), S(H)), (140, 144, 150), rim, 180))
    # studs
    for sx in (178, 722):
        for ox in (-13, 13):
            for oy in (-13, 13):
                x, y = (sx + ox - pl) * K, (122 + oy - pt) * K
                r = 10.5 * K
                st = Image.new("L", bed.size, 0)
                ImageDraw.Draw(st).ellipse([x - r + 2, y - r + 3, x + r + 2, y + r + 3], fill=150)
                over(im, solid((S(W), S(H)), (60, 62, 66), st.filter(ImageFilter.GaussianBlur(2)), 255))
                st = Image.new("L", bed.size, 0)
                ImageDraw.Draw(st).ellipse([x - r, y - r, x + r, y + r], fill=255)
                over(im, solid((S(W), S(H)), (176, 179, 184), st, 255))
                st = Image.new("L", bed.size, 0)
                ImageDraw.Draw(st).ellipse([x - r * 0.62, y - r * 0.7, x + r * 0.35, y + r * 0.1], fill=120)
                over(im, solid((S(W), S(H)), (255, 255, 255), st.filter(ImageFilter.GaussianBlur(2)), 255))
    # badge
    bs = 32
    b = badge.resize((S(bs), S(bs)), Image.LANCZOS)
    im.alpha_composite(b, (int((450 - bs / 2 - pl) * K), int((pb - 26 - bs / 2 - pt) * K)))
    return im

# ---------------------------------------------------------------- main

FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"


SOFT = ("ledo", "ledp", "glowsel", "glowmove", "tileglow", "btnhalo", "panel", "spanel")


def main(ltp: Path, web: Path):
    A = ltp / "Assets"
    global FONT
    if (A / "Fonts" / "sfpro.ttf").is_file():
        FONT = str(A / "Fonts" / "sfpro.ttf")
    cat = json.loads((web / "catalog.json").read_text(encoding="utf-8"))
    img = lambda url: web / "img" / url.split("/img/")[1].split("?")[0]
    out = web / "ui"
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    sizes = []

    def save(im, name):
        sizes.append((name, im.width, im.height))
        # soft art (halos, glows, the roster panel) is stored at half size: the overlay scales
        # every texture to its widget, and ShellUI's texture memory is small
        if name.startswith(SOFT):
            im = im.resize((max(1, im.width // 2), max(1, im.height // 2)), Image.LANCZOS)
        if name.startswith("wall"):   # opaque photos: JPEG keeps the pack small
            im.convert("RGB").save(out / (name + ".jpg"), quality=88, optimize=True)
            return
        im.save(out / (name + ".png"), optimize=True)
    n = 0

    # chrome; wallpapers in LegoToypad's kBackgroundChoices order (wall0 = Default)
    walls = sorted((A / "Wallpapers").glob("Background*.*"),
                   key=lambda p: int(p.stem.split("_")[1]) if "_" in p.stem else 1)
    if not walls:
        save(render_background(load(img(cat["background"]))), "wall0"); n += 1
    for wi, wp in enumerate(walls):
        save(render_background(load(wp)), "wall%d" % wi); n += 1
    # pad skins (kPadSkins: Default, Old, Plain); skin 0 keeps the plain pad%d names
    PAD_FILES = ("left_upper", "Center", "right_upper", "left_lower_left", "left_lower_right",
                 "right_lower_left", "right_lower_right")
    skins = [d for d in ("default", "Old", "Plain") if (A / "Pads" / d).is_dir()]
    for si, d in enumerate(skins[1:], 1):
        for i, f in enumerate(PAD_FILES):
            save(render_pad_art(load(A / "Pads" / d / (f + ".png")), i), "pk%d_%d" % (si, i)); n += 1
    # Settings screen
    st = A / "Tiles" / "Settings_tile.png"
    save(rounded_panel(load(st if st.is_file() else img(cat["charactersTile"])), 840, 420, 14), "spanel"); n += 1
    save(render_settings_row(), "srow"); n += 1
    save(Image.new("RGBA", (8, 2), (255, 204, 51, 70)), "rule"); n += 1
    save(fit(load(img(cat["settingsText"])), S(234), S(40)), "settext"); n += 1
    wm = load(img(cat["wordmark"]))
    wm = wm.crop(wm.getbbox())
    save(fit(wm, S(460), S(90)), "wordmark"); n += 1
    badge = wm.crop((0, 0, wm.height, wm.height))          # the LEGO badge half of the wordmark
    save(fit(badge, S(48), S(48)), "legobadge"); n += 1
    # "Ported by SpiderNic96" in the by-mark's own typeface (Compacta)
    pf = A / "Fonts" / "Compacta Regular.woff2"
    try:
        from fontTools.ttLib import TTFont
        import io
        f = TTFont(str(pf)); f.flavor = None
        buf = io.BytesIO(); f.save(buf); buf.seek(0)
        ft = ImageFont.truetype(buf, 120)
    except Exception:
        ft = ImageFont.truetype(FONT, 100)
    pm = Image.new("RGBA", (1600, 200), (0, 0, 0, 0))
    ImageDraw.Draw(pm).text((10, 10), "Ported by SpiderNic96", font=ft, fill=(255, 255, 255, 255))
    pm = pm.crop(pm.getbbox())
    save(pm.resize((int(pm.width * S(22) / pm.height), S(22)), Image.LANCZOS), "portmark"); n += 1
    # Toypad skin
    save(render_tp_plate(badge), "tp_plate"); n += 1
    for r in range(3):
        piece = render_tp_region(r)
        save(piece, "tp_r%d" % r); n += 1
        # LED surface: the whole piece, solid, so a lit pad has no white trim left around it
        m = Image.new("RGBA", piece.size, (255, 255, 255, 0))
        m.putalpha(piece.split()[-1].point(lambda v: 255 if v > 40 else v * 255 // 40))
        save(m, "tp_m%d" % r); n += 1
        save(render_tp_light(r, False), "tp_lit%d" % r); n += 1
        save(render_tp_light(r, True), "tp_core%d" % r); n += 1
    # search screen
    save(render_key(False), "key"); n += 1
    save(render_key(True), "keysel"); n += 1
    save(render_qbox(), "qbox"); n += 1
    save(render_hint_backing(), "hintbg"); n += 1
    save(fit(load(img(cat["byMark"])), S(400), S(30)), "bymark"); n += 1
    pads = [load(img(u)) for u in cat["pads"]]
    for i in range(7):
        save(render_pad_art(pads[i], i), "pad%d" % i); n += 1
    for kind in SHAPES:
        save(render_mask(kind), "mask%s" % kind)
        save(render_outline(kind, 2.0), "line%s" % kind)
        save(render_selected_outline(kind), "sel%s" % kind)
        w, h = SHAPES[kind]
        save(render_focus_glow(w, h, kind == "D", 14, SELECTION, True), "glowsel%s" % kind)
        save(render_focus_glow(w, h, kind == "D", 14, MOVE_SOURCE, True), "glowmove%s" % kind)
        n += 5
    for r in range(3):
        im, _ = led_halo(PAD_CELLS, r, K)
        save(im, "ledo%d" % r)
        cells, s = peek_cells()
        im, _ = led_halo(cells, r, 1.0)
        save(im, "ledp%d" % r)
        n += 2
    for name, key in (("load", "loadBtn"), ("move", "moveBtn"), ("clear", "clearBtn")):
        save(fit(load(img(cat[key])), S(46), S(46)), "btn_" + name); n += 1
    save(render_disc(S(54)), "btnhalo"); n += 1
    save(render_portrait_focus(), "pfocus"); n += 1
    save(render_plus(), "plus"); n += 1
    save(render_ring(), "ring"); n += 1
    save(render_tile_border(False), "tileline"); n += 1
    save(render_tile_border(True), "tilesel"); n += 1
    save(render_focus_glow(TILE_W, TILE_H, False, 12, SELECTION), "tileglow"); n += 1
    save(render_star(), "star"); n += 1
    save(render_separator(), "sep"); n += 1
    save(rounded_panel(load(img(cat["charactersTile"])), 780, 470, 14), "panel"); n += 1
    sb = load(img(cat["scrollBar"]))
    save(sb.resize((S(10), S(160)), Image.LANCZOS), "scroll"); n += 1
    for i, part in enumerate(render_pill_parts()):
        save(part, ["toastl", "toastm", "toastr", "toastbl", "toastbm", "toastbr"][i]); n += 1
    icons = A / "ControllerIcons" / "DualShock4"
    for g in ("A", "B", "X", "Y", "LB", "RB", "LT", "RT", "DpadUp", "DpadDown", "DpadRight", "Start",
              "LeftStickClick", "RightStickClick"):
        p = icons / (g + ".png")
        if p.is_file():
            save(fit(load(p), S(40), S(40)), "g_" + g); n += 1
    sort_icons = {0: cat["sorts"][0]["icon"]}
    for srt in cat["sorts"]:
        save(fit(load(img(srt["icon"])), S(300), S(62)), "sort%d" % srt["id"]); n += 1
    save(fit(load(img(cat["favoritesIcon"])), S(90), S(66)), "favlogo"); n += 1

    # worlds
    tile_base = load(img(cat["worldTile"]))
    rows = []
    for wi, f in enumerate(cat["franchises"]):
        logo = load(img(f["logo"]))
        grow = 1.15 if f["name"] in ("Ghostbusters", "Scooby-Doo!") else 1.3 if f["name"] == "The Simpsons" else 1.0
        save(render_world_tile(tile_base, logo, grow=grow), "w%d" % wi)
        save(fit(logo, S(360), S(62)), "wl%d" % wi)
        n += 2
        starter = f["name"] in ("DC Comics", "The Lord of the Rings", "The LEGO Movie")
        rows.append("W\t%d\t%s\t%d\t%d\t%d" % (wi, f["name"], f["year1"], f["year2"], starter))
    save(render_world_tile(tile_base, load(img(cat["favoritesIcon"]))), "wfav"); n += 1

    # portraits, one per bin
    done = set()

    def portrait(e):
        nonlocal n
        b = e["bin"]
        if b in done:
            return
        done.add(b)
        p = img(e["portrait"])
        photo = load(p) if p.is_file() and p.name != "0" else None
        save(render_portrait(photo, hexrgb(e["color"])), "p%d" % b)
        n += 1

    for wi, f in enumerate(cat["franchises"]):
        for c in f["characters"]:
            portrait(c)
            rows.append("C\t%d\t%d\t%s\t%s" % (wi, c["bin"], c["name"], c["color"]))
        for gi, v in enumerate(f["vehicles"]):
            rows.append("G\t%d\t%d\t%s" % (wi, gi, v["base"]))
            for b in v["builds"]:
                portrait(b)
                rows.append("V\t%d\t%d\t%d\t%d\t%s\t%s" % (wi, gi, b["build"], b["bin"], b["name"], b["color"]))
    (out / "catalog.tsv").write_text("\n".join(rows) + "\n", encoding="utf-8")
    (out / "skins.txt").write_text("\n".join(["Toypad", "Default"] + skins[1:]) + "\n", encoding="utf-8")
    (out / "sizes.tsv").write_text("".join("%s\t%d\t%d\n" % e for e in sizes), encoding="utf-8")
    size = sum(p.stat().st_size for p in out.iterdir())
    print("ui pack: %s  (%d textures, %d portraits, %.1f MB)" % (out, n, len(done), size / 1e6))


if __name__ == "__main__":
    main(Path(sys.argv[1]), Path(sys.argv[2]))
