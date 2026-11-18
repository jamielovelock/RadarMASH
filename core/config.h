// RadarMASH - open firmware for the GeekMagic Radar 360.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Everything a user can set, and how it is written down. The settings page talks to the device (or the simulator) in
// this JSON, and the same text is what gets saved, so there is one format to keep compatible.
#pragma once
#include "feed.h"
#include "pages.h"
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
  char logo_url[128];                 // where airline logos come from, with {icao} in it; empty (the default) for none
  uint8_t brightness = 100;           // backlight, percent
  bool utc_auto = true;               // clock: take the time zone from the position (needs home_set); otherwise use utc_offset_min
  int16_t utc_offset_min = 0;         // clock: minutes ahead of UTC, when not automatic
  bool verify_certs = true;           // check the security certificates of https sources (turn off only for your own server)
  uint8_t night = 0;                  // dim at night: 0 never, 1 from sunset to sunrise (needs a position), 2 between the two times below
  uint8_t night_brightness = 15;      // percent
  int16_t night_start_min = 22 * 60, night_end_min = 7 * 60;   // local minutes after midnight
  Units units = Units::MetricKmh;     // for the weather page
  uint16_t page_cycle_s = 0;          // move to the next page every so many seconds; 0: only when asked
  float overhead_km = 3;              // how close an aircraft must pass to go in the overhead log
  char board_airport[5];              // the airport board's airport (as the route shows it, e.g. MAN); empty: the busiest seen
  char visitor_types[96];             // aircraft types that earn a notice, e.g. "A388,B744"; military always does
  char api_key[25];                   // if set, changing anything over the network needs this key. Never sent back out.

  Config();
  bool wantsLive() const { return live && home_set; }
};

constexpr char VISITOR_TYPES[] = "A388,B744,B748,A124,A225,C17,C5M,B52,U2,E3TF,E3CF,K35R,A400,EUFI,F35,SPIT,HURI,LANC,DC3,BLCF";

// Write the whole configuration as one JSON object. Returns its length, or 0 if `out` is too small.
// The control key is only written when `for_saving`; what goes to a web page says "api_key_set" instead.
size_t configToJson(const Config &c, char *out, size_t out_size, bool for_saving = false);

// Read JSON into `c`. Only the keys present are changed, so a page can send one setting at a time. Values out of range
// are clamped and nonsense is ignored. Returns false if the text is not a JSON object at all.
bool configFromJson(const char *text, size_t len, Config &c);

// A request to join a Wi-Fi network: {"ssid":"...","pass":"..."}. The name must be 1-32 characters; the password empty
// (an open network) or 8-63 characters, which is what WPA allows. Returns false, leaving the outputs empty, otherwise.
bool parseWifiRequest(const char *text, size_t len, char ssid[33], char pass[65]);

// Text made safe to put inside a JSON string (network names can contain anything).
size_t jsonEscape(const char *in, char *out, size_t out_size);

static_assert(sizeof VISITOR_TYPES <= sizeof Config().visitor_types, "the default list must fit");
constexpr size_t CONFIG_JSON_MAX = 2600;   // a buffer this big always fits configToJson

}  // namespace rm
