# RadarMASH

Open firmware for the GeekMagic Radar 360 (model `radar21en`): a 2.1" round, 360x360 desk display that shows the aircraft
around you. Named in honour of Radar O'Reilly, who always heard the choppers first.

**Status: early development. There is no firmware to flash yet.** What exists today is the drawing code and a desktop
simulator.

## Layout

| Folder | What it is |
|---|---|
| `core/` | Portable C++ drawing code and the radar screen. No hardware dependencies. |
| `sim/` | Desktop simulator: runs `core/` on your computer and shows the screen in a web browser. |
| `tests/` | Unit tests for `core/`. |

The device firmware will be added as its own folder and will build the same `core/` code.

## Running the simulator

You need a C++17 compiler and `make` (macOS or Linux), plus `curl` if you want the map.

    make -C sim run

Then open http://localhost:8360. "Short press" and "Long press" stand in for the device's single button.

To save one frame instead: `sim/radarmash-sim --shot frame.bmp --t 9000 --select 0`

## Map underlay

The radar can draw over a map built from standard 256-pixel web map tiles. The source is a URL pattern such as
`https://tile.openstreetmap.org/{z}/{x}/{y}.png`, so any provider that serves tiles this way can be used; the simulator page
has a box for the pattern, the credit line the provider asks for, a style (as is, dark, green) and brightness. Light maps
are turned dark in software. The zoom follows the radar range.

The default is OpenStreetMap's own tile server, which needs no key. Its
[usage policy](https://operations.osmfoundation.org/policies/tiles/) applies: tiles are cached (the simulator keeps them
in `~/.cache/radarmash` for 30 days), only what is on screen is fetched, and the credit stays visible. Map data is
(c) OpenStreetMap contributors.

## Tests

    make test      # unit tests for the drawing code and radar behaviour
    make check     # what CI runs: the tests, then a simulator smoke test

The tests need only a C++ compiler. CI runs them on every push with both GCC and Clang, with memory and
undefined-behaviour checking switched on. Run one group with `build/tests <part of a test name>`.

One test matters more than the rest: drawing the screen in narrow strips, as the device does, must give exactly the same
picture as drawing it whole.

## Licence

GPL-3.0-or-later; see `LICENSE`. Two bundled files keep their own licences, reproduced inside them: `core/font5x7.h`
(Adafruit GFX Library, BSD) and `sim/third_party/stb_image.h` (stb, public domain / MIT; simulator only).

RadarMASH is an independent hobby project. It is not affiliated with GeekMagic, and contains none of their code.
