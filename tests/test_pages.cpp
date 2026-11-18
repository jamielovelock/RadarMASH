// RadarMASH - open firmware for the GeekMagic Radar 360.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../core/alert.h"
#include "../core/config.h"
#include "../core/control.h"
#include "../core/pages.h"
#include "test.h"
#include <stdio.h>
#include <string.h>

using namespace rm;

namespace {
const char *WX =
    "{\"latitude\":51.0,\"utc_offset_seconds\":3600,\"current\":{\"time\":\"2026-07-01T17:00\",\"temperature_2m\":9.9,\"apparent_temperature\":6.5,"
    "\"relative_humidity_2m\":74,\"precipitation\":0.10,\"weather_code\":51,\"cloud_cover\":4,\"wind_speed_10m\":15.8,\"wind_direction_10m\":256,"
    "\"wind_gusts_10m\":29.9,\"pressure_msl\":988.7,\"is_day\":0},\"daily\":{\"time\":[\"2026-11-18\",\"2026-11-19\",\"2026-11-20\"],"
    "\"weather_code\":[61,63,3],\"temperature_2m_max\":[12.3,9.2,7.8],\"temperature_2m_min\":[7.7,4.9,-4.6],\"precipitation_probability_max\":[100,100,39],"
    "\"sunrise\":[\"2026-11-18T07:25\",\"2026-11-19T07:26\",\"2026-11-20T07:28\"],\"sunset\":[\"2026-11-18T16:12\",\"2026-11-19T16:11\",\"2026-11-20T16:10\"]}}";

Aircraft plane(const char *hex, const char *cs, double lat, double lon) {
  Aircraft a;
  snprintf(a.hex, sizeof a.hex, "%s", hex); snprintf(a.callsign, sizeof a.callsign, "%s", cs);
  a.lat = lat; a.lon = lon; a.alt_ft = 30000; a.speed_kt = 400;
  return a;
}
Radar *freshRadar() {
  static Radar r;
  r = Radar();
  r.home_lat = 51.0; r.home_lon = -1.0; r.range_km = 20;
  return &r;
}
bool sameInStrips(Radar &r) {
  static uint16_t whole[SCREEN_W * SCREEN_H], strips[SCREEN_W * SCREEN_H];
  Canvas all(whole, 0, SCREEN_H);
  r.draw(all);
  for (int y = 0; y < SCREEN_H; y += 40) { Canvas c(strips + y * SCREEN_W, y, 40); r.draw(c); }
  return memcmp(whole, strips, sizeof whole) == 0;
}
int litPixels(Radar &r) {
  static uint16_t whole[SCREEN_W * SCREEN_H];
  Canvas all(whole, 0, SCREEN_H);
  r.draw(all);
  int n = 0;
  for (int i = 0; i < SCREEN_W * SCREEN_H; i++) n += whole[i] != 0;
  return n;
}
}  // namespace

TEST(weather_reply_is_read) {
  Weather w;
  CHECK(parseWeather(WX, strlen(WX), w));
  CHECK(w.have && w.code == 51 && w.humidity == 74 && w.cloud == 4 && w.wind_deg == 256 && !w.is_day);
  CHECK(w.temp_c > 9.8f && w.temp_c < 10.0f && w.gust_kmh > 29 && w.pressure_hpa > 988);
  CHECK(w.utc_offset_min == 60 && w.sunrise_min == 7 * 60 + 25 && w.sunset_min == 16 * 60 + 12);
  CHECK(w.days == 3 && w.day[0].hi_c == 12 && w.day[2].lo_c == -5 && w.day[2].rain_pct == 39 && w.day[1].code == 63);
  CHECK(w.day[0].weekday == 3 && w.day[2].weekday == 5);        // 18 Nov 2026 is a Wednesday
  CHECK(strcmp(weatherText(61), "Light rain") == 0 && weatherText(42)[0] == 0);
}

