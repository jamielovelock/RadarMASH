// RadarMASH - open firmware for the GeekMagic Radar 360.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The radar screen and the small amount of app behaviour around it (sweep, one-button selection, range).
// Portable: shared by the device firmware and the desktop simulator.
#pragma once
#include "canvas.h"
#include <stdint.h>

namespace rm {

struct Aircraft {
  char callsign[9] = "";
  char type[5] = "";          // ICAO type, e.g. B77W
  char origin[5] = "";        // airport code, empty if unknown
  char dest[5] = "";
  double lat = 0, lon = 0;
  int alt_ft = 0;
  float track_deg = 0;        // 0 = north, clockwise
  float speed_kt = 0;
};

constexpr int MAX_AIRCRAFT = 32;
constexpr float RADAR_RADIUS = 172.0f;    // outer range ring, in pixels

struct Radar {
  double home_lat = 0, home_lon = 0;
  float range_km = 40;
  Aircraft ac[MAX_AIRCRAFT];
  int count = 0;
  int selected = -1;          // index into ac, or -1
  float sweep_deg = 0;        // current sweep bearing

  void tick(uint32_t now_ms);                   // advance the sweep
  void shortPress();                            // select next aircraft in view, then none
  void longPress();                             // cycle range 20 / 40 / 80 km
  void draw(Canvas &c) const;                   // draw this canvas's band of the screen

  bool project(const Aircraft &a, float &x, float &y) const;   // false if outside the outer ring
};

// Moving made-up traffic around a made-up home, for the simulator and for a demo mode on the device.
void demoTraffic(Radar &r, uint32_t now_ms);

}  // namespace rm
