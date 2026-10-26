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
} cui_config;

void cui_configure(const cui_config *c);
int cui_start(void);
