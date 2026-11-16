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

The device firmware will be added as its own folder and will build the same `core/` code.

## Running the simulator

You need a C++17 compiler and `make` (macOS or Linux).

    make -C sim run

Then open http://localhost:8360. "Short press" and "Long press" stand in for the device's single button.

To save one frame instead: `sim/radarmash-sim --shot frame.bmp --t 9000 --select 0`

## Licence

GPL-3.0-or-later; see `LICENSE`. `core/font5x7.h` comes from the Adafruit GFX Library and stays under its BSD licence,
reproduced in that file.

RadarMASH is an independent hobby project. It is not affiliated with GeekMagic, and contains none of their code.
