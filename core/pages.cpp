// RadarMASH - open firmware for the GeekMagic Radar 360.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pages.h"
#include "alert.h"
#include "json.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace rm {

static const float PI_F = 3.14159265f;
static const Color COL_RING = rgb(0, 120, 60), COL_DIM = rgb(0, 150, 80), COL_TEXT = rgb(230, 230, 230), COL_HI = rgb(255, 240, 80);
static const Color COL_BLUE = rgb(120, 220, 255), COL_AMBER = rgb(255, 190, 60), COL_GREY = rgb(150, 150, 150), COL_CLOUD = rgb(190, 196, 204);

// ---- which page ----
static const char *const PAGE_NAMES[] = {"radar", "weather", "board", "log"};
const char *pageName(Page p) { return (int)p < (int)Page::Count ? PAGE_NAMES[(int)p] : "radar"; }
bool pageByName(const char *name, Page &out) {
  for (int i = 0; i < (int)Page::Count; i++)
    if (strcmp(name, PAGE_NAMES[i]) == 0) { out = (Page)i; return true; }
  return false;
}
static bool pageOn(const Settings &s, Page p) {
  return p == Page::Radar || (p == Page::Weather && s.page_weather) || (p == Page::Board && s.page_board) || (p == Page::Log && s.page_log);
}
Page nextPage(const Settings &s, Page cur) {
  for (int step = 1; step <= (int)Page::Count; step++) {
    Page p = (Page)(((int)cur + step) % (int)Page::Count);
    if (pageOn(s, p)) return p;
  }
  return Page::Radar;
}

// ---- weather ----
static bool num(json::Span obj, const char *key, double &out) {
  json::Span v;
  return json::member(obj, key, v) && json::number(v, out) && isfinite(out);
}
static bool nth(json::Span obj, const char *key, int index, json::Span &out) {       // obj[key][index]
  json::Span arr;
  if (!json::member(obj, key, arr) || !json::isArray(arr)) return false;
  const char *it = nullptr;
  for (int i = 0; json::element(arr, it, out); i++)
    if (i == index) return true;
  return false;
}
static bool nthNum(json::Span obj, const char *key, int index, double &out) {
  json::Span v;
  return nth(obj, key, index, v) && json::number(v, out) && isfinite(out);
}
static inline bool digit(char ch) { return ch >= '0' && ch <= '9'; }
static int clockOf(json::Span v) {                     // "2026-11-18T07:25" -> 445
  char s[24];
  if (!json::string(v, s, sizeof s)) return -1;
  const char *t = strchr(s, 'T');
  if (!t || strlen(t) < 6 || !digit(t[1]) || !digit(t[2]) || t[3] != ':' || !digit(t[4]) || !digit(t[5])) return -1;
  int h = (t[1] - '0') * 10 + (t[2] - '0'), m = (t[4] - '0') * 10 + (t[5] - '0');
  return h < 24 && m < 60 ? h * 60 + m : -1;
}
static int weekdayOf(json::Span v) {                   // "2026-11-18" -> 3 (Wednesday)
  char s[16];
  if (!json::string(v, s, sizeof s) || strlen(s) < 10 || s[4] != '-' || s[7] != '-') return -1;
  for (int i = 0; i < 10; i++) if (i != 4 && i != 7 && !digit(s[i])) return -1;
  int y = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
  int m = (s[5] - '0') * 10 + (s[6] - '0'), d = (s[8] - '0') * 10 + (s[9] - '0');
  if (m < 1 || m > 12 || d < 1 || d > 31) return -1;
  static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y--;
  return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}
