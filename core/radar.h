// RadarMASH - open firmware for the GeekMagic Radar 360.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The radar screen and the small amount of app behaviour around it (sweep, one-button selection, range).
// Portable: shared by the device firmware and the desktop simulator.
#pragma once
#include "canvas.h"
#include "font.h"
#include "logo.h"
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
  char airline_icao[4] = "";  // operator code, e.g. RYR; from the callsign or the route lookup
  char type[5] = "";          // ICAO type, e.g. B77W
  char origin[5] = "";        // airport code, empty if unknown
  char dest[5] = "";
  char squawk[5] = "";        // transponder code as sent, e.g. 7700; empty if not reported
  Shape shape = Shape::Unknown;
  double lat = 0, lon = 0;
  int alt_ft = 0;
  float track_deg = 0;        // 0 = north, clockwise
  float speed_kt = 0;
  int16_t climb_fpm = 0;      // feet per minute, negative when descending; 0 if not reported
  float trail_lat[TRAIL_POINTS], trail_lon[TRAIL_POINTS];   // past positions, newest first
  int trail_len = 0;
  bool on_ground = false;
  // Live aircraft only: the last reported position and when it arrived. The drawn position runs on from it between updates.
  bool live = false;
  double fix_lat = 0, fix_lon = 0;
  uint32_t fix_ms = 0;
  // Where this aircraft's label was last put (worked out when drawing; kept so labels do not hop about).
  mutable uint8_t label_slot = 0;
  mutable int16_t label_x = 0, label_y = 0;
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
  bool declutter = true;      // move labels so they do not sit on each other, on aircraft or on the tag box
  bool logos = true;          // airline logo in the tag box, when a logo source has been set
  bool map = true;            // map underlay, when one has been loaded
  bool clock = true;          // the time of day at the bottom of the scope, once the time is known
  bool alerts = true;         // watch the whole world for an aircraft in distress (squawking 7700) and offer to go and look
  MapStyle map_style = MapStyle::Dark;
  uint8_t map_brightness = 55;                // percent
  uint8_t max_aircraft = 10;                  // show at most this many, nearest home first (up to MAX_AIRCRAFT)
  uint8_t font = 0;                           // index into FONTS
  uint8_t map_detail = 0;                     // extra map zoom levels: 0 normal, 1 sharper (smaller labels, more to download)
  uint8_t sweep_seconds = 4;                  // time for the sweep to go once round
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
  LogoSource *logos = nullptr;                  // optional, owned by the caller; see logo.h
  char map_credit[28] = "";                     // attribution the map source asks for, shown at the bottom
  int16_t clock_min = -1;                       // minutes since midnight to show as the time, or -1 if not known; set by the caller
  bool clock_utc = false;                       // the time is UTC, not local: shown with a Z
  char alert[24] = "", alert2[40] = "";         // two lines of flashing notice at the top; empty for none (see alert.h)
  char pin_hex[7] = "";                         // when set, the highlight stays on this aircraft instead of cycling
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
  void layoutLabels() const;                    // decides where every label and the tag box go, once per frame
  mutable uint32_t layout_key_ = 0;
  mutable int16_t tag_x_ = 0, tag_y_ = 0, tag_w_ = 0, tag_h_ = 0;
  mutable const Logo *tag_logo_ = nullptr;
  // kilometres per degree of longitude at home, remembered so the cosine is not recomputed for every point drawn
  mutable double kx_for_lat_ = 1e9;
  mutable float kx_ = 0;
};

// Moving made-up traffic around wherever home is, for the simulator and for a demo mode on the device.
void demoTraffic(Radar &r, uint32_t now_ms);

}  // namespace rm
