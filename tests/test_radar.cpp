// RadarMASH - open firmware for the GeekMagic Radar 360.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../core/radar.h"
#include "test.h"
#include <math.h>
#include <stdlib.h>
#include <memory>
#include <stdio.h>
#include <string.h>
#include <vector>

using namespace rm;

namespace {
const double KM_PER_DEG_LAT = 110.57;
double kmPerDegLon(const Radar &r) { return 111.32 * cos(r.home_lat * 3.14159265358979 / 180.0); }

// A radar with home at a round-number spot and nothing switched on that moves by itself.
std::unique_ptr<Radar> blankRadar() {
  std::unique_ptr<Radar> r(new Radar());
  r->home_lat = 50; r->home_lon = 0; r->range_km = 40;
  r->set.auto_cycle = false;
  return r;
}
void place(Radar &r, int i, const char *callsign, double east_km, double north_km) {
  Aircraft &a = r.ac[i];
  snprintf(a.callsign, sizeof a.callsign, "%s", callsign);
  a.lat = r.home_lat + north_km / KM_PER_DEG_LAT;
  a.lon = r.home_lon + east_km / kmPerDegLon(r);
  if (i >= r.count) r.count = i + 1;
}

typedef std::vector<uint16_t> Pixels;
Pixels renderWhole(const Radar &r) {
  Pixels px((size_t)SCREEN_W * SCREEN_H, 0xDEAD);
  Canvas c(px.data(), 0, SCREEN_H);
  r.draw(c);
  return px;
}
// Draw the way the device does: one strip at a time into a small buffer.
Pixels renderInBands(const Radar &r, int band) {
  Pixels px((size_t)SCREEN_W * SCREEN_H, 0xDEAD), strip((size_t)SCREEN_W * band);
  for (int y0 = 0; y0 < SCREEN_H; y0 += band) {
    int rows = y0 + band > SCREEN_H ? SCREEN_H - y0 : band;
    Canvas c(strip.data(), y0, rows);
    r.draw(c);
    memcpy(px.data() + (size_t)y0 * SCREEN_W, strip.data(), (size_t)SCREEN_W * rows * 2);
  }
  return px;
}
}  // namespace

// ---- projection ----
TEST(home_is_the_centre_of_the_screen) {
  auto r = blankRadar();
  float x, y;
  r->toScreen(r->home_lat, r->home_lon, x, y);
  CHECK_NEAR(x, CENTRE, 0.01); CHECK_NEAR(y, CENTRE, 0.01);
}

TEST(north_is_up_and_east_is_right) {
  auto r = blankRadar();
  float x, y;
  place(*r, 0, "N", 0, 20);
  CHECK(r->project(r->ac[0], x, y));
  CHECK_NEAR(x, CENTRE, 0.5); CHECK_NEAR(y, CENTRE - RADAR_RADIUS / 2, 0.5);     // 20 km of a 40 km range
  place(*r, 0, "E", 20, 0);
  CHECK(r->project(r->ac[0], x, y));
  CHECK_NEAR(x, CENTRE + RADAR_RADIUS / 2, 0.5); CHECK_NEAR(y, CENTRE, 0.5);
}

TEST(range_scales_the_picture) {
  auto r = blankRadar();
  float x40, x80, y;
  place(*r, 0, "E", 20, 0);
  r->project(r->ac[0], x40, y);
  r->range_km = 80;
  r->project(r->ac[0], x80, y);
  CHECK_NEAR(x80 - CENTRE, (x40 - CENTRE) / 2, 0.5);
}

TEST(aircraft_beyond_range_are_not_in_view) {
  auto r = blankRadar();
  float x, y;
  place(*r, 0, "FAR", 45, 0);
  CHECK(!r->project(r->ac[0], x, y));
  place(*r, 0, "NEAR", 30, 0);
  CHECK(r->project(r->ac[0], x, y));
}

