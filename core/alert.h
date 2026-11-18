// RadarMASH - open firmware for the GeekMagic Radar 360.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Aircraft in distress, and the clock.
//
// A transponder set to 7700 is the pilot telling everyone there is an emergency on board; 7600 means the radio has
// failed and 7500 unlawful interference. (7000 is nothing of the kind: it is the ordinary "flying visually, not talking
// to anyone" code in Europe.) Any of the three seen on the scope is drawn in red. Separately, the radar can keep watch
// on the whole world for a 7700 and offer to go and look: one press moves the scope to that aircraft, wherever it is,
// and another press comes home.
#pragma once
#include "radar.h"
#include <stddef.h>

namespace rm {

// The whole world's aircraft on one code, in the same format as the position feeds.
constexpr const char *WATCH_ADSB_FI = "https://opendata.adsb.fi/api/v2/sqk/7700";
constexpr const char *WATCH_ADSB_LOL = "https://api.adsb.lol/v2/sqk/7700";
constexpr int WATCH_EVERY_S = 60;                   // how often to ask: emergencies last many minutes

bool isEmergencySquawk(const char *squawk);         // 7500, 7600 or 7700

// The emergency watch and the "go and look" view. All of it works on the Radar it is given, under the caller's lock.
struct Watch {
  bool have = false;                  // an emergency is known somewhere
  Aircraft target;                    // it, as last reported
  char dismissed[7] = "";             // the one the user has already been to see: it does not ask again
  bool away = false;                  // the scope is over there, not at home
  uint32_t away_since = 0;
  double back_lat = 0, back_lon = 0;  // where home is, for coming back
  float back_range = 40;

  // The world reply, already parsed (nearest home first is best; the first airborne one is used). n = 0 means all clear.
  void found(const Aircraft *list, int n);
  // The button. Returns true if the press was used up here (going to look, or coming home).
  bool press(Radar &r, uint32_t now_ms);
  // Once a frame or so: follows the aircraft while away, comes home after ten minutes or when it is all over, and
  // keeps the notice at the top of the scope up to date. `enabled` is the user's setting.
  void tick(Radar &r, uint32_t now_ms, bool enabled);
  void comeHome(Radar &r);

  bool alerting() const;              // is there something new to offer?
};

// Great-circle distance in km.
double distanceKm(double lat1, double lon1, double lat2, double lon2);

// ---- the clock ----
// Where the time zone comes from when it is left on automatic: a free service that knows the zone (and summer time) for
// a position. The position is sent rounded to about a kilometre.
constexpr const char *ZONE_OPEN_METEO = "https://api.open-meteo.com/v1/forecast?latitude={lat}&longitude={lon}&timezone=auto";
// Reads "utc_offset_seconds" from its reply, as minutes. False if it is not there or not believable.
bool parseUtcOffset(const char *text, size_t len, int &minutes);
// Minutes since midnight for a time in seconds since 1970 (UTC) and an offset in minutes. -1 if the time is plainly
// not set (a device that has not heard from a time server yet thinks it is 1970).
int clockMinutes(long long unix_utc, int offset_min);

}  // namespace rm
