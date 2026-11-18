// RadarMASH - open firmware for the GeekMagic Radar 360.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "alert.h"
#include "json.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace rm {

static const uint32_t AWAY_MAX_MS = 10UL * 60UL * 1000UL;
static const float AWAY_RANGE_KM = 40;

bool isEmergencySquawk(const char *s) { return strcmp(s, "7700") == 0 || strcmp(s, "7600") == 0 || strcmp(s, "7500") == 0; }

double distanceKm(double lat1, double lon1, double lat2, double lon2) {
  const double rad = 3.14159265358979 / 180.0;
  double a = sin((lat2 - lat1) * rad / 2), b = sin((lon2 - lon1) * rad / 2);
  double h = a * a + cos(lat1 * rad) * cos(lat2 * rad) * b * b;
  return 2 * 6371.0 * asin(sqrt(h > 1 ? 1 : h));
}

void Watch::found(const Aircraft *list, int n) {
  for (int i = 0; i < n; i++) {
    if (list[i].on_ground || !list[i].hex[0]) continue;
    // while away, stay with the one being watched if it is still there
    if (away && have && strcmp(list[i].hex, target.hex) != 0) {
      bool still = false;
      for (int k = 0; k < n; k++) if (strcmp(list[k].hex, target.hex) == 0 && !list[k].on_ground) still = true;
      if (still) continue;
    }
    target = list[i]; have = true;
    return;
  }
  have = false;
  dismissed[0] = 0;                                                  // all clear: the next one is news again
}

bool Watch::alerting() const { return have && !away && strcmp(target.hex, dismissed) != 0; }

void Watch::comeHome(Radar &r) {
  if (!away) return;
  away = false;
  r.home_lat = back_lat; r.home_lon = back_lon; r.range_km = back_range;
  r.pin_hex[0] = 0;
  r.count = 0; r.selected = -1;                                      // a different sky
}

bool Watch::press(Radar &r, uint32_t now) {
  if (away) { memcpy(dismissed, target.hex, sizeof dismissed); comeHome(r); return true; }
  if (!alerting()) return false;
  back_lat = r.home_lat; back_lon = r.home_lon; back_range = r.range_km;
  away = true; away_since = now;
  r.home_lat = target.lat; r.home_lon = target.lon; r.range_km = AWAY_RANGE_KM;
  memcpy(r.pin_hex, target.hex, sizeof r.pin_hex);
  r.count = 1; r.ac[0] = target; r.ac[0].trail_len = 0;             // something to look at until the feed for there arrives
  r.ac[0].live = true; r.ac[0].fix_lat = target.lat; r.ac[0].fix_lon = target.lon; r.ac[0].fix_ms = now;
  r.selected = 0;
  return true;
}

void Watch::tick(Radar &r, uint32_t now, bool enabled) {
  if (away) {
    if (!enabled || !have || now - away_since > AWAY_MAX_MS) {       // over, switched off, or forgotten about
      memcpy(dismissed, target.hex, sizeof dismissed);
      comeHome(r);
    } else {
      // keep it on the scope: when it nears the edge, centre on it again
      for (int i = 0; i < r.count; i++) {
        if (strcmp(r.ac[i].hex, target.hex) != 0) continue;
        if (distanceKm(r.home_lat, r.home_lon, r.ac[i].lat, r.ac[i].lon) > r.range_km * 0.6) { r.home_lat = r.ac[i].lat; r.home_lon = r.ac[i].lon; }
        break;
      }
    }
  }
  r.alert[0] = r.alert2[0] = 0;
  if (!enabled || !have) return;
  if (away) {
    snprintf(r.alert, sizeof r.alert, "%.4s %.8s", target.squawk[0] ? target.squawk : "7700", target.callsign);
    snprintf(r.alert2, sizeof r.alert2, "press to go home");
  } else if (alerting()) {
    int km = (int)distanceKm(r.home_lat, r.home_lon, target.lat, target.lon);
    snprintf(r.alert, sizeof r.alert, "%.4s %.8s", target.squawk[0] ? target.squawk : "7700", target.callsign);
    snprintf(r.alert2, sizeof r.alert2, "%d km away: press to look", km > 99999 ? 99999 : km);
  }
}

bool parseUtcOffset(const char *text, size_t len, int &minutes) {
  json::Span root = json::parse(text, len), v;
  double s;
  if (!json::isObject(root) || !json::member(root, "utc_offset_seconds", v) || !json::number(v, s) || !isfinite(s)) return false;
  if (s < -12 * 3600 || s > 14 * 3600) return false;
  minutes = (int)lround(s / 60.0);
  return true;
}

int clockMinutes(long long unix_utc, int offset_min) {
  if (unix_utc < 1700000000LL) return -1;
  long long m = (unix_utc / 60 + offset_min) % 1440;
  return (int)(m < 0 ? m + 1440 : m);
}

}  // namespace rm
