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
  logo_url[0] = 0; board_airport[0] = 0; api_key[0] = 0;
  snprintf(visitor_types, sizeof visitor_types, "%s", VISITOR_TYPES);
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

size_t configToJson(const Config &c, char *out, size_t n, bool for_saving) {
  if (!out || n < 2) return 0;
  Out o{out, n};
  o.add("{\"v\":1,");
  for (int i = 0; i < TOGGLE_COUNT; i++) o.add("\"%s\":%s,", TOGGLES[i].name, c.set.*TOGGLES[i].field ? "true" : "false");
  o.add("\"map_style\":%d,\"map_brightness\":%d,\"max_aircraft\":%d,", (int)c.set.map_style, (int)c.set.map_brightness, (int)c.set.max_aircraft);
  o.add("\"map_detail\":%d,\"sweep_seconds\":%d,", (int)c.set.map_detail, (int)c.set.sweep_seconds);
  o.add("\"home_set\":%s,\"home_lat\":%.5f,\"home_lon\":%.5f,", c.home_set ? "true" : "false", c.home_lat, c.home_lon);
  o.add("\"range_km\":%d,\"live\":%s,\"refresh_s\":%d,\"include_ground\":%s,\"brightness\":%d,", (int)lroundf(c.range_km), c.live ? "true" : "false",
        (int)c.refresh_s, c.include_ground ? "true" : "false", (int)c.brightness);
  o.add("\"verify_certs\":%s,\"utc_auto\":%s,\"utc_offset_min\":%d,", c.verify_certs ? "true" : "false", c.utc_auto ? "true" : "false", (int)c.utc_offset_min);
  o.str("font", FONTS[c.set.font < FONT_COUNT ? c.set.font : 0].id);
  o.str("feed_url", c.feed_url);
  o.str("map_url", c.map_url);
  o.str("map_credit", c.map_credit);
  o.str("logo_url", c.logo_url);
  o.add("\"night\":%d,\"night_brightness\":%d,\"night_start_min\":%d,\"night_end_min\":%d,", (int)c.night, (int)c.night_brightness, (int)c.night_start_min, (int)c.night_end_min);
  o.add("\"units\":%d,\"page_cycle_s\":%d,\"overhead_km\":%.1f,", (int)c.units, (int)c.page_cycle_s, (double)c.overhead_km);
  o.str("board_airport", c.board_airport);
  o.str("visitor_types", c.visitor_types);
  if (for_saving) o.str("api_key", c.api_key); else o.add("\"api_key_set\":%s,", c.api_key[0] ? "true" : "false");
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
  if (getNum(root, "map_detail", 0, 1, d)) c.set.map_detail = (uint8_t)d;
  if (getNum(root, "sweep_seconds", 2, 30, d)) c.set.sweep_seconds = (uint8_t)d;

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
  getBool(root, "verify_certs", c.verify_certs);
  getBool(root, "utc_auto", c.utc_auto);
  if (getNum(root, "utc_offset_min", -720, 840, d)) c.utc_offset_min = (int16_t)d;
  if (getNum(root, "brightness", 1, 100, d)) c.brightness = (uint8_t)d;   // never fully dark: a black screen looks broken
  if (json::member(root, "font", v)) {
    char id[16];
    int i = json::string(v, id, sizeof id) ? fontIndex(id) : -1;
    if (i >= 0) c.set.font = (uint8_t)i;                    // a font this build does not have: keep the current one
  }
  getUrl(root, "feed_url", c.feed_url, sizeof c.feed_url);
  getUrl(root, "map_url", c.map_url, sizeof c.map_url);
  if (json::member(root, "logo_url", v)) {
    char buf[8];
    if (json::string(v, buf, sizeof buf) && !buf[0]) c.logo_url[0] = 0;          // an empty string turns logos off
    else getUrl(root, "logo_url", c.logo_url, sizeof c.logo_url);
  }
  if (getNum(root, "night", 0, 2, d)) c.night = (uint8_t)d;
  if (getNum(root, "night_brightness", 1, 100, d)) c.night_brightness = (uint8_t)d;
  if (getNum(root, "night_start_min", 0, 1439, d)) c.night_start_min = (int16_t)d;
  if (getNum(root, "night_end_min", 0, 1439, d)) c.night_end_min = (int16_t)d;
  if (getNum(root, "units", 0, (int)Units::Count - 1, d)) c.units = (Units)(int)d;
  if (getNum(root, "page_cycle_s", 0, 3600, d)) c.page_cycle_s = (uint16_t)(d > 0 && d < 5 ? 5 : d);   // faster than that is a strobe
  if (getNum(root, "overhead_km", 0.5, 50, d)) c.overhead_km = (float)d;
  // Short codes: letters and digits, made capitals; anything else in them and the whole value is ignored.
  auto codes = [&](const char *key, char *out, size_t n, bool commas) {
    char buf[128];
    if (!json::member(root, key, v) || !json::string(v, buf, sizeof buf)) return;
    char clean[128];
    size_t k = 0;
    for (const char *s = buf; *s; s++) {
      char ch = *s;
      if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
      if (ch == ' ') continue;
      if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || (commas && ch == ','))) return;
      clean[k++] = ch;
    }
    clean[k] = 0;
    if (k < n) memcpy(out, clean, k + 1);
  };
  codes("board_airport", c.board_airport, sizeof c.board_airport, false);
  codes("visitor_types", c.visitor_types, sizeof c.visitor_types, true);
  if (json::member(root, "api_key", v)) {             // plain characters that are safe in a web address; empty removes it
    char buf[64];
    if (json::string(v, buf, sizeof buf) && strlen(buf) < sizeof c.api_key) {
      bool good = true;
      for (const char *s = buf; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '-' || *s == '_' || *s == '.')) good = false;
      if (good) memcpy(c.api_key, buf, strlen(buf) + 1);
    }
  }
  if (json::member(root, "map_credit", v)) {
    char buf[sizeof c.map_credit];
    if (json::string(v, buf, sizeof buf)) memcpy(c.map_credit, buf, sizeof buf);
  }
  return true;
}

bool parseWifiRequest(const char *text, size_t len, char ssid[33], char pass[65]) {
  ssid[0] = 0; pass[0] = 0;
  json::Span root = json::parse(text, len), s, p;
  if (!json::isObject(root) || !json::member(root, "ssid", s)) return false;
  char name[80], word[140];
  if (!json::string(s, name, sizeof name)) return false;
  word[0] = 0;
  if (json::member(root, "pass", p) && !json::string(p, word, sizeof word)) return false;
  size_t nl = strlen(name), wl = strlen(word);
  if (nl < 1 || nl > 32 || !(wl == 0 || (wl >= 8 && wl <= 63))) return false;
  memcpy(ssid, name, nl + 1); memcpy(pass, word, wl + 1);
  return true;
}

size_t jsonEscape(const char *in, char *out, size_t n) {
  size_t o = 0;
  if (!n) return 0;
  for (; *in && o + 2 < n; in++) {
    unsigned char ch = (unsigned char)*in;
    if (ch < 32) continue;
    if (ch == '"' || ch == '\\') out[o++] = '\\';
    out[o++] = (char)ch;
  }
  out[o] = 0;
  return o;
}

}  // namespace rm