TEST(weather_nonsense_is_refused_and_leaves_the_old_one) {
  Weather w;
  parseWeather(WX, strlen(WX), w);
  const char *bad[] = {"", "x", "[]", "{}", "{\"current\":5}", "{\"current\":{\"temperature_2m\":900,\"weather_code\":1}}",
                       "{\"current\":{\"temperature_2m\":9,\"weather_code\":1234}}", "{\"current\":{\"weather_code\":1}}"};
  for (const char *b : bad) CHECK(!parseWeather(b, strlen(b), w));
  CHECK(w.have && w.code == 51);
  // a bare minimum is enough, and odd extras do no harm
  const char *thin = "{\"current\":{\"temperature_2m\":-3.5,\"weather_code\":71},\"daily\":{\"time\":[\"junk\"],\"sunrise\":[\"T9\"],\"weather_code\":[1],"
                     "\"temperature_2m_max\":[500],\"temperature_2m_min\":[-500]}}";
  CHECK(parseWeather(thin, strlen(thin), w));
  CHECK(w.days == 1 && w.day[0].hi_c == 99 && w.day[0].lo_c == -99 && w.day[0].weekday == -1 && w.sunrise_min == -1);
  for (size_t cut = 0; cut < strlen(WX); cut += 7) { Weather t; parseWeather(WX, cut, t); }   // cut short anywhere: no crash
}

TEST(night_by_the_sun_and_by_the_clock) {
  Weather w;
  parseWeather(WX, strlen(WX), w);
  CHECK(!isNight(0, 100, w, 0, 0));
  CHECK(isNight(1, 7 * 60 + 24, w, 0, 0) && !isNight(1, 7 * 60 + 25, w, 0, 0) && !isNight(1, 16 * 60 + 11, w, 0, 0) && isNight(1, 16 * 60 + 12, w, 0, 0));
  CHECK(!isNight(1, 100, Weather(), 0, 0));                     // no weather yet: not night
  CHECK(!isNight(1, -1, w, 0, 0) && !isNight(2, -1, w, 60, 120));   // no time yet: not night
  CHECK(isNight(2, 23 * 60, w, 22 * 60, 7 * 60) && isNight(2, 6 * 60, w, 22 * 60, 7 * 60) && !isNight(2, 12 * 60, w, 22 * 60, 7 * 60));
  CHECK(isNight(2, 13 * 60, w, 12 * 60, 14 * 60) && !isNight(2, 15 * 60, w, 12 * 60, 14 * 60) && !isNight(2, 5, w, 60, 60));
}

TEST(pages_go_round_the_ones_switched_on) {
  Settings s;
  CHECK(nextPage(s, Page::Radar) == Page::Weather && nextPage(s, Page::Log) == Page::Radar);
  s.page_weather = false; s.page_log = false;
  CHECK(nextPage(s, Page::Radar) == Page::Board && nextPage(s, Page::Board) == Page::Radar);
  s.page_board = false;
  CHECK(nextPage(s, Page::Radar) == Page::Radar && nextPage(s, Page::Weather) == Page::Radar);
  Page p;
  CHECK(pageByName("log", p) && p == Page::Log && !pageByName("nope", p) && strcmp(pageName(Page::Board), "board") == 0);
}

TEST(sightings_keep_the_closest_pass_and_make_room) {
  Radar &r = *freshRadar();
  Sightings s;
  r.count = 2; r.ac[0] = plane("aaa001", "ONE1", 51.2, -1.0); r.ac[1] = plane("aaa002", "TWO2", 51.0, -1.3);
  snprintf(r.ac[0].origin, 5, "LHR"); snprintf(r.ac[0].dest, 5, "JFK");
  s.note(r, 1800000000, 1000);
  CHECK(s.n == 2 && s.e[0].min_km > 20 && s.e[0].min_km < 24);
  r.ac[0].lat = 51.01; r.ac[0].alt_ft = 12000;
  s.note(r, 1800000060, 61000);
  CHECK(s.n == 2 && s.e[0].min_km < 1.5f && s.e[0].alt_ft == 12000 && s.e[0].closest_at == 1800000060);
  r.ac[0].lat = 51.3;
  s.note(r, 1800000120, 121000);
  CHECK(s.e[0].min_km < 1.5f && s.e[0].last == 1800000120);    // going away again does not undo the closest pass
  r.ac[1].on_ground = true; r.count = 2;
  // a crowd arrives: the list stays its size and the stalest entries go
  for (int i = 0; i < 40; i++) {
    char hex[16]; snprintf(hex, sizeof hex, "b%05d", i);   // (room for any int; plane() keeps the first six)
    r.count = 1; r.ac[0] = plane(hex, "CROWD", 51.1, -1.0);
    s.note(r, 1800000200 + i, 200000 + i * 1000);
  }
  CHECK(s.n == SIGHTINGS);
  for (int i = 0; i < s.n; i++) CHECK(strncmp(s.e[i].hex, "aaa", 3) != 0);
}

