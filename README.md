# RadarMASH

Open firmware for the GeekMagic Radar 360 (model `radar21en`): a 2.1" round, 360x360 desk display that shows the aircraft
around you. Named in honour of Radar O'Reilly, who always heard the choppers first.

**Status: first release, v0.1.0.** It runs as the everyday firmware on the author's radar: sweep, map, live aircraft with
checked routes, saved settings, a settings page, Wi-Fi setup, and a way back to the original firmware. It is still young.
Read the firmware section below before flashing anything: the case is sealed and Wi-Fi is the only way in.

## Layout

| Folder | What it is |
|---|---|
| `core/` | Portable C++ drawing code and the radar screen. No hardware dependencies. |
| `sim/` | Desktop simulator: runs `core/` on your computer and shows the screen in a web browser, with demo or live aircraft. |
| `tests/` | Unit tests for `core/`. |
| `firmware/` | Device firmware (PlatformIO, Arduino for ESP32-S3). |
| `web/` | The settings page, served by both the simulator and the device. |
| `tools/` | Build helpers. |

The firmware builds the same `core/` code as the simulator, so what you see in one is what the other draws.

## Running the simulator

You need a C++17 compiler and `make` (macOS or Linux), plus `curl` if you want the map.

    make -C sim run

Then open http://localhost:8360: the round screen, with the settings page underneath. It is the same page the device
serves, so anything set up here works the same there. The simulator keeps its settings in
`~/.config/radarmash/simulator.json`.

To save one frame instead: `sim/radarmash-sim --shot frame.bmp --t 9000 --select 0`

## Live aircraft

    make run ARGS="--live --home LAT,LON"

shows real aircraft around that position instead of the demo traffic. Your position is only ever given on the command
line; it is not stored in the project.

- **Positions** come from [adsb.fi](https://adsb.fi) or [adsb.lol](https://adsb.lol), two free community services that
  serve the same "readsb" format. If one stops answering, the simulator switches to the other. The refresh rate is
  settable; please keep it gentle (the default is every 5 seconds, the minimum 2).
- **Airline names and routes** come from [adsbdb](https://www.adsbdb.com), looked up a few callsigns at a time and
  remembered. These services hold what a flight number usually flies, and flight numbers get reused, so some answers
  are wrong for the aircraft overhead. Each route is therefore checked against where the aircraft is, which way it is
  heading and whether it is climbing or descending. If it does not fit, or there is none, [hexdb](https://hexdb.io) is
  asked and held to the same test. A route that fails is not shown: no route is better than a wrong one.
- Between updates each aircraft is carried on along its track, so it glides rather than jumps.
- At most 10 aircraft are shown by default, the nearest first; the limit is settable.

## Map underlay

The radar can draw over a map built from standard 256-pixel web map tiles. The source is a URL pattern such as
`https://tile.openstreetmap.org/{z}/{x}/{y}.png`, so any provider that serves tiles this way can be used; the simulator page
has a box for the pattern, the credit line the provider asks for, a style (as is, dark, green) and brightness. Light maps
are turned dark in software. The zoom follows the radar range.

The default is OpenStreetMap's own tile server, which needs no key. Its
[usage policy](https://operations.osmfoundation.org/policies/tiles/) applies: tiles are cached (the simulator keeps them
in `~/.cache/radarmash` for 30 days), only what is on screen is fetched, and the credit stays visible. Map data is
(c) OpenStreetMap contributors.

## Firmware (development build)

**Read this before flashing anything.** The Radar 360's case is sealed and it has no USB data connection, so Wi-Fi is
the only way to load firmware. A build that cannot join Wi-Fi and accept an update, once made permanent, cannot be
replaced. The firmware is written around that:

- It never makes itself permanent at start-up. Until it is confirmed, any reset or power cycle returns to the firmware
  in the other slot (the stock firmware, if that is what you came from).
- Left alone for 30 minutes while unconfirmed, it goes back by itself. Holding the button for 8 seconds does the same.
- Its own updater writes only the other slot, and confirms itself first so a failed upload never leaves nothing bootable.
- Only ever upload the app image. Never write a bootloader, partition table or "factory" image.

To build, run `firmware/build.sh` (needs PlatformIO). No Wi-Fi details are compiled in: the firmware joins the network
the device already knows, and otherwise puts up a hotspot called `RadarMASH-xxxx`; join it and the setup page appears
(or open http://192.168.4.1). New details are only stored once they have worked. Over that hotspot only the setup page
can be reached, never the settings or the updater. That produces `radarmash-radar21en.bin`, which the stock firmware's updater
accepts: `curl -H 'Content-Type: application/octet-stream' --data-binary @radarmash-radar21en.bin http://DEVICE/api/ota`.
Stock refuses uploads for about a minute after it boots. The device then serves the settings page at its address.
Settings, including where the radar is, are stored on the device only.

## Settings

One page (`web/admin.html`) sets everything: which features are shown, the font, brightness, range, live or demo aircraft, where
the radar is, the data source and refresh rate, and the map source, style and brightness. It talks JSON:

    GET  /api/settings            everything, as one object
    POST /api/settings            any subset, e.g. {"trails": false, "range_km": 80}
    GET  /api/status              what the radar is doing
    POST /api/press, /api/hold    the button

Values out of range are clamped and anything unrecognised is ignored, so an old page and new firmware (or the reverse)
get along.

## Fonts

The settings page offers the original pixel font and four typefaces, each in three sizes with soft edges. To add or
change one, edit the list in `tools/make_fonts.py` and run it (it needs Pillow, and downloads the font files into
`build/`); it rewrites `core/fonts_data.cpp`, which is checked in so ordinary builds need neither. Only fonts that may
be redistributed in a converted form under their own name are suitable.

## Tests

    make test      # unit tests for the drawing code and radar behaviour
    make check     # what CI runs: the tests, then a simulator smoke test

The tests need only a C++ compiler. CI runs them on every push with both GCC and Clang, with memory and
undefined-behaviour checking switched on. Run one group with `build/tests <part of a test name>`.

One test matters more than the rest: drawing the screen in narrow strips, as the device does, must give exactly the same
picture as drawing it whole.

## Licence

GPL-3.0-or-later; see `LICENSE`. The firmware links the PNGdec library (Apache-2.0), fetched at build time. The fonts in
`core/fonts_data.cpp` are bitmaps of B612 Mono, JetBrains Mono, Roboto Condensed and VT323, each under the SIL Open Font
License 1.1 (see `licenses/`). Two bundled files keep their own licences, reproduced inside them: `core/font5x7.h`
(Adafruit GFX Library, BSD) and `sim/third_party/stb_image.h` (stb, public domain / MIT; simulator only).

RadarMASH is an independent hobby project. It is not affiliated with GeekMagic, and contains none of their code.