// ---- the button ----
TEST(long_press_cycles_range) {
  auto r = blankRadar();
  r->selected = 0;
  r->longPress(); CHECK_NEAR(r->range_km, 80, 0.01); CHECK(r->selected == -1);
  r->longPress(); CHECK_NEAR(r->range_km, 160, 0.01);
  r->longPress(); CHECK_NEAR(r->range_km, 10, 0.01);    // wraps round to the closest view
  r->longPress(); CHECK_NEAR(r->range_km, 20, 0.01);
  r->longPress(); CHECK_NEAR(r->range_km, 40, 0.01);
  r->range_km = 33;                                     // an odd value from saved settings still steps sensibly
  r->longPress(); CHECK_NEAR(r->range_km, 40, 0.01);
}

TEST(short_press_steps_through_aircraft_in_view_then_none) {
  auto r = blankRadar();
  place(*r, 0, "A", 5, 5); place(*r, 1, "FAR", 500, 0); place(*r, 2, "C", -5, 5);
  r->shortPress(); CHECK(r->selected == 0);
  r->shortPress(); CHECK(r->selected == 2);       // skips the one out of view
  r->shortPress(); CHECK(r->selected == -1);
  r->shortPress(); CHECK(r->selected == 0);
}

TEST(short_press_wraps_when_auto_cycling) {
  auto r = blankRadar();
  r->set.auto_cycle = true;
  place(*r, 0, "A", 5, 5); place(*r, 1, "B", -5, 5);
  r->tick(0); CHECK(r->selected == 0);
  r->shortPress(); CHECK(r->selected == 1);
  r->shortPress(); CHECK(r->selected == 0);
}

TEST(short_press_with_no_aircraft_is_harmless) {
  auto r = blankRadar();
  r->shortPress(); CHECK(r->selected == -1);
  r->set.auto_cycle = true;
  r->tick(0); r->shortPress(); CHECK(r->selected == -1);
}

// ---- time ----
TEST(sweep_turns_once_every_four_seconds) {
  auto r = blankRadar();
  r->tick(0); CHECK_NEAR(r->sweep_deg, 0, 0.01);
  r->tick(1000); CHECK_NEAR(r->sweep_deg, 90, 0.01);
  r->tick(7000); CHECK_NEAR(r->sweep_deg, 270, 0.01);
}

TEST(sweep_speed_can_be_set) {
  auto r = blankRadar();
  r->set.sweep_seconds = 10;
  r->tick(2500); CHECK_NEAR(r->sweep_deg, 90, 0.01);
  r->tick(12500); CHECK_NEAR(r->sweep_deg, 90, 0.01);
  r->set.sweep_seconds = 2;
  r->tick(500); CHECK_NEAR(r->sweep_deg, 90, 0.01);
  r->set.sweep_seconds = 0;                         // nonsense from a damaged settings file must not divide by zero
  r->tick(250); CHECK(r->sweep_deg >= 0 && r->sweep_deg < 360);
  r->set.sweep_seconds = 30;
  r->tick(4000000000u); CHECK(r->sweep_deg >= 0 && r->sweep_deg < 360);   // late in the 49-day millisecond clock
}

TEST(auto_cycle_dwells_then_moves_on) {
  auto r = blankRadar();
  r->set.auto_cycle = true;
  place(*r, 0, "A", 5, 5); place(*r, 1, "B", -5, 5);
  r->tick(100); CHECK(r->selected == 0);
  r->tick(5099); CHECK(r->selected == 0);
  r->tick(5100); CHECK(r->selected == 1);
  r->tick(10100); CHECK(r->selected == 0);
}

TEST(a_press_restarts_the_dwell) {
  auto r = blankRadar();
  r->set.auto_cycle = true;
  place(*r, 0, "A", 5, 5); place(*r, 1, "B", -5, 5);
  r->tick(0);
  r->tick(4000); r->shortPress(); CHECK(r->selected == 1);
  r->tick(8999); CHECK(r->selected == 1);          // would have moved at 5000 without the press
  r->tick(9000); CHECK(r->selected == 0);
}

TEST(highlight_drops_when_the_aircraft_leaves) {
  auto r = blankRadar();
  place(*r, 0, "A", 5, 5);
  r->shortPress(); CHECK(r->selected == 0);
  place(*r, 0, "A", 500, 5);                       // flew away
  r->tick(1); CHECK(r->selected == -1);
  place(*r, 0, "A", 5, 5); r->shortPress();
  r->count = 0;                                    // feed came back empty
  r->tick(2); CHECK(r->selected == -1);
}