TEST(board_picks_the_busiest_airport_unless_told) {
  PageData d;
  char ap[5];
  boardAirport(d, ap);
  CHECK(ap[0] == 0);
  Radar &r = *freshRadar();
  r.count = 3;
  r.ac[0] = plane("aaa001", "ONE1", 51.1, -1.0); snprintf(r.ac[0].origin, 5, "LHR"); snprintf(r.ac[0].dest, 5, "JFK");
  r.ac[1] = plane("aaa002", "TWO2", 51.1, -1.1); snprintf(r.ac[1].origin, 5, "SIN"); snprintf(r.ac[1].dest, 5, "LHR");
  r.ac[2] = plane("aaa003", "TRE3", 51.1, -1.2); snprintf(r.ac[2].origin, 5, "EDI"); snprintf(r.ac[2].dest, 5, "LGW");
  d.seen.note(r, 1800000000, 1000);
  boardAirport(d, ap);
  CHECK(strcmp(ap, "LHR") == 0);
  snprintf(d.airport, sizeof d.airport, "LGW");
  boardAirport(d, ap);
  CHECK(strcmp(ap, "LGW") == 0);
}

TEST(every_page_draws_the_same_in_strips_full_or_empty) {
  Radar &r = *freshRadar();
  static PageData d;
  d = PageData();
  r.pages = &d; r.clock_min = 845;
  for (int pass = 0; pass < 2; pass++) {
    for (int p = 1; p < (int)Page::Count; p++) {
      r.page = (uint8_t)p;
      CHECK(sameInStrips(r));
      CHECK(litPixels(r) > 1500);
    }
    // second time round with something to show, and a notice over the top
    parseWeather(WX, strlen(WX), d.wx); d.wx_wanted = true;
    r.count = 2; r.ac[0] = plane("aaa001", "ONE1", 51.005, -1.0); r.ac[1] = plane("aaa002", "TWO2", 51.01, -1.01);
    snprintf(r.ac[0].origin, 5, "LHR"); snprintf(r.ac[0].dest, 5, "JFK"); snprintf(r.ac[1].origin, 5, "SIN"); snprintf(r.ac[1].dest, 5, "LHR");
    d.seen.note(r, 1800000000, 1000);
    snprintf(r.alert, sizeof r.alert, "7700 MAYDAY1"); snprintf(r.alert2, sizeof r.alert2, "900 km away: press to look");
  }
  r.page = 0; r.alert[0] = 0;
  CHECK(sameInStrips(r));                                       // and the radar page is still itself
}

