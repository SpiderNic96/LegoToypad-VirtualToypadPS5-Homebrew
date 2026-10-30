# 2.0.2 — Console picker on firmware 10.xx (SpiderNic96)

- **Console picker now starts where ShellUI's code can't be read through ptrace.** A 10.40 log
  showed every startup probe failing with EFAULT (`status=-5 read=14`), so the picker never came
  up and ShellUI later restarted. Reads and writes of ShellUI's code now fall back to the kernel's
  physical-memory map. The bytes are still checked before and after each write, and the original
  is put back on any mismatch.

# 2.0.1 — Firmware 7.00–13.60, safer hooking (SpiderNic96)

- **Console picker on firmware 7.00–13.60.** 9.60 without etaHEN now uses the background render
  mode instead of having no picker. On firmware that refuses a ptrace write into ShellUI's code
  page, the patch is retried by briefly unlocking the page (as 13.60 already did). The bytes are
  still verified, and the original is put back on any mismatch. A host test checks every firmware
  from 7.00 to 13.60 has a way in.
- **Cold start:** the payload now waits up to 90 s for the game to load its USB library, instead
  of giving up after 10 s with "restart the game".
- **Safer hooking:**
  - Import slots are only replaced if they hold what the loader put there (the bound function,
    or a stub inside a loaded module). The loader's not-bound-yet marker, a foreign hook or
    garbage is never overwritten, and is logged as `not hooking …`.
  - The game stays paused while its slots are rewritten.

# 2.0.0 — Console picker, phone remote, EU 1.24, 60 FPS and resolution (SpiderNic96)

- **EU CUSA01176 v1.24** support alongside US CUSA00935 v1.23 (code layout detected at start).
- **60 FPS** and **internal resolution 1080 / 1440 / 2160** patches (connorh315's), verified
  before writing; `fps60=` and `resolution=` in `config.ini`.
- **Phone remote**: LegoToypad's web app served by the PS5 on port 8765, with live pads, LEDs,
  favourites and sorts.
- **Console picker**: LegoToypad's picker drawn in SceShellUI — pads with live LEDs, Load / Move /
  Clear, worlds, rosters, vehicle builds, favourites, sorts, search with an on-screen keyboard,
  sneak peek (hold L2), fast loading, Settings (wallpapers or Clear, pad skins, realistic or flat
  LEDs, transparency, UI and text size). Falls back to the classic menu if it can't start.
- **Toypad pad skin** modelled on the real hardware, with diffused LED light and hot spots.
- `config.ini` grows the overlay settings; older configs are upgraded in place.

# Release 1.0

- Added **By HeitorSpectre** to the Virtual Toy Pad menu title.
- Virtual Toy Pad menu with D-pad and left analog stick navigation.
- Character and vehicle selection grids with page navigation.
- Original game borders for normal and selected icons, with saved calibration.
- Independent X/Y position adjustments for the selected icon.
- Standalone ELF payload for a jailbroken PS5 running LEGO Dimensions **CUSA00935, update 1.23**.
- Minor alignment differences may remain for certain icons.