// ---- demo traffic ----
TEST(demo_traffic_is_well_formed) {
  auto r = blankRadar();
  for (uint32_t t = 0; t < 120000; t += 733) {
    demoTraffic(*r, t);
    CHECK(r->count > 0 && r->count <= MAX_AIRCRAFT);
    for (int i = 0; i < r->count; i++) {
      const Aircraft &a = r->ac[i];
      CHECK(a.callsign[0] != 0 && strlen(a.callsign) < sizeof a.callsign);
      CHECK(a.track_deg >= 0 && a.track_deg < 360);
      CHECK(a.trail_len >= 0 && a.trail_len <= TRAIL_POINTS);
      CHECK(a.shape != Shape::Unknown);
    }
  }
}

TEST(demo_trails_follow_the_path_without_jumps) {
  // Each trail point should be one step back along the path, even through a turn.
  auto r = blankRadar();
  for (uint32_t t = 500; t < 90000; t += 977) {
    demoTraffic(*r, t);
    const double kx = kmPerDegLon(*r);
    for (int i = 0; i < r->count; i++) {
      const Aircraft &a = r->ac[i];
      double px = a.lon, py = a.lat;
      for (int k = 0; k < a.trail_len; k++) {
        double d = hypot((a.trail_lon[k] - px) * kx, (a.trail_lat[k] - py) * KM_PER_DEG_LAT);
        CHECK(d > 0.5 && d < 1.3);                 // 1.2 km along the path; a chord across a turn is a little shorter
        px = a.trail_lon[k]; py = a.trail_lat[k];
      }
    }
  }
}

TEST(demo_aircraft_point_the_way_they_move) {
  auto r = blankRadar();
  auto later = blankRadar();
  for (uint32_t t = 1000; t < 60000; t += 1300) {
    demoTraffic(*r, t); demoTraffic(*later, t + 40);
    const double kx = kmPerDegLon(*r);
    for (int i = 0; i < r->count; i++) {
      double de = (later->ac[i].lon - r->ac[i].lon) * kx, dn = (later->ac[i].lat - r->ac[i].lat) * KM_PER_DEG_LAT;
      if (hypot(de, dn) > 5 || hypot(de, dn) < 1e-4) continue;      // wrapped round to its start, or float noise
      double moving = fmod(atan2(de, dn) * 180 / 3.14159265358979 + 360, 360);
      double diff = fabs(moving - r->ac[i].track_deg);
      if (diff > 180) diff = 360 - diff;
      CHECK(diff < 12);
    }
  }
}

TEST(aircraft_fade_until_the_sweep_returns) {
  // Just after the sweep has passed an aircraft it is at its brightest; just before the sweep arrives, its dimmest.
  auto r = blankRadar();
  place(*r, 0, "E", 20, 0);                        // due east: bearing 90
  r->ac[0].shape = Shape::Narrow;
  r->set.trails = false; r->set.labels = false;
  auto brightest = [&](uint32_t t) {
    r->tick(t);
    Pixels px = renderWhole(*r);
    float x, y; r->project(r->ac[0], x, y);
    int best = 0;
    for (int dy = -3; dy <= 3; dy++)
      for (int dx = -3; dx <= 3; dx++) { int red = px[(size_t)((int)y + dy) * SCREEN_W + (int)x + dx] >> 11; if (red > best) best = red; }
    return best;
  };
  int just_passed = brightest(1100), half_way = brightest(3000), about_to_arrive = brightest(4900);   // sweep at 99, 270, 81 degrees
  CHECK(just_passed > half_way);
  CHECK(half_way > about_to_arrive);
  r->set.fade = false;
  CHECK(brightest(4900) == brightest(1100));
}

