// RadarMASH - open firmware for the GeekMagic Radar 360.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "control.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace rm {

static const uint32_t DEBOUNCE_MS = 30, LONG_MS = 700, TAP_GAP_MS = 350;

Press Button::feed(bool pressed, uint32_t now) {
  if (pressed != raw_) { raw_ = pressed; change_ms_ = now; }
  if (raw_ != down_ && now - change_ms_ > DEBOUNCE_MS) {
    down_ = raw_;
    if (down_) { down_ms_ = now; long_done_ = false; }
    else if (!long_done_) { if (taps_ < 250) taps_++; up_ms_ = now; }
  }
  if (down_ && !long_done_ && now - down_ms_ > LONG_MS) { long_done_ = true; taps_ = 0; return Press::Long; }
  if (!down_ && taps_ && now - up_ms_ > TAP_GAP_MS) {
    const uint8_t n = taps_;
    taps_ = 0;
    return n == 1 ? Press::Short : n == 2 ? Press::Double : n >= 5 ? Press::Reset : Press::None;
  }
  return Press::None;
}

void setPage(Radar &r, Page p, uint32_t now) {
  r.page = (uint8_t)((int)p < (int)Page::Count ? p : Page::Radar);
  r.page_since_ms = now;
}

void doPress(Press p, Radar &r, Watch &w, uint32_t now) {
  switch (p) {
    case Press::Short:
      if (w.press(r, now)) setPage(r, Page::Radar, now);              // going to look at an emergency, or coming home
      else if (r.page != (uint8_t)Page::Radar) setPage(r, Page::Radar, now);
      else r.shortPress();
      break;
    case Press::Double: setPage(r, nextPage(r.set, (Page)r.page), now); break;
    case Press::Long:
      if (r.page == (uint8_t)Page::Radar && !w.away) r.longPress();
      break;
    default: break;
  }
}

void pageTick(Radar &r, const Watch &w, int cycle_s, uint32_t now) {
  if (cycle_s <= 0 || w.away || w.alerting()) { if (cycle_s <= 0) r.page_since_ms = now; return; }
  if (now - r.page_since_ms >= (uint32_t)cycle_s * 1000UL) setPage(r, nextPage(r.set, (Page)r.page), now);
}

bool keyOk(const char *configured, const char *given) {
  if (!configured || !configured[0]) return true;
  if (!given) return false;
  size_t a = strlen(configured), b = strlen(given);
  unsigned char diff = (unsigned char)(a != b);
  for (size_t i = 0; i < a; i++) diff |= (unsigned char)(configured[i] ^ given[i < b ? i : b]);   // no early exit: time says nothing
  return diff == 0;
}

static bool word(const char *v, const char *const *list) {
  for (; *list; list++) if (strcmp(v, *list) == 0) return true;
  return false;
}
static bool isNumber(const char *v) {
  if (!*v) return false;
  char *end = nullptr;
  strtod(v, &end);
  if (!end || *end) return false;
  for (const char *p = v; *p; p++)                      // digits only: no "inf", "nan" or hex, which JSON does not have
    if (!((*p >= '0' && *p <= '9') || *p == '-' || *p == '.')) return false;
  return true;
}

// Settings a one-line request may change: what is on the screen, not where data comes from or where the radar is.
static const char *const NUMBERS[] = {"brightness", "map_style", "map_brightness", "max_aircraft", "map_detail", "sweep_seconds", "refresh_s",
                                      "range_km", "night", "night_brightness", "night_start_min", "night_end_min", "units", "page_cycle_s",
                                      "overhead_km", nullptr};
static const char *const WORDS[] = {"font", "board_airport", nullptr};

Cmd command(const char *name, const char *value, Radar &r, Watch &w, uint32_t now, char *json, size_t n) {
  static const char *const ON[] = {"on", "true", "1", "yes", nullptr}, *const OFF[] = {"off", "false", "0", "no", nullptr};
  if (json && n) json[0] = 0;
  if (strcmp(name, "page") == 0) {
    Page p;
    if (strcmp(value, "next") == 0) p = nextPage(r.set, (Page)r.page);
    else if (!pageByName(value, p)) return Cmd::Bad;
    setPage(r, p, now);
    return Cmd::Done;
  }
  if (strcmp(name, "press") == 0) {
    Press p = strcmp(value, "short") == 0 || !value[0] ? Press::Short : strcmp(value, "double") == 0 ? Press::Double
              : strcmp(value, "long") == 0 ? Press::Long : Press::None;
    if (p == Press::None) return Cmd::Bad;
    doPress(p, r, w, now);
    return Cmd::Done;
  }
  if (strcmp(name, "range") == 0) {
    if (strcmp(value, "next") == 0) { if (!w.away) r.longPress(); return Cmd::Done; }   // whatever page is showing
    name = "range_km";
  }
  if (strcmp(name, "alert") == 0) {                     // look: go to the emergency on offer. home: come back.
    if (strcmp(value, "look") == 0) { if (w.alerting() && w.press(r, now)) setPage(r, Page::Radar, now); return Cmd::Done; }
    if (strcmp(value, "home") == 0) { if (w.away) w.press(r, now); return Cmd::Done; }
    return Cmd::Bad;
  }
  if (strcmp(name, "select") == 0) {                    // highlight an aircraft by callsign
    for (int i = 0; i < r.count; i++)
      if (strcmp(r.ac[i].callsign, value) == 0) { r.selected = i; r.last_cycle_ms = now; setPage(r, Page::Radar, now); return Cmd::Done; }
    return Cmd::Bad;
  }
  if (bool *t = toggleByName(r.set, name)) {
    bool to;
    if (word(value, ON)) to = true; else if (word(value, OFF)) to = false; else if (strcmp(value, "toggle") == 0) to = !*t; else return Cmd::Bad;
    snprintf(json, n, "{\"%s\":%s}", name, to ? "true" : "false");
    return Cmd::Settings;
  }
  if (word(name, NUMBERS)) {
    if (!isNumber(value) || strlen(value) > 12) return Cmd::Bad;
    snprintf(json, n, "{\"%s\":%s}", name, value);
    return Cmd::Settings;
  }
  if (word(name, WORDS)) {
    if (strlen(value) > 15) return Cmd::Bad;
    for (const char *p = value; *p; p++)
      if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9'))) return Cmd::Bad;
    snprintf(json, n, "{\"%s\":\"%s\"}", name, value);
    return Cmd::Settings;
  }
  return Cmd::Unknown;
}

}  // namespace rm