TEST(visitors_are_announced_once) {
  Radar &r = *freshRadar();
  Visitors v;
  r.count = 3;
  r.ac[0] = plane("aaa001", "ORDNRY", 51.05, -1.0); snprintf(r.ac[0].type, 5, "A320");
  r.ac[1] = plane("aaa002", "ASCOT1", 51.0, -0.9); r.ac[1].shape = Shape::Military; snprintf(r.ac[1].type, 5, "A400");
  r.ac[2] = plane("aaa003", "BIGJET", 50.95, -1.0); snprintf(r.ac[2].type, 5, "A388");
  v.check(r, "B744,A388", false, 1000);
  CHECK(strncmp(v.line1, "MILITARY ASCOT1", 15) == 0 && strstr(v.line2, "km E") != nullptr && r.selected == 1);
  v.tick(r, 1100, true);
  CHECK(strncmp(r.alert, "MILITARY", 8) == 0 && r.alert_style == 1);
  r.alert[0] = 0;
  v.check(r, "B744,A388", false, 6000);                         // the same sky again: nothing new
  CHECK(strncmp(v.line1, "MILITARY", 8) == 0);
  v.tick(r, 40000, true);
  CHECK(r.alert[0] == 0 && v.until == 0);                       // the notice has run its time
  Aircraft late = plane("aaa004", "JUMBO", 51.02, -1.0); snprintf(late.type, 5, "B744");
  r.ac[0] = late;
  v.check(r, "B744,A388", false, 41000);
  CHECK(strncmp(v.line1, "B744 JUMBO", 10) == 0 && r.selected == 0);
  // an emergency keeps the space; switching the notice off drops it
  snprintf(r.alert, sizeof r.alert, "7700 X"); r.alert_style = 0;
  v.tick(r, 41100, true);
  CHECK(strcmp(r.alert, "7700 X") == 0 && r.alert_style == 0);
  r.alert[0] = 0;
  v.tick(r, 41200, false);
  CHECK(r.alert[0] == 0 && v.until == 0);
  CHECK(typeListed("A388,B744", "B744") && !typeListed("A388,B744", "B74") && !typeListed("A388", "") && !typeListed("", "A388"));
  // helicopters only when asked, and nothing out of view
  Radar &q = *freshRadar();
  Visitors h;
  q.count = 2; q.ac[0] = plane("ccc001", "HELI1", 51.02, -1.0); q.ac[0].shape = Shape::Heli;
  q.ac[1] = plane("ccc002", "FARMIL", 53.0, -1.0); q.ac[1].shape = Shape::Military;
  h.check(q, "", false, 1000);
  CHECK(h.until == 0);
  h.check(q, "", true, 2000);
  CHECK(strncmp(h.line1, "HELICOPTER HELI1", 16) == 0);
}

TEST(range_follows_the_nearest_aircraft) {
  Radar &r = *freshRadar();
  r.set.auto_range = true; r.range_km = 10;
  CHECK(r.feedRangeKm() == AUTO_RANGE_MAX_KM);                  // it has to look further than it shows
  Aircraft a = plane("aaa001", "ONE1", 51.27, -1.0);            // 30 km north
  r.update(&a, 1, 1000);
  CHECK(r.range_km == 40);
  a = plane("aaa001", "ONE1", 51.25, -1.0);
  r.update(&a, 1, 5000);
  CHECK(r.range_km == 40);
  a = plane("aaa001", "ONE1", 51.05, -1.0);                     // 5.5 km: but not twice within fifteen seconds
  r.update(&a, 1, 9000);
  CHECK(r.range_km == 40);
  a = plane("aaa001", "ONE1", 51.05, -1.0);
  r.update(&a, 1, 20000);
  CHECK(r.range_km == 10);
  a = plane("aaa001", "ONE1", 51.075, -1.0);                    // 8.3 km: inside 10, so it stays (no flapping at the edge of 0.7)
  r.update(&a, 1, 40000);
  CHECK(r.range_km == 10);
  a = plane("aaa001", "ONE1", 53.0, -1.0);                      // far beyond: as wide as it goes, and no wider
  r.update(&a, 1, 60000);
  CHECK(r.range_km == AUTO_RANGE_MAX_KM);
  r.update(&a, 0, 90000);                                       // empty sky: stay put
  CHECK(r.range_km == AUTO_RANGE_MAX_KM);
  r.set.auto_range = false; r.range_km = 10;
  CHECK(r.feedRangeKm() == 10);
  a = plane("aaa001", "ONE1", 51.27, -1.0);
  r.update(&a, 1, 120000);
  CHECK(r.range_km == 10);
}