static inline double clampD(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

bool parseWeather(const char *text, size_t len, Weather &out) {
  json::Span root = json::parse(text, len), cur, daily;
  double t, code, d;
  if (!json::isObject(root) || !json::member(root, "current", cur) || !json::isObject(cur)) return false;
  if (!num(cur, "temperature_2m", t) || !num(cur, "weather_code", code) || t < -90 || t > 70 || code < 0 || code > 99) return false;
  Weather w;
  w.have = true;
  w.temp_c = (float)t; w.code = (uint8_t)code;
  w.feels_c = num(cur, "apparent_temperature", d) ? (float)clampD(d, -99, 99) : w.temp_c;
  if (num(cur, "relative_humidity_2m", d)) w.humidity = (uint8_t)clampD(d, 0, 100);
  if (num(cur, "cloud_cover", d)) w.cloud = (uint8_t)clampD(d, 0, 100);
  if (num(cur, "precipitation", d)) w.rain_mm = (float)clampD(d, 0, 999);
  if (num(cur, "wind_speed_10m", d)) w.wind_kmh = (float)clampD(d, 0, 500);
  if (num(cur, "wind_gusts_10m", d)) w.gust_kmh = (float)clampD(d, 0, 500);
  if (num(cur, "wind_direction_10m", d)) w.wind_deg = (int16_t)(((int)clampD(d, 0, 360)) % 360);
  if (num(cur, "pressure_msl", d)) w.pressure_hpa = (float)clampD(d, 0, 1200);
  if (num(cur, "is_day", d)) w.is_day = d != 0;
  if (num(root, "utc_offset_seconds", d) && d >= -12 * 3600 && d <= 14 * 3600) w.utc_offset_min = (int16_t)lround(d / 60.0);
  if (json::member(root, "daily", daily) && json::isObject(daily)) {
    json::Span v;
    if (nth(daily, "sunrise", 0, v)) w.sunrise_min = (int16_t)clockOf(v);
    if (nth(daily, "sunset", 0, v)) w.sunset_min = (int16_t)clockOf(v);
    for (int i = 0; i < 3; i++) {
      double hi, lo, c;
      if (!nthNum(daily, "temperature_2m_max", i, hi) || !nthNum(daily, "temperature_2m_min", i, lo) || !nthNum(daily, "weather_code", i, c)) break;
      WeatherDay &day = w.day[i];
      day.hi_c = (int8_t)lround(clampD(hi, -99, 99)); day.lo_c = (int8_t)lround(clampD(lo, -99, 99)); day.code = (uint8_t)clampD(c, 0, 99);
      if (nthNum(daily, "precipitation_probability_max", i, d)) day.rain_pct = (int8_t)clampD(d, 0, 100);
      if (nth(daily, "time", i, v)) day.weekday = (int8_t)weekdayOf(v);
      w.days = i + 1;
    }
  }
  out = w;
  return true;
}

const char *weatherText(int code) {
  switch (code) {
    case 0: return "Clear";
    case 1: return "Mainly clear";
    case 2: return "Partly cloudy";
    case 3: return "Overcast";
    case 45: case 48: return "Fog";
    case 51: case 53: case 55: return "Drizzle";
    case 56: case 57: return "Freezing drizzle";
    case 61: return "Light rain";
    case 63: return "Rain";
    case 65: return "Heavy rain";
    case 66: case 67: return "Freezing rain";
    case 71: return "Light snow";
    case 73: return "Snow";
    case 75: return "Heavy snow";
    case 77: return "Snow grains";
    case 80: return "Light showers";
    case 81: return "Showers";
    case 82: return "Heavy showers";
    case 85: case 86: return "Snow showers";
    case 95: return "Thunderstorm";
    case 96: case 99: return "Thunder and hail";
    default: return "";
  }
}

bool isNight(int mode, int clock_min, const Weather &wx, int start_min, int end_min) {
  if (clock_min < 0) return false;
  if (mode == 1) {
    if (!wx.have || wx.sunrise_min < 0 || wx.sunset_min < 0) return false;
    return clock_min < wx.sunrise_min || clock_min >= wx.sunset_min;
  }
  if (mode == 2) {
    if (start_min == end_min) return false;
    return start_min < end_min ? (clock_min >= start_min && clock_min < end_min) : (clock_min >= start_min || clock_min < end_min);
  }
  return false;
}

// ---- weather pictures, from circles and lines ----
enum class Sky : uint8_t { Sun, Moon, PartSun, PartMoon, Cloud, Fog, Rain, Snow, Storm };
static Sky skyFor(int code, bool day) {
  if (code <= 1) return day ? Sky::Sun : Sky::Moon;
  if (code == 2) return day ? Sky::PartSun : Sky::PartMoon;
  if (code == 3) return Sky::Cloud;
  if (code == 45 || code == 48) return Sky::Fog;
  if ((code >= 71 && code <= 77) || code == 85 || code == 86) return Sky::Snow;
  if (code >= 95) return Sky::Storm;
  return Sky::Rain;
}
static void drawSun(Canvas &c, float x, float y, float s) {
  c.disc(x, y, 9 * s, COL_HI);
  for (int i = 0; i < 8; i++) {
    float a = i * PI_F / 4, dx = sinf(a), dy = cosf(a);
    c.line(x + dx * 13 * s, y + dy * 13 * s, x + dx * 18 * s, y + dy * 18 * s, COL_HI);
  }
}
static void drawMoon(Canvas &c, float x, float y, float s) {
  c.disc(x, y, 11 * s, rgb(225, 225, 200));
  c.disc(x + 6 * s, y - 4 * s, 10 * s, rgb(0, 0, 0));
}
static void drawCloud(Canvas &c, float x, float y, float s, Color col) {
  c.disc(x - 10 * s, y + 4 * s, 8 * s, col); c.disc(x + 1 * s, y - 3 * s, 11 * s, col); c.disc(x + 12 * s, y + 5 * s, 7 * s, col);
  c.rect((int)(x - 10 * s), (int)(y + 4 * s), (int)(22 * s), (int)(8 * s) + 1, col);
}
static void drawSky(Canvas &c, Sky k, float x, float y, float s) {
  switch (k) {
    case Sky::Sun: drawSun(c, x, y, s); break;
    case Sky::Moon: drawMoon(c, x, y, s); break;
    case Sky::PartSun: drawSun(c, x - 8 * s, y - 7 * s, s * 0.75f); drawCloud(c, x + 3 * s, y + 4 * s, s, COL_CLOUD); break;
    case Sky::PartMoon: drawMoon(c, x - 8 * s, y - 7 * s, s * 0.75f); drawCloud(c, x + 3 * s, y + 4 * s, s, COL_CLOUD); break;
    case Sky::Cloud: drawCloud(c, x, y, s, COL_CLOUD); break;
    case Sky::Fog:
      for (int i = 0; i < 4; i++) { float w = (i % 2 ? 16 : 20) * s, yy = y - 9 * s + i * 6 * s; c.line(x - w, yy, x + w, yy, COL_GREY); }
      break;
    case Sky::Rain:
      drawCloud(c, x, y - 5 * s, s, COL_CLOUD);
      for (int i = -1; i <= 1; i++) c.line(x + i * 9 * s + 2 * s, y + 10 * s, x + i * 9 * s - 2 * s, y + 19 * s, COL_BLUE);
      break;
    case Sky::Snow:
      drawCloud(c, x, y - 5 * s, s, COL_CLOUD);
      for (int i = -1; i <= 1; i++) c.disc(x + i * 9 * s, y + (i ? 13 : 17) * s, 1.8f * s + 0.4f, rgb(255, 255, 255));
      break;
    case Sky::Storm:
      drawCloud(c, x, y - 5 * s, s, COL_GREY);
      c.line(x + 3 * s, y + 8 * s, x - 3 * s, y + 14 * s, COL_HI); c.line(x - 3 * s, y + 14 * s, x + 3 * s, y + 14 * s, COL_HI);
      c.line(x + 3 * s, y + 14 * s, x - 2 * s, y + 21 * s, COL_HI);
      break;
  }
}

static int showTemp(float c, Units u) { return (int)lroundf(u == Units::Imperial ? c * 9.0f / 5.0f + 32.0f : c); }
static int showWind(float kmh, Units u) {
  return (int)lroundf(u == Units::MetricKmh ? kmh : u == Units::MetricKnots ? kmh * 0.539957f : kmh * 0.621371f);
}
static const char *windUnit(Units u) { return u == Units::MetricKmh ? "km/h" : u == Units::MetricKnots ? "kt" : "mph"; }
static const char *compass(float deg) {
  static const char *const N[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  int i = (int)floorf((deg + 22.5f) / 45.0f);
  return N[((i % 8) + 8) % 8];
}

static void pageFrame(Canvas &c) {
  c.fill(rgb(0, 0, 0));
  c.ring(CENTRE, CENTRE, RADAR_RADIUS, COL_RING);
}

static void drawWeather(Canvas &c, const PageData &d) {
  pageFrame(c);
  const Weather &w = d.wx;
  if (!w.have) {
    c.textCentred(180, 140, "Weather", 3, COL_TEXT);
    c.textCentred(180, 184, d.wx_wanted ? "waiting for the forecast" : "set your position first", 1, COL_DIM);
    return;
  }
  // the wind, as an arrow at the rim flying in from where it blows
  static const char *const NESW[] = {"N", "E", "S", "W"};
  for (int i = 0; i < 4; i++) {
    float a = i * PI_F / 2, r = RADAR_RADIUS - 12;
    c.textCentred((int)(CENTRE + sinf(a) * r), (int)(CENTRE - cosf(a) * r) - lineHeight(1) / 2, NESW[i], 1, COL_DIM);
  }
  if (w.wind_kmh >= 1) {
    float a = w.wind_deg * PI_F / 180.0f, sx = sinf(a), sy = -cosf(a), px = -sy, py = sx;
    float tip = RADAR_RADIUS - 44, base = RADAR_RADIUS - 22, tail = RADAR_RADIUS - 6;
    c.triangle(CENTRE + sx * tip, CENTRE + sy * tip, CENTRE + sx * base + px * 9, CENTRE + sy * base + py * 9,
               CENTRE + sx * base - px * 9, CENTRE + sy * base - py * 9, COL_HI);
    c.line(CENTRE + sx * base, CENTRE + sy * base, CENTRE + sx * tail, CENTRE + sy * tail, COL_HI);
  }
  char line[64];
  c.textCentred(180, 52, weatherText(w.code), 2, COL_TEXT);
  // the picture and the temperature, side by side and centred as a pair
  snprintf(line, sizeof line, "%d\xF8", showTemp(w.temp_c, d.units));
  const int tw = Canvas::textWidth(line, 3), x0 = 180 - (44 + 12 + tw) / 2;
  drawSky(c, skyFor(w.code, w.is_day), (float)(x0 + 22), 104, 1.0f);
  c.text(x0 + 56, 84, line, 3, COL_TEXT);
  snprintf(line, sizeof line, "feels %d\xF8", showTemp(w.feels_c, d.units));
  c.text(x0 + 56, 86 + lineHeight(3), line, 1, COL_GREY);

  snprintf(line, sizeof line, "%s %d %s", compass(w.wind_deg), showWind(w.wind_kmh, d.units), windUnit(d.units));
  c.textCentred(180, 142, line, 2, COL_HI);
  if (w.gust_kmh > w.wind_kmh + 8) {
    snprintf(line, sizeof line, "gusts %d", showWind(w.gust_kmh, d.units));
    c.textCentred(180, 144 + lineHeight(2), line, 1, COL_AMBER);
  }
  snprintf(line, sizeof line, "Cloud %d%%   Humidity %d%%", (int)w.cloud, (int)w.humidity);
  c.textCentred(180, 180, line, 1, COL_TEXT);
  snprintf(line, sizeof line, "Rain %.1f mm   %d hPa", (double)w.rain_mm, (int)lroundf(w.pressure_hpa));
  c.textCentred(180, 195, line, 1, COL_TEXT);
  if (w.sunrise_min >= 0 && w.sunset_min >= 0) {
    snprintf(line, sizeof line, "Sunrise %02d:%02d   Sunset %02d:%02d", w.sunrise_min / 60, w.sunrise_min % 60, w.sunset_min / 60, w.sunset_min % 60);
    c.textCentred(180, 210, line, 1, COL_BLUE);
  }
  c.blendRect(84, 229, 192, 1, COL_RING, 255);
  static const char *const DAYS[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  for (int i = 0; i < w.days; i++) {
    const WeatherDay &day = w.day[i];
    const int x = 180 + (i - 1) * 66 + (w.days == 2 ? 33 : 0);
    c.textCentred(x, 236, i == 0 ? "Today" : day.weekday >= 0 ? DAYS[day.weekday] : "", 1, COL_DIM);
    drawSky(c, skyFor(day.code, true), (float)x, 262, 0.5f);
    snprintf(line, sizeof line, "%d / %d\xF8", showTemp(day.hi_c, d.units), showTemp(day.lo_c, d.units));
    c.textCentred(x, 277, line, 1, COL_TEXT);
    if (day.rain_pct >= 0) { snprintf(line, sizeof line, "%d%%", (int)day.rain_pct); c.textCentred(x, 290, line, 1, COL_BLUE); }
  }
}

// ---- aircraft seen lately ----
void Sightings::note(const Radar &r, long long unix_now, uint32_t now_ms) {
  for (int i = 0; i < r.count; i++) {
    const Aircraft &a = r.ac[i];
    if (!a.hex[0] || a.on_ground) continue;
    const float km = (float)distanceKm(r.home_lat, r.home_lon, a.lat, a.lon);
    Sighting *s = nullptr;
    for (int k = 0; k < n; k++)
      if (strcmp(e[k].hex, a.hex) == 0 && now_ms - e[k].last_ms < 30UL * 60UL * 1000UL) { s = &e[k]; break; }   // back after half an hour is a new visit
    if (!s) {
      if (n < SIGHTINGS) s = &e[n++];
      else { s = &e[0]; for (int k = 1; k < n; k++) if ((int32_t)(e[k].last_ms - s->last_ms) < 0) s = &e[k]; }
      *s = Sighting();
      memcpy(s->hex, a.hex, sizeof s->hex);
      s->min_km = 1e9f;
    }
    s->last = unix_now; s->last_ms = now_ms;
    if (a.callsign[0]) memcpy(s->callsign, a.callsign, sizeof s->callsign);
    if (a.type[0]) memcpy(s->type, a.type, sizeof s->type);
    if (a.origin[0] && a.dest[0]) { memcpy(s->origin, a.origin, sizeof s->origin); memcpy(s->dest, a.dest, sizeof s->dest); }
    if (km < s->min_km) { s->min_km = km; s->alt_ft = a.alt_ft; s->closest_at = unix_now; }
  }
}

void boardAirport(const PageData &d, char out[5]) {
  out[0] = 0;
  if (d.airport[0]) { snprintf(out, 5, "%s", d.airport); return; }
  int best = 0;                                         // nothing chosen: the airport most of the traffic uses
  const Sightings &s = d.seen;
  for (int i = 0; i < s.n * 2; i++) {
    const char *code = i % 2 ? s.e[i / 2].dest : s.e[i / 2].origin;
    if (!code[0]) continue;
    int count = 0;
    for (int k = 0; k < s.n; k++) count += (strcmp(s.e[k].origin, code) == 0) + (strcmp(s.e[k].dest, code) == 0);
    if (count > best) { best = count; snprintf(out, 5, "%s", code); }
  }
}

static void timeText(long long unix_s, int offset_min, char out[8]) {
  int m = clockMinutes(unix_s, offset_min);
  if (m < 0) snprintf(out, 8, "--:--"); else snprintf(out, 8, "%02d:%02d", m / 60 % 24, m % 60);
}
// The entries that pass `want`, newest first by `when`. Returns how many, up to max.
template <typename Want, typename When>
static int newest(const Sightings &s, const Sighting **out, int max, Want want, When when) {
  int n = 0;
  bool used[SIGHTINGS] = {};
  while (n < max) {
    int best = -1;
    for (int i = 0; i < s.n; i++)
      if (!used[i] && want(s.e[i]) && (best < 0 || when(s.e[i]) > when(s.e[best]))) best = i;
    if (best < 0) break;
    used[best] = true; out[n++] = &s.e[best];
  }
  return n;
}

static void drawBoard(Canvas &c, const PageData &d) {
  pageFrame(c);
  char ap[5], line[64], hm[8];
  boardAirport(d, ap);
  if (!ap[0]) {
    c.textCentred(180, 140, "Airport board", 2, COL_TEXT);
    c.textCentred(180, 172, "no flights with a known route yet", 1, COL_DIM);
    return;
  }
  snprintf(line, sizeof line, "%s", ap);
  c.textCentred(180, 40, line, 3, COL_HI);
  c.textCentred(180, 42 + lineHeight(3), "arrivals and departures seen", 1, COL_DIM);
  const Sighting *rows[7];
  int n = newest(d.seen, rows, 7, [&](const Sighting &s) { return strcmp(s.origin, ap) == 0 || strcmp(s.dest, ap) == 0; },
                 [](const Sighting &s) { return (int32_t)s.last_ms; });
  if (!n) c.textCentred(180, 160, "none lately", 1, COL_DIM);
  for (int i = 0; i < n; i++) {
    const Sighting &s = *rows[i];
    const bool arriving = strcmp(s.dest, ap) == 0;
    timeText(s.last, d.utc_offset_min, hm);
    snprintf(line, sizeof line, "%s %s %s %s", hm, s.callsign[0] ? s.callsign : s.hex, arriving ? "from" : "to", arriving ? s.origin : s.dest);
    c.textCentred(180, 92 + i * 26, line, 2, arriving ? COL_BLUE : COL_AMBER);
  }
}

static void drawLog(Canvas &c, const Settings &set, const PageData &d) {
  pageFrame(c);
  char line[64], hm[8];
  c.textCentred(180, 40, "Overhead", 3, COL_HI);
  snprintf(line, sizeof line, "passed within %g km", (double)d.overhead_km);
  c.textCentred(180, 42 + lineHeight(3), line, 1, COL_DIM);
  const Sighting *rows[5];
  int n = newest(d.seen, rows, 5, [&](const Sighting &s) { return s.min_km <= d.overhead_km; }, [](const Sighting &s) { return (int32_t)s.last_ms; });
  if (!n) c.textCentred(180, 160, "nothing yet", 1, COL_DIM);
  for (int i = 0; i < n; i++) {
    const Sighting &s = *rows[i];
    const int y = 90 + i * 40;
    timeText(s.closest_at, d.utc_offset_min, hm);
    snprintf(line, sizeof line, "%s %s %s", hm, s.callsign[0] ? s.callsign : s.hex, typeShown(set, s.type));
    c.textCentred(180, y, line, 2, COL_TEXT);
    int k = snprintf(line, sizeof line, "FL%03d  %.1f km", s.alt_ft / 100, (double)s.min_km);
    if (s.origin[0] && s.dest[0]) snprintf(line + k, sizeof line - k, "  %s > %s", s.origin, s.dest);
    c.textCentred(180, y + lineHeight(2) + 2, line, 1, COL_BLUE);
  }
}

void drawPage(Canvas &c, const Radar &r, const PageData &d) {
  switch ((Page)r.page) {
    case Page::Weather: drawWeather(c, d); break;
    case Page::Board: drawBoard(c, d); break;
    case Page::Log: drawLog(c, r.set, d); break;
    default: break;
  }
}

// ---- unusual visitors ----
bool typeListed(const char *list, const char *type) {
  size_t n = strlen(type);
  if (!n) return false;
  for (const char *p = list; *p;) {
    const char *end = strchr(p, ',');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len == n && strncmp(p, type, n) == 0) return true;
    if (!end) break;
    p = end + 1;
  }
  return false;
}

void Visitors::check(Radar &r, const char *types, bool helicopters, uint32_t now) {
  bool busy = until && (int32_t)(now - until) < 0;
  for (int i = 0; i < r.count; i++) {
    const Aircraft &a = r.ac[i];
    float x, y;
    if (!a.hex[0] || a.on_ground || !r.project(a, x, y)) continue;
    const bool mil = a.shape == Shape::Military, heli = helicopters && a.shape == Shape::Heli, listed = typeListed(types, a.type);
    if (!mil && !heli && !listed) continue;
    bool known = false;
    for (int k = 0; k < 16; k++)
      if (strcmp(seen[k], a.hex) == 0 && now - seen_ms[k] < 60UL * 60UL * 1000UL) { seen_ms[k] = now; known = true; }   // still about: not news
    if (known) continue;
    memcpy(seen[next], a.hex, sizeof seen[next]); seen_ms[next] = now; next = (next + 1) % 16;
    if (busy) continue;                                 // one notice at a time; the others are simply marked as seen
    const char *name = a.callsign[0] ? a.callsign : a.hex;
    const char *type = typeShown(r.set, a.type);
    if (strlen(type) + strlen(name) > 17) type = a.type;   // the heading is one line of the notice: a long name gives way to the code
    if (listed) snprintf(line1, sizeof line1, "%.14s %.8s", type, name);
    else snprintf(line1, sizeof line1, "%s %.8s", mil ? "MILITARY" : "HELICOPTER", name);
    const double kx = 111.32 * cos(r.home_lat * 3.14159265358979 / 180.0);
    float east = (float)((a.lon - r.home_lon) * kx), north = (float)((a.lat - r.home_lat) * 110.57);
    float bearing = atan2f(east, north) * 180.0f / PI_F;
    int km = (int)lroundf(sqrtf(east * east + north * north));
    int k = 0;
    if (!listed && a.type[0]) k = snprintf(line2, sizeof line2, "%.14s  ", typeShown(r.set, a.type));
    snprintf(line2 + k, sizeof line2 - k, "%d km %s  FL%03d", km > 999 ? 999 : km, compass(bearing), a.alt_ft / 100);
    until = (now + 30000UL) | 1; forced = false;
    busy = true;
    r.selected = i; r.last_cycle_ms = now;              // and the highlight goes to it
  }
}

void Visitors::tick(Radar &r, uint32_t now, bool enabled) {
  if (!until) return;
  if ((!enabled && !forced) || (int32_t)(now - until) >= 0) { until = 0; forced = false; return; }
  if (r.alert[0]) return;                               // an emergency has the space
  snprintf(r.alert, sizeof r.alert, "%s", line1);
  snprintf(r.alert2, sizeof r.alert2, "%s", line2);
  r.alert_style = 1;
}

void Visitors::say(const char *first, const char *second, uint32_t now, uint32_t for_ms) {
  snprintf(line1, sizeof line1, "%s", first);
  snprintf(line2, sizeof line2, "%s", second);
  until = (now + for_ms) | 1; forced = true;
}

}  // namespace rm
