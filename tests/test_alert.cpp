// RadarMASH - open firmware for the GeekMagic Radar 360.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../core/alert.h"
#include "../core/config.h"
#include "../core/feed.h"
#include "test.h"
#include <stdio.h>
#include <string.h>
#include <vector>

using namespace rm;

namespace {
Aircraft plane(const char *hex, const char *cs, double lat, double lon, const char *sq = "7700") {
  Aircraft a;
  snprintf(a.hex, sizeof a.hex, "%s", hex); snprintf(a.callsign, sizeof a.callsign, "%s", cs); snprintf(a.squawk, sizeof a.squawk, "%s", sq);
  a.lat = lat; a.lon = lon; a.alt_ft = 30000; a.speed_kt = 400;
  return a;
}
Radar *freshRadar() {                                // a Radar is big: keep it off the stack
  static Radar r;
  r = Radar();
  r.home_lat = 51.0; r.home_lon = -1.0; r.range_km = 20;
  return &r;
}
}  // namespace

TEST(alert_only_the_three_distress_codes_count) {
  CHECK(isEmergencySquawk("7700") && isEmergencySquawk("7600") && isEmergencySquawk("7500"));
  CHECK(!isEmergencySquawk("7000") && !isEmergencySquawk("") && !isEmergencySquawk("1234") && !isEmergencySquawk("77000"));
}

TEST(alert_feed_squawk_is_read_and_checked) {
  const char *j = "{\"ac\":[{\"hex\":\"abc123\",\"flight\":\"TST1\",\"lat\":51.1,\"lon\":-1.1,\"alt_baro\":9000,\"squawk\":\"7700\"},"
                  "{\"hex\":\"abc124\",\"flight\":\"TST2\",\"lat\":51.2,\"lon\":-1.1,\"alt_baro\":9000,\"squawk\":\"77x0<\"},"
                  "{\"hex\":\"abc125\",\"flight\":\"TST3\",\"lat\":51.3,\"lon\":-1.1,\"alt_baro\":9000}]}";
  std::vector<Aircraft> out(8);
  CHECK((parseAircraft(j, strlen(j), 51.0, -1.0, false, out.data(), 8)) == (3));
  CHECK(strcmp(out[0].squawk, "7700") == 0);
  CHECK(out[1].squawk[0] == 0 && out[2].squawk[0] == 0);
}

TEST(alert_press_goes_to_look_and_press_again_comes_home) {
  Radar &r = *freshRadar();
  Watch w;
  Aircraft far = plane("e00001", "MAYDAY1", -33.9, 151.2);
  w.found(&far, 1);
  w.tick(r, 1000, true);
  CHECK(w.alerting());
  CHECK(strncmp(r.alert, "7700 MAYDAY1", 12) == 0 && strstr(r.alert2, "km away") != nullptr);

  CHECK(w.press(r, 2000));
  CHECK(w.away && r.home_lat == -33.9 && r.home_lon == 151.2 && r.range_km == 40);
  CHECK(strcmp(r.pin_hex, "e00001") == 0 && r.count == 1 && r.selected == 0);
  w.tick(r, 2100, true);
  CHECK(strstr(r.alert2, "home") != nullptr);
  r.tick(2200);
  CHECK((r.selected) == (0));                           // held on it: no cycling away

  CHECK(w.press(r, 3000));
  CHECK(!w.away && r.home_lat == 51.0 && r.home_lon == -1.0 && r.range_km == 20 && r.pin_hex[0] == 0);
  w.tick(r, 3100, true);
  CHECK(!w.alerting() && r.alert[0] == 0);           // seen: it does not ask again
  CHECK(!w.press(r, 3200));                          // so the button is back to its usual job
}

TEST(alert_clears_and_a_new_one_asks_again) {
  Radar &r = *freshRadar();
  Watch w;
  Aircraft a = plane("e00001", "MAYDAY1", 10, 10);
  w.found(&a, 1); w.press(r, 0); w.press(r, 10);
  CHECK(!w.alerting());
  w.found(nullptr, 0);
  CHECK(!w.have);
  w.found(&a, 1);
  CHECK(w.alerting());                               // the same aircraft in trouble again later is news
}

TEST(alert_comes_home_alone_when_it_is_over_or_forgotten) {
  Radar &r = *freshRadar();
  Watch w;
  Aircraft a = plane("e00001", "MAYDAY1", 10, 10);
  w.found(&a, 1); w.press(r, 0);
  w.found(nullptr, 0);
  w.tick(r, 1000, true);
  CHECK(!w.away && r.home_lat == 51.0);

  Aircraft b = plane("e00002", "MAYDAY2", 20, 20);   // (the first has been seen, so it would not ask again)
  w.found(&b, 1); w.press(r, 2000);
  w.tick(r, 2000 + 9 * 60 * 1000, true);
  CHECK(w.away);
  w.tick(r, 2000 + 11 * 60 * 1000, true);
  CHECK(!w.away && r.home_lat == 51.0);

  w = Watch(); w.found(&a, 1); w.press(r, 0);
  w.tick(r, 100, false);                             // switched off in the settings while away
  CHECK(!w.away && r.alert[0] == 0);
}