TEST(button_tells_one_press_from_two_from_a_hold) {
  Button b;
  uint32_t t = 1000;
  auto run = [&](bool down, uint32_t ms) { Press last = Press::None; for (uint32_t end = t + ms; t < end; t += 5) { Press p = b.feed(down, t); if (p != Press::None) last = p; } return last; };
  CHECK(run(false, 500) == Press::None);
  CHECK(run(true, 100) == Press::None);
  CHECK(run(false, 600) == Press::Short);
  run(true, 100); run(false, 150); run(true, 100);
  CHECK(run(false, 600) == Press::Double);
  CHECK(run(true, 900) == Press::Long);
  CHECK(b.heldMs(t) > 800);
  CHECK(run(false, 600) == Press::None);                        // letting go after a hold is not another press
  for (int i = 0; i < 5; i++) { run(true, 80); run(false, 120); }
  CHECK(run(false, 600) == Press::Reset);
  for (int i = 0; i < 3; i++) { run(true, 80); run(false, 120); }
  CHECK(run(false, 600) == Press::None);                        // three or four mean nothing
  // contact bounce is not a press
  for (int i = 0; i < 6; i++) { b.feed(i % 2 == 0, t); t += 4; }
  CHECK(run(false, 600) == Press::None);
}

TEST(presses_do_the_right_thing_on_each_page) {
  Radar &r = *freshRadar();
  Watch w;
  r.count = 2; r.ac[0] = plane("aaa001", "ONE1", 51.02, -1.0); r.ac[1] = plane("aaa002", "TWO2", 51.03, -1.0);
  r.set.auto_cycle = false;
  doPress(Press::Short, r, w, 100);
  CHECK(r.selected == 0);
  doPress(Press::Double, r, w, 200);
  CHECK(r.page == (uint8_t)Page::Weather);
  doPress(Press::Long, r, w, 300);
  CHECK(r.range_km == 20);                                      // a hold only changes range on the radar page
  doPress(Press::Short, r, w, 400);
  CHECK(r.page == 0 && r.selected == 0);                        // back to the scope, without also moving the highlight
  doPress(Press::Long, r, w, 500);
  CHECK(r.range_km == 40);
  // an emergency on offer: one press from any page goes to it
  setPage(r, Page::Log, 600);
  Aircraft far = plane("e00001", "MAYDAY1", -33.9, 151.2); snprintf(far.squawk, 5, "7700");
  w.found(&far, 1);
  doPress(Press::Short, r, w, 700);
  CHECK(w.away && r.page == 0 && r.range_km == 40);
  doPress(Press::Long, r, w, 800);
  CHECK(r.range_km == 40);
  doPress(Press::Short, r, w, 900);
  CHECK(!w.away && r.home_lat == 51.0);
}

TEST(page_timer_moves_on_and_waits_for_an_emergency) {
  Radar &r = *freshRadar();
  Watch w;
  pageTick(r, w, 0, 50000);
  CHECK(r.page == 0);
  pageTick(r, w, 10, 55000);
  CHECK(r.page == 0);
  pageTick(r, w, 10, 60000);
  CHECK(r.page == (uint8_t)Page::Weather);
  pageTick(r, w, 10, 65000);
  CHECK(r.page == (uint8_t)Page::Weather);
  Aircraft far = plane("e00001", "MAYDAY1", 10, 10);
  w.found(&far, 1);
  pageTick(r, w, 10, 90000);
  CHECK(r.page == (uint8_t)Page::Weather);                      // held while there is something to answer
}

