/* UI sound effects for the console picker (port of LegoToypad's Assets/SFX).
 *
 * The payload decodes {Navigate,Select,Move,Remove}.wav from /data/ldmod/web/sfx/ once and hands the PCM to the
 * overlay renderer inside SceShellUI (which owns the PS5 audio stack); the renderer plays it.
 * Missing files are simply silent. Volume (0-100) is the "Sound volume" row in the picker's Settings
 * and sfx_volume= in config.ini. */
#pragma once
#include <stdint.h>

enum { SFX_NAVIGATE, SFX_SELECT, SFX_MOVE, SFX_REMOVE, SFX_COUNT };

typedef int (*sfx_send_fn)(uint16_t type, const void *a, uint32_t na, const void *b, uint32_t nb);

void sfx_init(void);                                   /* background thread: load the WAV files */
void sfx_link_up(int shellui_pid, sfx_send_fn send);   /* overlay link is up: hand sounds to SceShellUI */
void sfx_set_volume(int percent);                      /* 0..100, 0 = off; applies immediately */
void sfx_play(int which);                              /* a new sound cuts the previous one */
void sfx_status(const char *msg);                      /* renderer status line: "SFX!" lines become a notification */
