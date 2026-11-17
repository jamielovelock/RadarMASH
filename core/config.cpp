// RadarMASH - open firmware for the GeekMagic Radar 360.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "config.h"
#include "json.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace rm {

Config::Config() {
  snprintf(feed_url, sizeof feed_url, "%s", FEED_ADSB_FI);
  snprintf(map_url, sizeof map_url, "%s", MAP_OSM);
  snprintf(map_credit, sizeof map_credit, "%s", "(c) OpenStreetMap");
}

// ---- writing ----
struct Out {
  char *p; size_t n, used = 0; bool ok = true;
  void add(const char *fmt, ...) {
    if (!ok) return;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(p + used, n - used, fmt, ap);
    va_end(ap);
    if (w < 0 || (size_t)w >= n - used) ok = false; else used += (size_t)w;
  }
  void str(const char *key, const char *value) {        // quotes and backslashes escaped, control characters dropped
    add("\"%s\":\"", key);
    for (const char *s = value; *s && ok; s++) {
      if ((unsigned char)*s < 32) continue;
      if (*s == '"' || *s == '\\') add("\\%c", *s); else add("%c", *s);
    }
    add("\",");
  }
};

size_t configToJson(const Config &c, char *out, size_t n) {
  if (!out || n < 2) return 0;
  Out o{out, n};
  o.add("{\"v\":1,");
  for (int i = 0; i < TOGGLE_COUNT; i++) o.add("\"%s\":%s,", TOGGLES[i].name, c.set.*TOGGLES[i].field ? "true" : "false");
  o.add("\"map_style\":%d,\"map_brightness\":%d,\"max_aircraft\":%d,", (int)c.set.map_style, (int)c.set.map_brightness, (int)c.set.max_aircraft);
  o.add("\"home_set\":%s,\"home_lat\":%.5f,\"home_lon\":%.5f,", c.home_set ? "true" : "false", c.home_lat, c.home_lon);
  o.add("\"range_km\":%d,\"live\":%s,\"refresh_s\":%d,\"include_ground\":%s,\"brightness\":%d,", (int)lroundf(c.range_km), c.live ? "true" : "false",
        (int)c.refresh_s, c.include_ground ? "true" : "false", (int)c.brightness);
  o.str("feed_url", c.feed_url);
  o.str("map_url", c.map_url);
  o.str("map_credit", c.map_credit);
  if (!o.ok || o.used + 1 >= n) { out[0] = 0; return 0; }
  out[o.used - 1] = '}';                                // replace the last comma
  out[o.used] = 0;
  return o.used;
}

// ---- reading ----
static bool getNum(json::Span obj, const char *key, double lo, double hi, double &out) {
  json::Span v;
  double d;
  if (!json::member(obj, key, v) || !json::number(v, d) || !isfinite(d)) return false;
  out = d < lo ? lo : d > hi ? hi : d;
  return true;
}
static void getBool(json::Span obj, const char *key, bool &out) {
  json::Span v;
  bool b;
  if (json::member(obj, key, v) && json::boolean(v, b)) out = b;
}
// A web address is only accepted if it is plainly one, and fits.
static void getUrl(json::Span obj, const char *key, char *out, size_t n) {
  json::Span v;
  char buf[200];
  if (!json::member(obj, key, v) || !json::string(v, buf, sizeof buf)) return;
  size_t len = strlen(buf);
  if (len >= n || (strncmp(buf, "https://", 8) != 0 && strncmp(buf, "http://", 7) != 0) || strchr(buf, ' ')) return;
  memcpy(out, buf, len + 1);
}

bool configFromJson(const char *text, size_t len, Config &c) {
  json::Span root = json::parse(text, len), v;
  if (!json::isObject(root)) return false;
  double d;
  for (int i = 0; i < TOGGLE_COUNT; i++) getBool(root, TOGGLES[i].name, c.set.*TOGGLES[i].field);
  if (getNum(root, "map_style", 0, (int)MapStyle::Count - 1, d)) c.set.map_style = (MapStyle)(int)d;
  if (getNum(root, "map_brightness", 0, 100, d)) c.set.map_brightness = (uint8_t)d;
  if (getNum(root, "max_aircraft", 1, MAX_AIRCRAFT, d)) c.set.max_aircraft = (uint8_t)d;

  // Home is only taken as a pair, and only if both halves are real coordinates (no clamping: a wrong place is worse
  // than no place).
  double lat, lon;
  json::Span la, lo;
  if (json::member(root, "home_lat", la) && json::member(root, "home_lon", lo) && json::number(la, lat) && json::number(lo, lon) &&
      isfinite(lat) && isfinite(lon) && lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180) {
    c.home_lat = lat; c.home_lon = lon; c.home_set = true;
    getBool(root, "home_set", c.home_set);              // a saved file may say the pair is only the demo default
  } else if (json::member(root, "home_set", v)) {
    bool b;
    if (json::boolean(v, b) && !b) c.home_set = false;  // "forget where I am"
  }

  if (getNum(root, "range_km", 5, 400, d)) c.range_km = (float)d;
  getBool(root, "live", c.live);
  if (getNum(root, "refresh_s", 2, 300, d)) c.refresh_s = (uint16_t)d;
  getBool(root, "include_ground", c.include_ground);
  if (getNum(root, "brightness", 1, 100, d)) c.brightness = (uint8_t)d;   // never fully dark: a black screen looks broken
  getUrl(root, "feed_url", c.feed_url, sizeof c.feed_url);
  getUrl(root, "map_url", c.map_url, sizeof c.map_url);
  if (json::member(root, "map_credit", v)) {
    char buf[sizeof c.map_credit];
    if (json::string(v, buf, sizeof buf)) memcpy(c.map_credit, buf, sizeof buf);
  }
  return true;
}

}  // namespace rm