TEST(one_line_controls) {
  Radar &r = *freshRadar();
  Watch w;
  char j[96];
  CHECK(command("page", "weather", r, w, 10, j, sizeof j) == Cmd::Done && r.page == (uint8_t)Page::Weather);
  CHECK(command("page", "next", r, w, 10, j, sizeof j) == Cmd::Done && r.page == (uint8_t)Page::Board);
  CHECK(command("page", "kitchen", r, w, 10, j, sizeof j) == Cmd::Bad);
  CHECK(command("page", "radar", r, w, 10, j, sizeof j) == Cmd::Done && r.page == 0);
  CHECK(command("range", "next", r, w, 10, j, sizeof j) == Cmd::Done && r.range_km == 40);
  CHECK(command("range", "80", r, w, 10, j, sizeof j) == Cmd::Settings && strcmp(j, "{\"range_km\":80}") == 0);
  CHECK(command("map", "toggle", r, w, 10, j, sizeof j) == Cmd::Settings && strcmp(j, "{\"map\":false}") == 0);
  CHECK(command("clock", "on", r, w, 10, j, sizeof j) == Cmd::Settings && strcmp(j, "{\"clock\":true}") == 0);
  CHECK(command("clock", "maybe", r, w, 10, j, sizeof j) == Cmd::Bad);
  CHECK(command("brightness", "40", r, w, 10, j, sizeof j) == Cmd::Settings && strcmp(j, "{\"brightness\":40}") == 0);
  CHECK(command("brightness", "40}", r, w, 10, j, sizeof j) == Cmd::Bad && command("brightness", "nan", r, w, 10, j, sizeof j) == Cmd::Bad);
  CHECK(command("brightness", "0x10", r, w, 10, j, sizeof j) == Cmd::Bad && command("brightness", "", r, w, 10, j, sizeof j) == Cmd::Bad);
  CHECK(command("font", "vt323", r, w, 10, j, sizeof j) == Cmd::Settings && strcmp(j, "{\"font\":\"vt323\"}") == 0);
  CHECK(command("font", "a\"b", r, w, 10, j, sizeof j) == Cmd::Bad);
  r.count = 1; r.ac[0] = plane("aaa001", "ONE1", 51.02, -1.0);
  CHECK(command("select", "ONE1", r, w, 10, j, sizeof j) == Cmd::Done && r.selected == 0);
  CHECK(command("select", "NOBODY", r, w, 10, j, sizeof j) == Cmd::Bad);
  CHECK(command("press", "double", r, w, 10, j, sizeof j) == Cmd::Done && r.page == (uint8_t)Page::Weather);
  CHECK(command("press", "hard", r, w, 10, j, sizeof j) == Cmd::Bad);
  // where the radar is, where its data comes from and the key itself are not for one-line requests
  const char *no[] = {"home_lat", "home_lon", "feed_url", "map_url", "logo_url", "api_key", "verify_certs", "live", "", "nonsense"};
  for (const char *name : no) CHECK(command(name, "1", r, w, 10, j, sizeof j) == Cmd::Unknown);
  // every settings change it writes is something the settings reader takes
  Config c;
  command("night", "2", r, w, 10, j, sizeof j);
  CHECK(configFromJson(j, strlen(j), c) && c.night == 2);
}

TEST(alert_controls_look_and_come_home) {
  Radar &r = *freshRadar();
  Watch w;
  char j[96];
  CHECK(command("alert", "look", r, w, 10, j, sizeof j) == Cmd::Done && !w.away);     // nothing on offer: nothing happens
  Aircraft far = plane("e00001", "MAYDAY1", 10, 10);
  w.found(&far, 1);
  setPage(r, Page::Weather, 5);
  CHECK(command("alert", "look", r, w, 20, j, sizeof j) == Cmd::Done && w.away && r.page == 0);
  CHECK(command("alert", "look", r, w, 30, j, sizeof j) == Cmd::Done && w.away);      // asking twice does not bounce it home
  CHECK(command("alert", "home", r, w, 40, j, sizeof j) == Cmd::Done && !w.away);
  CHECK(command("alert", "dance", r, w, 50, j, sizeof j) == Cmd::Bad);
}

TEST(control_key) {
  CHECK(keyOk("", nullptr) && keyOk("", "anything") && keyOk(nullptr, nullptr));
  CHECK(keyOk("hawkeye", "hawkeye"));
  CHECK(!keyOk("hawkeye", nullptr) && !keyOk("hawkeye", "") && !keyOk("hawkeye", "hawkey") && !keyOk("hawkeye", "hawkeyes") && !keyOk("hawkeye", "Hawkeye"));
}

