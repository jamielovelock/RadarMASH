// RadarMASH - open firmware for the GeekMagic Radar 360.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The radar screen and the small amount of app behaviour around it (sweep, one-button selection, range).
// Portable: shared by the device firmware and the desktop simulator.
#pragma once
#include "canvas.h"
#include <stdint.h>

namespace rm {

// Which silhouette to draw. Chosen from what the data feed says about the aircraft.
enum class Shape : uint8_t { Unknown, Light, Narrow, Wide, Heli, Military };

constexpr int TRAIL_POINTS = 16;

struct Aircraft {
  char callsign[9] = "";
  char airline[26] = "";      // operator name, empty if unknown
  char type[5] = "";          // ICAO type, e.g. B77W
  char origin[5] = "";        // airport code, empty if unknown
  char dest[5] = "";
  Shape shape = Shape::Unknown;
  double lat = 0, lon = 0;
  int alt_ft = 0;
  float track_deg = 0;        // 0 = north, clockwise
  float speed_kt = 0;
  float trail_lat[TRAIL_POINTS], trail_lon[TRAIL_POINTS];   // past positions, newest first
  int trail_len = 0;
};

// Everything a user can switch on or off. The web settings page will read and write this.
struct Settings {
  bool sweep = true;          // rotating sweep and its trail
  bool rings = true;          // range rings and axes
  bool labels = true;         // callsign next to each aircraft
  bool altitude = true;       // flight level under the callsign
  bool trails = true;         // where each aircraft has been
  bool shapes = true;         // silhouettes by aircraft kind (off: plain arrowheads)
  bool tag = true;            // small box beside the highlighted aircraft; the tag_ switches choose what it shows
  bool tag_route = true;
  bool tag_type = true;
  bool tag_level = true;
  bool tag_speed = false;
  bool auto_cycle = true;     // highlight each aircraft in view in turn
  bool card = false;          // large detail card for the highlighted aircraft
};

constexpr int MAX_AIRCRAFT = 32;
constexpr float RADAR_RADIUS = 172.0f;    // outer range ring, in pixels

struct Radar {
  Settings set;
  double home_lat = 0, home_lon = 0;
  float range_km = 40;
  Aircraft ac[MAX_AIRCRAFT];
  int count = 0;
  int selected = -1;          // index into ac, or -1
  float sweep_deg = 0;        // current sweep bearing
  uint32_t now_ms = 0, last_cycle_ms = 0;

  void tick(uint32_t now_ms);                   // advance the sweep and the auto-cycle
  void shortPress();                            // highlight the next aircraft in view
  void longPress();                             // cycle range 20 / 40 / 80 km
  void draw(Canvas &c) const;                   // draw this canvas's band of the screen

  void toScreen(double lat, double lon, float &x, float &y) const;
  bool project(const Aircraft &a, float &x, float &y) const;   // false if outside the outer ring
};

// Moving made-up traffic around a made-up home, for the simulator and for a demo mode on the device.
void demoTraffic(Radar &r, uint32_t now_ms);

}  // namespace rm
