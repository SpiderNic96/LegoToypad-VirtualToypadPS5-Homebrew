/* Virtual Toy Pad console overlay UI (LegoToypad's picker, drawn in the PS5 system UI).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>

typedef struct {
    int enabled;          /* overlay=1 in config.ini */
    uint32_t peek_mask;   /* ScePad buttons held for the sneak peek (default L2) */
    int peek_size;        /* 0 off, 1 small, 2 medium, 3 large (LegoToypad's sizes) */
    int fast_load;        /* close the picker after a LOAD (LegoToypad "fast loading") */
    int tex_mode;         /* 0 auto, 1 pixel upload, 2 PNG files in /Temp */
    uint32_t combo;       /* menu_buttons: still held when the picker opens, not a press */
    int background;       /* LegoToypad order: 0 default wallpaper, 1 Clear, 2.. wallpapers 2..26 */
    int opacity;          /* whole-picker opacity percent, 20..100 (LegoToypad default 92) */
    int pad_skin;         /* 0 Toypad (a real toypad), 1 Default, 2 Old, 3 Plain (LegoToypad's) */
    int ui_size;          /* 0 Auto (110%), 1 90%, 2 100%, 3 110%, 4 118% */
    int text_size;        /* 0 Auto (125%), 1 100%, 2 115%, 3 130%, 4 150% */
} cui_config;

/* implemented by the payload (main.c): persists settings changed on the TV Settings screen */
void cui_save_config(const cui_config *c);

void cui_configure(const cui_config *c);
int cui_start(void);