TEST(new_settings_round_trip_and_the_key_stays_home) {
  Config c;
  CHECK(c.night == 0 && c.units == Units::MetricKmh && c.page_cycle_s == 0 && c.api_key[0] == 0 && c.visitor_types[0] && c.set.visitors && !c.set.auto_range);
  const char *j = "{\"night\":2,\"night_brightness\":8,\"night_start_min\":1380,\"night_end_min\":390,\"units\":3,\"page_cycle_s\":2,\"overhead_km\":5.5,"
                  "\"board_airport\":\"man\",\"visitor_types\":\"a388, c17\",\"api_key\":\"Radar-4077\",\"auto_range\":true,\"page_log\":false}";
  CHECK(configFromJson(j, strlen(j), c));
  CHECK(c.night == 2 && c.night_brightness == 8 && c.night_start_min == 1380 && c.night_end_min == 390 && c.units == Units::Imperial);
  CHECK(c.page_cycle_s == 5 && c.overhead_km == 5.5f && strcmp(c.board_airport, "MAN") == 0 && strcmp(c.visitor_types, "A388,C17") == 0);
  CHECK(strcmp(c.api_key, "Radar-4077") == 0 && c.set.auto_range && !c.set.page_log);
  char page[CONFIG_JSON_MAX], saved[CONFIG_JSON_MAX];
  CHECK(configToJson(c, page, sizeof page) > 0 && configToJson(c, saved, sizeof saved, true) > 0);
  CHECK(strstr(page, "Radar-4077") == nullptr && strstr(page, "\"api_key_set\":true") != nullptr);   // never sent back to a page
  CHECK(strstr(saved, "\"api_key\":\"Radar-4077\"") != nullptr);
  Config d;
  CHECK(configFromJson(saved, strlen(saved), d));
  CHECK(strcmp(d.api_key, "Radar-4077") == 0 && d.night == 2 && d.units == Units::Imperial && strcmp(d.board_airport, "MAN") == 0);
  // what a page got back does not wipe the key when it is posted again
  CHECK(configFromJson(page, strlen(page), d) && strcmp(d.api_key, "Radar-4077") == 0);
  // bad values are ignored, an empty key removes it
  const char *bad = "{\"api_key\":\"has space\",\"board_airport\":\"M/N\",\"visitor_types\":\"A388;DROP\",\"night\":9,\"units\":77}";
  configFromJson(bad, strlen(bad), d);
  CHECK(strcmp(d.api_key, "Radar-4077") == 0 && strcmp(d.board_airport, "MAN") == 0 && strcmp(d.visitor_types, "A388,C17") == 0 && d.night == 2 && d.units == Units::Imperial);
  const char *off = "{\"api_key\":\"\",\"board_airport\":\"\"}";
  configFromJson(off, strlen(off), d);
  CHECK(d.api_key[0] == 0 && d.board_airport[0] == 0);
  // the fullest configuration still fits the buffer
  Config big;
  memset(big.feed_url, 'a', sizeof big.feed_url - 1); big.feed_url[sizeof big.feed_url - 1] = 0;
  memset(big.map_url, 'a', sizeof big.map_url - 1); big.map_url[sizeof big.map_url - 1] = 0;
  memset(big.logo_url, 'a', sizeof big.logo_url - 1); big.logo_url[sizeof big.logo_url - 1] = 0;
  memset(big.visitor_types, 'A', sizeof big.visitor_types - 1); big.visitor_types[sizeof big.visitor_types - 1] = 0;
  memset(big.api_key, 'k', sizeof big.api_key - 1); big.api_key[sizeof big.api_key - 1] = 0;
  memset(big.map_credit, '"', sizeof big.map_credit - 1); big.map_credit[sizeof big.map_credit - 1] = 0;
  CHECK(configToJson(big, saved, sizeof saved, true) > 0);
}
