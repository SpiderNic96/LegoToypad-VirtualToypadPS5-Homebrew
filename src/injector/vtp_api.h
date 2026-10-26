/* Services the console overlay (cui.c) shares with the phone web remote (webremote.c).
 * Slots are in LegoToypad order: 0 left-upper, 1 centre, 2 right-upper, 3 left-lower-left,
 * 4 left-lower-right, 5 right-lower-left, 6 right-lower-right.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* pad state from the latest monitor snapshot; returns 0 while the game is linked */
int vtp_pad(int used[7], int bin[7], uint32_t fid[7]);
/* figure commands with LegoToypad timing; block until the game handled them (0 = ok) */
int vtp_load(int slot, int bin, char *msg, size_t n);
int vtp_move(int src, int dst, char *msg, size_t n);
int vtp_clear(int slot, char *msg, size_t n);
int vtp_clearall(char *msg, size_t n);
/* favourites / sort / LED mirror preferences (shared with the phone) */
int vtp_favorites(int *bins, int max);
int vtp_is_favorite(int bin);
int vtp_toggle_favorite(int bin);           /* 1 = now a favourite */
int vtp_sort_get(void);
void vtp_sort_set(int sort);
int vtp_led_mirror(void);
void vtp_led_mirror_set(int on);
/* live LED regions (0 left, 1 centre, 2 right): mode 0 off 1 solid 2 flash 3 fade,
 * colour already calibrated to display RGB, intensity 0..1 */
void vtp_leds(int mode[3], uint8_t rgb[3][3], float intensity[3]);
int vtp_leds_animating(void);
/* status line shared with the phone */
void vtp_status_set(const char *msg);

/* console overlay input published by the game hook (ld_header.ext) */
typedef struct {
    int valid;
    uint32_t mode, open, open_seq, close_req, close_ack, raw, wr, heartbeat;
    uint32_t ring[32];
} vtp_ext;
void vtp_ext_snapshot(vtp_ext *e);
/* queued for the monitor thread: mode < 0 leaves it, close != 0 asks the hook to close */
void vtp_ext_request(int mode, int close);
