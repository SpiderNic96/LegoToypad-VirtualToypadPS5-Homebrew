# Virtual Toy Pad by HeitorSpectre

[![Stars](https://img.shields.io/github/stars/HeitorSpectre/Virtual-Toy-Pad?style=flat&label=stars&labelColor=555&color=007ec6)](https://github.com/HeitorSpectre/Virtual-Toy-Pad/stargazers)
[![Downloads](https://img.shields.io/github/downloads/HeitorSpectre/Virtual-Toy-Pad/total?style=flat&label=downloads&labelColor=555&color=4c1)](https://github.com/HeitorSpectre/Virtual-Toy-Pad/releases)

An unofficial ELF payload that lets you play the PS4 version of LEGO Dimensions on a jailbroken PS5 with a virtual Toy Pad, without the physical accessory. Select characters and vehicles using your DualSense.

## Compatibility

- **LEGO Dimensions:** CUSA00935, **update 1.23**. Game offsets are version-specific.
- **Console:** Jailbroken PS5 with a working ELF loader (such as ELFLDR or a compatible etaHEN loader).
- Other game versions, regions, and PS5 environments have not been validated.

## Installation and controls

1. **Close LEGO Dimensions** before loading the payload.
2. Extract `Virtual-Toy-Pad.elf` from the release ZIP and send it to your PS5 ELF loader.
3. Launch LEGO Dimensions and wait for the virtual Toy Pad to be detected.
4. Press **L3 + R3** to open/close the Virtual Toy Pad menu. Navigate with the D-pad or left analog stick.
5. **X:** select; **Circle:** back; **L1/R1:** character/vehicle category; **L2/R2:** pages.
6. Within the grid, hold **Square + D-pad** to adjust native borders, **Square + L2/R2** to adjust the selected border size, or **Square + Triangle** to cycle border modes.

The payload stores data in `/data/ldmod` on the PS5. PSN sign-in is not required.

### Console overlay (LegoToypad picker on the TV)

With `overlay=1` in `/data/ldmod/config.ini` (the default) and the `ui` art folder copied to
`/data/ldmod/web/ui`, the payload loads a small renderer into the PS5 system UI and **L3 + R3**
opens harrysof's LegoToypad picker over the game instead of the text menu:

- **Pad screen:** D-pad/left stick picks a pad; **X** opens Load / Move / Clear; **Square** moves
  the figure; **R1** goes straight to loading; **L1** clears; **Triangle** opens Settings;
  **Circle** closes.
- **Settings** (LegoToypad's list): background (26 wallpapers or **Clear**, so the game shows
  through), pad skin (**Toypad**, a real toypad, or LegoToypad's Default / Old / Plain), window
  transparency (20-100%), UI size and text size (Auto suits a TV), sneak peek size,
  toypad LEDs, fast loading, the phone remote's address, clear all pads, reset to defaults.
  Changes are saved to `config.ini`.
- **Worlds / roster / builds:** **X** opens or loads, **Circle** goes back, **L1/R1** change the
  sort (Default, Year 1, Year 2, User, Favorites, Story), **Triangle** favourites a figure,
  **Square** opens Search.
- **Search:** type on the on-screen keyboard (**Square** deletes); results update as you type.
  **Up** from the keyboard moves into the results, **X** loads.
- **Sneak peek:** hold **L2** during gameplay to see the pads, occupants and LEDs in the screen
  corners (`peek_buttons`, `peek_size`).
- `fast_load=1` closes the picker after a figure is loaded. `overlay_textures=2` switches the art
  to PNG files if it stays blank on your firmware.

If the renderer cannot start (see `/data/ldmod/ldinject.log` and `/system_tmp/vtpovl_shellui.log`)
the classic text menu keeps working. Do not run another SceShellUI overlay (Common FPS, HUD) at the
same time. Build the art pack from a LegoToypad checkout with
`python3 tools/build_ui_pack.py <LegoToypad> <web folder>`.

## Build from source

On Linux or WSL, install `clang`, `ld.lld`, `make`, `gcc`, `xxd`, Python 3, `pyelftools`, and the [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk).

Set `PS5_PAYLOAD_SDK` to your SDK location (default: `$HOME/ps5-payload-sdk`). Then run:

```bash
bash build.sh
```

The compiled payload is `build/ldinject.elf`. The build generates `ldhook.bin` and `blob.h` automatically; these intermediate files are not version-controlled.

## Repository contents

This private repository includes essential source code, build scripts, figure tables, and documentation. It contains no game files, EBOOT dumps, firmware, PSN content, credentials, or diagnostic captures.

## Credits, license, and disclaimer

Created by **HeitorSpectre**. This is an independent, unofficial project not affiliated with LEGO, Warner Bros., or the game's developers. All trademarks belong to their respective owners. Users are responsible for using their own game copies lawfully.

The console overlay re-implements the picker of [LegoToypad](https://github.com/harrysof/LegoToypad) by **harrysof** (MIT) and builds its art from LegoToypad's assets. Its SceShellUI loader in `src/overlay` comes from **Common FPS** by **porhe911 / erickdavestech** and `shsrv` by **John Törnblom** (GPL-3.0-or-later); `stb_image.h` is by Sean Barrett (public domain / MIT).

`src/injector/pt.c` and `src/injector/pt.h` include work by **John Törnblom (2024)** under **GPL-3.0-or-later**. The original notices are preserved. See `COPYING` for the license terms. The release ZIP includes the corresponding source code in `Codigo-Fonte.zip`.
