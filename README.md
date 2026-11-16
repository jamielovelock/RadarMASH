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

You need a C++17 compiler and `make` (macOS or Linux).

    make -C sim run

Then open http://localhost:8360. "Short press" and "Long press" stand in for the device's single button.

To save one frame instead: `sim/radarmash-sim --shot frame.bmp --t 9000 --select 0`

## Tests

    make test      # unit tests for the drawing code and radar behaviour
    make check     # what CI runs: the tests, then a simulator smoke test

The tests need only a C++ compiler. CI runs them on every push with both GCC and Clang, with memory and
undefined-behaviour checking switched on. Run one group with `build/tests <part of a test name>`.

One test matters more than the rest: drawing the screen in narrow strips, as the device does, must give exactly the same
picture as drawing it whole.

## Licence

GPL-3.0-or-later; see `LICENSE`. `core/font5x7.h` comes from the Adafruit GFX Library and stays under its BSD licence,
reproduced in that file.

RadarMASH is an independent hobby project. It is not affiliated with GeekMagic, and contains none of their code.