// ---- drawing ----
TEST(drawing_in_strips_matches_drawing_whole) {
  // The device has no room for a full frame and draws 40 rows at a time. The result must be identical.
  auto r = blankRadar();
  const uint32_t times[] = {0, 2600, 6500, 9000, 13000, 21000};
  for (uint32_t t : times)
    for (int mode = 0; mode < 4; mode++) {
      demoTraffic(*r, t); r->tick(t);
      r->selected = mode == 0 ? -1 : (int)(t / 1000 + mode) % r->count;
      r->set.tag = mode != 2; r->set.card = mode >= 2;
      Pixels whole = renderWhole(*r);
      CHECK(whole == renderInBands(*r, 40));
      CHECK(whole == renderInBands(*r, 7));        // an awkward size that does not divide 360
    }
}

TEST(draw_writes_every_pixel) {
  auto r = blankRadar();
  demoTraffic(*r, 3000); r->tick(3000);
  Pixels px = renderWhole(*r);                     // buffer starts as 0xDEAD
  int untouched = 0;
  for (uint16_t p : px) untouched += p == 0xDEAD;
  CHECK(untouched == 0);
}

TEST(nothing_is_drawn_outside_the_glass) {
  auto r = blankRadar();
  demoTraffic(*r, 3000); r->tick(3000);            // nothing highlighted, so no tag or card
  Pixels px = renderWhole(*r);
  int stray = 0, lit = 0;
  for (int y = 0; y < SCREEN_H; y++)
    for (int x = 0; x < SCREEN_W; x++) {
      uint16_t p = px[(size_t)y * SCREEN_W + x];
      if (hypot(x - CENTRE, y - CENTRE) > RADAR_RADIUS + 2) stray += p != 0; else lit += p != 0;
    }
  CHECK(stray == 0);
  CHECK(lit > 50000);
}

TEST(every_switch_changes_the_picture) {
  auto r = blankRadar();
  demoTraffic(*r, 9000); r->tick(9000);
  r->selected = 0;
  r->set.card = true;                              // so the card switch has something to remove
  const Pixels base = renderWhole(*r);
  bool Settings::*const switches[] = {&Settings::sweep, &Settings::rings, &Settings::labels, &Settings::altitude, &Settings::trails, &Settings::fade,
                                      &Settings::shapes, &Settings::tag, &Settings::tag_route, &Settings::tag_type,
                                      &Settings::tag_level, &Settings::tag_speed, &Settings::card};
  for (auto sw : switches) {
    r->set.*sw = !(r->set.*sw);
    CHECK(renderWhole(*r) != base);
    r->set.*sw = !(r->set.*sw);
  }
  CHECK(renderWhole(*r) == base);
}

TEST(radar_draws_the_same_in_strips_with_every_font) {
  auto r = blankRadar();
  demoTraffic(*r, 9000); r->tick(9000);
  r->set.card = true;
  Pixels classic;
  for (int f = 0; f < FONT_COUNT; f++)
    for (int sel = -1; sel < 2; sel++) {
      r->set.font = (uint8_t)f; r->selected = sel;
      Pixels whole = renderWhole(*r);
      CHECK(whole == renderInBands(*r, 40));
      CHECK(whole == renderInBands(*r, 7));
      if (f == 0 && sel == 0) classic = whole;
      if (f > 0 && sel == 0) CHECK(whole != classic);          // the font really changed the picture
    }
  setFont(0);
}

