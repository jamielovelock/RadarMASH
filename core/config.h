// RadarMASH - open firmware for the GeekMagic Radar 360.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Everything a user can set, and how it is written down. The settings page talks to the device (or the simulator) in
// this JSON, and the same text is what gets saved, so there is one format to keep compatible.
#pragma once
#include "feed.h"
#include "radar.h"
#include <stddef.h>

namespace rm {

constexpr const char *MAP_OSM = "https://tile.openstreetmap.org/{z}/{x}/{y}.png";

struct Config {
  Settings set;                       // the on/off switches and the other display choices
  bool home_set = false;              // has the user said where the radar is?
  double home_lat = 51.0, home_lon = -1.0;   // until then: nowhere in particular, for the demo
  float range_km = 40;
  bool live = false;                  // real aircraft (needs home_set); otherwise demo traffic
  char feed_url[128];
  uint16_t refresh_s = 5;
  bool include_ground = false;
  char map_url[128];
  char map_credit[28];
  uint8_t brightness = 100;           // backlight, percent

  Config();
  bool wantsLive() const { return live && home_set; }
};

// Write the whole configuration as one JSON object. Returns its length, or 0 if `out` is too small.
size_t configToJson(const Config &c, char *out, size_t out_size);

// Read JSON into `c`. Only the keys present are changed, so a page can send one setting at a time. Values out of range
// are clamped and nonsense is ignored. Returns false if the text is not a JSON object at all.
bool configFromJson(const char *text, size_t len, Config &c);

constexpr size_t CONFIG_JSON_MAX = 1440;   // a buffer this big always fits configToJson

}  // namespace rm
