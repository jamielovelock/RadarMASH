// RadarMASH - open firmware for the GeekMagic Radar 360.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The radar screen and the small amount of app behaviour around it (sweep, one-button selection, range).
// Portable: shared by the device firmware and the desktop simulator.
#pragma once
#include "canvas.h"
#include "map.h"
#include <stdint.h>

namespace rm {

// Which silhouette to draw. Chosen from what the data feed says about the aircraft.
enum class Shape : uint8_t { Unknown, Light, Narrow, Wide, Heli, Military };

constexpr int TRAIL_POINTS = 16;

struct Aircraft {
  char hex[7] = "";            // ICAO 24-bit address: what identifies an aircraft from one update to the next
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
  bool on_ground = false;
  // Live aircraft only: the last reported position and when it arrived. The drawn position runs on from it between updates.
  bool live = false;
  double fix_lat = 0, fix_lon = 0;
  uint32_t fix_ms = 0;
};

// Everything a user can switch on or off. The web settings page will read and write this.
struct Settings {
  bool sweep = true;          // rotating sweep and its trail
  bool rings = true;          // range rings and axes
  bool labels = true;         // callsign next to each aircraft
  bool altitude = true;       // flight level under the callsign
  bool trails = true;         // where each aircraft has been
  bool fade = true;           // aircraft flash bright as the sweep passes, then fade until it comes round again
  bool shapes = true;         // silhouettes by aircraft kind (off: plain arrowheads)
  bool tag = true;            // small box beside the highlighted aircraft; the tag_ switches choose what it shows
  bool tag_route = true;
  bool tag_type = true;
  bool tag_level = true;
  bool tag_speed = false;
  bool auto_cycle = true;     // highlight each aircraft in view in turn
  bool card = false;          // large detail card for the highlighted aircraft
  bool map = true;            // map underlay, when one has been loaded
  MapStyle map_style = MapStyle::Dark;
  uint8_t map_brightness = 55;                // percent
  uint8_t max_aircraft = 10;                  // show at most this many, nearest home first (up to MAX_AIRCRAFT)
};

// The on/off switches by name, for settings pages. The simulator and the device both serve these.
struct Toggle { const char *name, *label; bool Settings::*field; };
extern const Toggle TOGGLES[];
extern const int TOGGLE_COUNT;
bool *toggleByName(Settings &s, const char *name);      // nullptr if there is no such switch

// Optional speed-up, costing about 520 KB of memory. The parts of the scope that do not move (rings, axes, map) and the
// bearing of every pixel are worked out once and kept, so each frame only has to add the sweep. Without it every frame
// redoes the lot, which is correct but several times slower on the device.
struct ScopeCache {
  uint16_t bg[SCREEN_W * SCREEN_H];        // the still background, ready to copy
  uint16_t bearing[SCREEN_W * SCREEN_H];   // 0..65535 for 0..360 degrees clockwise from up
  uint32_t key = 0;                        // what bg was drawn for
  bool valid = false, bearings_done = false;
};

constexpr int MAX_AIRCRAFT = 64;
constexpr float RADAR_RADIUS = 172.0f;    // outer range ring, in pixels
constexpr float RANGES_KM[] = {10, 20, 40, 80, 160};   // the zoom levels a long press steps through

struct Radar {
  Settings set;
  double home_lat = 0, home_lon = 0;
  float range_km = 40;
  Aircraft ac[MAX_AIRCRAFT];
  int count = 0;
  int selected = -1;          // index into ac, or -1
  const MapImage *map = nullptr;                // underlay to draw, owned by the caller
  ScopeCache *cache = nullptr;                  // optional, owned by the caller; see ScopeCache
  char map_credit[28] = "";                     // attribution the map source asks for, shown at the bottom
  float sweep_deg = 0;        // current sweep bearing
  uint32_t now_ms = 0, last_cycle_ms = 0;

  void tick(uint32_t now_ms);                   // advance the sweep, the auto-cycle and live aircraft between updates
  // Take a fresh list from the feed (it is modified). Trails, routes and the highlight carry over by hex address.
  void update(Aircraft *fresh, int n, uint32_t now_ms);
  void shortPress();                            // highlight the next aircraft in view
  void longPress();                             // step to the next range (zoom level)
  void prepare();                               // once per frame before draw(): brings the cache up to date, if there is one
  void draw(Canvas &c) const;                   // draw this canvas's band of the screen

  void toScreen(double lat, double lon, float &x, float &y) const;
  bool project(const Aircraft &a, float &x, float &y) const;   // false if outside the outer ring

private:
  // kilometres per degree of longitude at home, remembered so the cosine is not recomputed for every point drawn
  mutable double kx_for_lat_ = 1e9;
  mutable float kx_ = 0;
};

// Moving made-up traffic around wherever home is, for the simulator and for a demo mode on the device.
void demoTraffic(Radar &r, uint32_t now_ms);

}  // namespace rm