TEST(cached_scope_matches_the_slow_way) {
  // The cache is a speed-up, not a different picture: every pixel must be within a shade of the uncached result.
  auto r = blankRadar();
  std::unique_ptr<ScopeCache> cache(new ScopeCache());
  const uint32_t times[] = {0, 900, 2600, 3999};
  for (uint32_t t : times)
    for (int rings = 0; rings < 2; rings++) {
      r->set.rings = rings == 1;
      demoTraffic(*r, t); r->tick(t);
      r->cache = nullptr;
      Pixels slow = renderWhole(*r);
      r->cache = cache.get();
      r->prepare();
      Pixels fast = renderWhole(*r);
      int worst = 0;
      for (size_t i = 0; i < slow.size(); i++) {
        int dr = abs((slow[i] >> 11) - (fast[i] >> 11)), dg = abs(((slow[i] >> 5) & 63) - ((fast[i] >> 5) & 63)), db = abs((slow[i] & 31) - (fast[i] & 31));
        int x = (int)(i % SCREEN_W), y = (int)(i / SCREEN_W);
        if (hypot(x - CENTRE, y - CENTRE) < 6) continue;
        double bearing = fmod(atan2(x - CENTRE, -(y - CENTRE)) * 180 / 3.14159265358979 + 360, 360);
        double off = fabs(bearing - r->sweep_deg); if (off > 180) off = 360 - off;
        if (off < 1.5) continue;                          // the bright leading edge lands within a degree either way
        int d = dr > db ? dr : db; d = d > (dg + 1) / 2 ? d : (dg + 1) / 2;
        if (d > worst) worst = d;
      }
      CHECK(worst <= 3);
      CHECK(fast == renderInBands(*r, 40));             // and strips still match whole
      CHECK(fast == renderInBands(*r, 7));
    }
}

TEST(cache_is_rebuilt_when_the_background_changes) {
  auto r = blankRadar();
  std::unique_ptr<ScopeCache> cache(new ScopeCache());
  r->cache = cache.get();
  demoTraffic(*r, 3000); r->tick(3000);
  r->prepare();
  CHECK(cache->valid);
  Pixels with_rings = renderWhole(*r);
  r->set.rings = false;                               // changed, but prepare() not yet called: must not show a stale background
  Pixels stale_guard = renderWhole(*r);
  r->cache = nullptr;
  CHECK(stale_guard == renderWhole(*r));              // it fell back to the slow way
  r->cache = cache.get();
  uint32_t before = cache->key;
  r->prepare();
  CHECK(cache->key != before);
  CHECK(renderWhole(*r) != with_rings);
  r->prepare();                                       // nothing changed: key stays
  uint32_t same = cache->key;
  r->set.sweep = false; r->set.labels = false;        // things that are not part of the background do not rebuild it
  r->prepare();
  CHECK(cache->key == same);
  auto none = blankRadar();
  none->prepare();                                    // no cache at all: harmless
}

TEST(switches_can_be_found_by_name) {
  Settings s;
  CHECK(TOGGLE_COUNT >= 15);
  for (int i = 0; i < TOGGLE_COUNT; i++) {
    bool *f = toggleByName(s, TOGGLES[i].name);
    CHECK(f == &(s.*TOGGLES[i].field));
    for (int k = 0; k < i; k++) CHECK(strcmp(TOGGLES[i].name, TOGGLES[k].name) != 0);   // names are unique
  }
  CHECK(toggleByName(s, "no_such_switch") == nullptr && toggleByName(s, "") == nullptr);
}

TEST(awkward_aircraft_data_does_not_break_drawing) {
  auto r = blankRadar();
  place(*r, 0, "EDGE", 39.2, 0); place(*r, 1, "TOP", 0, 39.2); place(*r, 2, "BOT", 0, -39.2); place(*r, 3, "LEFT", -39.2, 0);
  place(*r, 4, "CENTRE", 0, 0);
  for (int i = 0; i < r->count; i++) {
    Aircraft &a = r->ac[i];
    memset(a.airline, 'W', sizeof a.airline - 1); a.airline[sizeof a.airline - 1] = 0;   // the longest and widest name that fits
    a.alt_ft = i == 0 ? -500 : 99900; a.track_deg = 359.9f; a.speed_kt = 9999;
    a.shape = (Shape)(i % 6);
    a.trail_len = TRAIL_POINTS;
    for (int k = 0; k < TRAIL_POINTS; k++) { a.trail_lat[k] = (float)a.lat + k * 5.0f; a.trail_lon[k] = (float)a.lon - k * 9.0f; }
  }
  r->set.card = true;
  for (int sel = -1; sel < r->count; sel++) {
    r->selected = sel;
    CHECK(renderWhole(*r) == renderInBands(*r, 40));
  }
  r->count = MAX_AIRCRAFT;                         // a full list of blank entries
  r->selected = MAX_AIRCRAFT - 1;
  CHECK(renderWhole(*r).size() == (size_t)SCREEN_W * SCREEN_H);
}
