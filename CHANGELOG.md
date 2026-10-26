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