TEST(alert_follows_the_aircraft_and_ignores_ones_on_the_ground) {
  Radar &r = *freshRadar();
  Watch w;
  Aircraft list[2] = {plane("e00009", "PARKED", 0, 0), plane("e00001", "MAYDAY1", 10, 10)};
  list[0].on_ground = true;
  w.found(list, 2);
  CHECK(strcmp(w.target.hex, "e00001") == 0);
  w.press(r, 0);
  r.ac[0].lat = 10.3;                                // 33 km north of the centre, range 40: time to re-centre
  w.tick(r, 100, true);
  CHECK(r.home_lat == 10.3);
  Aircraft other[2] = {plane("e00002", "OTHER", 50, 0), plane("e00001", "MAYDAY1", 10.4, 10)};
  w.found(other, 2);
  CHECK(strcmp(w.target.hex, "e00001") == 0);        // stays with the one being watched
}

TEST(alert_emergency_aircraft_draws_the_same_in_strips) {
  Radar &r = *freshRadar();
  r.count = 1; r.ac[0] = plane("e00001", "MAYDAY1", 51.02, -1.02); r.ac[0].live = false;
  snprintf(r.alert, sizeof r.alert, "7700 MAYDAY1"); snprintf(r.alert2, sizeof r.alert2, "12 km away: press to look");
  r.clock_min = 14 * 60 + 5; r.clock_utc = true;
  r.set.auto_cycle = false; r.now_ms = 250;
  static uint16_t whole[SCREEN_W * SCREEN_H], strips[SCREEN_W * SCREEN_H];
  Canvas all(whole, 0, SCREEN_H);
  r.draw(all);
  for (int y = 0; y < SCREEN_H; y += 40) { Canvas c(strips + y * SCREEN_W, y, 40); r.draw(c); }
  CHECK(memcmp(whole, strips, sizeof whole) == 0);
  int red = 0;
  for (int i = 0; i < SCREEN_W * SCREEN_H; i++) if ((whole[i] >> 11) > 16 && ((whole[i] >> 5) & 63) < 12) red++;
  CHECK(red > 500);                                  // the notice and the aircraft are there, and red
}

TEST(clock_minutes_and_offsets) {
  CHECK((clockMinutes(0, 0)) == (-1));                  // not set
  long long midnight = 1800000000LL - 1800000000LL % 86400;
  CHECK((clockMinutes(midnight + 30 * 60, 0)) == (30));
  CHECK((clockMinutes(midnight + 30 * 60, -60)) == (1410));    // 23:30 the day before
  CHECK((clockMinutes(midnight + 23 * 3600, 330)) == (270));   // India: 04:30 next day
}

TEST(clock_offset_reply_is_read_and_checked) {
  int m = 99;
  const char *ok = "{\"latitude\":51.0,\"utc_offset_seconds\":3600,\"timezone\":\"Europe/London\"}";
  CHECK(parseUtcOffset(ok, strlen(ok), m) && m == 60);
  const char *nepal = "{\"utc_offset_seconds\":20700}";
  CHECK(parseUtcOffset(nepal, strlen(nepal), m) && m == 345);
  const char *silly = "{\"utc_offset_seconds\":999999}", *none = "{\"error\":true}";
  CHECK(!parseUtcOffset(silly, strlen(silly), m) && !parseUtcOffset(none, strlen(none), m) && !parseUtcOffset("x", 1, m));
}

TEST(clock_settings_round_trip_and_clamp) {
  Config c;
  CHECK(c.utc_auto && c.utc_offset_min == 0 && c.set.clock && c.set.alerts);
  const char *j = "{\"utc_auto\":false,\"utc_offset_min\":330,\"clock\":false,\"alerts\":false}";
  CHECK(configFromJson(j, strlen(j), c));
  char buf[CONFIG_JSON_MAX];
  CHECK(configToJson(c, buf, sizeof buf) > 0);
  Config d;
  CHECK(configFromJson(buf, strlen(buf), d));
  CHECK(!d.utc_auto && d.utc_offset_min == 330 && !d.set.clock && !d.set.alerts);
  const char *wild = "{\"utc_offset_min\":100000}";
  configFromJson(wild, strlen(wild), d);
  CHECK(((int)d.utc_offset_min) == (840));
}
