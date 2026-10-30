/* UI sound effects for the console picker (port of LegoToypad's Assets/SFX).
 * Drop Navigate.wav / Select.wav / Move.wav / Remove.wav into /data/ldmod/sfx/ on the PS5.
 * Missing files are simply silent. Optional /data/ldmod/sfx/volume.txt holds 0-100 (default 70). */
#pragma once

enum { SFX_NAVIGATE, SFX_SELECT, SFX_MOVE, SFX_REMOVE, SFX_COUNT };

void sfx_init(void);          /* starts a background thread; never blocks, never fails loudly */
void sfx_play(int which);     /* restarts playback (a new sound cuts the previous one) */
