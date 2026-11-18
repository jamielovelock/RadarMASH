// RadarMASH - open firmware for the GeekMagic Radar 360.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The screens other than the radar scope, and what feeds them:
//   weather   the conditions now and the next three days, with the wind drawn round the rim
//   board     aircraft seen going to or from one airport
//   log       "what was that?": the aircraft that lately passed close overhead
// plus the notice for an unusual visitor, and night dimming. All portable, like the rest of core.
#pragma once
#include "radar.h"
#include <stddef.h>

namespace rm {

enum class Page : uint8_t { Radar, Weather, Board, Log, Count };
const char *pageName(Page p);                         // "radar", "weather", "board", "log"
bool pageByName(const char *name, Page &out);
Page nextPage(const Settings &s, Page current);       // the next page that is switched on; the radar always is

// ---- weather ----
// One request gives the conditions, three days, sunrise and sunset, and the time zone. The position is sent rounded
// to about a kilometre. Free, no key.
constexpr const char *WEATHER_OPEN_METEO =
    "https://api.open-meteo.com/v1/forecast?latitude={lat}&longitude={lon}&timezone=auto"
    "&current=temperature_2m,apparent_temperature,relative_humidity_2m,precipitation,weather_code,cloud_cover,"
    "wind_speed_10m,wind_direction_10m,wind_gusts_10m,pressure_msl,is_day"
    "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset&forecast_days=3";
constexpr int WEATHER_EVERY_S = 900;                  // the service updates every quarter of an hour

struct WeatherDay {
  int8_t weekday = -1;        // 0 = Sunday; -1 if not known
  uint8_t code = 0;           // WMO weather code
  int8_t hi_c = 0, lo_c = 0;
  int8_t rain_pct = -1;       // chance of rain or snow; -1 if not given
};
struct Weather {
  bool have = false;
  float temp_c = 0, feels_c = 0, wind_kmh = 0, gust_kmh = 0, rain_mm = 0, pressure_hpa = 0;
  int16_t wind_deg = 0;       // where the wind comes FROM, degrees clockwise from north
  uint8_t humidity = 0, cloud = 0, code = 0;
  bool is_day = true;
  int16_t sunrise_min = -1, sunset_min = -1;   // today's, minutes after local midnight; -1 if not given
  int16_t utc_offset_min = 0;
  WeatherDay day[3];
  int days = 0;
};
// False (leaving `out` alone) unless the reply has at least a temperature and a weather code that make sense.
bool parseWeather(const char *text, size_t len, Weather &out);
const char *weatherText(int code);                    // "Light rain"; "" for a code not known

enum class Units : uint8_t { MetricKmh, MetricMph, MetricKnots, Imperial, Count };   // C km/h, C mph, C knots, F mph

// Night dimming. mode 0: never. 1: between sunset and sunrise (needs the weather). 2: between two set times.
// clock_min is local minutes after midnight, or -1 if the time is not known (then it is not night).
bool isNight(int mode, int clock_min, const Weather &wx, int start_min, int end_min);

// ---- aircraft seen lately ----
struct Sighting {
  char hex[7] = "", callsign[9] = "", type[5] = "", origin[5] = "", dest[5] = "";
  int alt_ft = 0;             // at its closest
  float min_km = 0;           // the closest it came to home
  long long last = 0, closest_at = 0;   // unix seconds; 0 if the time was not known
  uint32_t last_ms = 0;
};
constexpr int SIGHTINGS = 24;
struct Sightings {
  Sighting e[SIGHTINGS];
  int n = 0;
  // After each update from the feed (not while the scope is away from home). The oldest entry makes way for a new one.
  void note(const Radar &r, long long unix_now, uint32_t now_ms);
};

// Everything the pages draw from. Owned by the caller, who points Radar::pages at it.
struct PageData {
  Weather wx;
  bool wx_wanted = false;     // false: there is no position to ask the weather for
  Sightings seen;
  char airport[5] = "";       // the board's airport; empty: whichever is seen most
  Units units = Units::MetricKmh;
  float overhead_km = 3;      // how close counts as "overhead" for the log
  int utc_offset_min = 0;     // for showing when things were seen
};
void boardAirport(const PageData &d, char out[5]);    // the airport the board is showing; "" if none yet
void drawPage(Canvas &c, const Radar &r, const PageData &d);   // r.page says which; the radar page is not drawn here

// ---- unusual visitors ----
// Military aircraft, and any type on the user's list, get a notice at the top of the scope when they come into view.
bool typeListed(const char *list, const char *type);  // list is "A388,B744,C17"; whole codes only
struct Visitors {
  char seen[16][7] = {};
  uint32_t seen_ms[16] = {};
  int next = 0;
  uint32_t until = 0;                 // a notice is up until then; 0 for none
  char line1[24] = "", line2[40] = "";
  bool forced = false;                // the notice is the radar's own (see say), shown even with visitor notices off
  // After each update from the feed (not while away). Highlights the newcomer and starts its notice.
  void check(Radar &r, const char *types, bool helicopters, uint32_t now_ms);
  // Once a frame, after Watch::tick: puts the notice up unless an emergency already has the space.
  void tick(Radar &r, uint32_t now_ms, bool enabled);
  // The same notice space, for the radar to say something of its own for a few seconds.
  void say(const char *first, const char *second, uint32_t now_ms, uint32_t for_ms);
};

}  // namespace rm
