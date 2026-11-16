// RadarMASH - open firmware for the GeekMagic Radar 360.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../core/map.h"
#include "../core/radar.h"
#include "test.h"
#include <memory>
#include <stdio.h>
#include <vector>

using namespace rm;

namespace {
// A made-up tile source. Every pixel's colour says where it is on the world map, so a test can tell which way up
// and which way round the picture came out: red rises to the east, blue rises to the south.
struct FakeTiles : TileSource {
  std::vector<uint16_t> buf = std::vector<uint16_t>(TILE_SIZE * TILE_SIZE);
  int asked = 0, zoom_seen = -1;
  bool none = false;                  // pretend every tile failed to download
  int only_x = -1;                    // if set, only this tile column is available
  uint16_t flat = 0; bool use_flat = false;
  const uint16_t *tile(int zoom, int x, int y) override {
    asked++; zoom_seen = zoom;
    if (none || (only_x >= 0 && x != only_x)) return nullptr;
    for (int py = 0; py < TILE_SIZE; py++)
      for (int px = 0; px < TILE_SIZE; px++)
        buf[py * TILE_SIZE + px] = use_flat ? flat : rgb(((x * TILE_SIZE + px) / 16 % 32) << 3, 0, ((y * TILE_SIZE + py) / 16 % 32) << 3);
    return buf.data();
  }
};
std::unique_ptr<MapImage> compose(FakeTiles &src, float range_km = 40, MapStyle style = MapStyle::AsIs, double lat = 51, double lon = -1) {
  std::unique_ptr<MapImage> m(new MapImage());
  composeMap(*m, src, lat, lon, range_km, style);
  return m;
}
int red(const MapImage &m, int x, int y) { return m.px[y * SCREEN_W + x] >> 11; }
int blue(const MapImage &m, int x, int y) { return m.px[y * SCREEN_W + x] & 31; }
}  // namespace

TEST(mercator_known_points) {
  CHECK_NEAR(mercatorX(0, 0), 128, 1e-6);
  CHECK_NEAR(mercatorY(0, 0), 128, 1e-6);
  CHECK_NEAR(mercatorX(-180, 3), 0, 1e-6);
  CHECK_NEAR(mercatorX(180, 3), 256 * 8, 1e-6);
  // central London is in tile 511/340 at zoom 10
  CHECK((int)(mercatorX(-0.1278, 10) / 256) == 511);
  CHECK((int)(mercatorY(51.5074, 10) / 256) == 340);
  CHECK(mercatorY(60, 5) < mercatorY(50, 5));          // further north is nearer the top
  CHECK(mercatorY(89.9, 2) >= 0 && mercatorY(-89.9, 2) <= 256 * 4);   // the poles are clamped, not infinite
}

TEST(zoom_follows_range) {
  int last = MAX_ZOOM + 1;
  for (float km : RANGES_KM) {
    int z = zoomForRange(51, km);
    CHECK(z >= 0 && z <= MAX_ZOOM);
    CHECK(z < last);                                   // each wider range uses a lower zoom
    last = z;
  }
  CHECK(zoomForRange(51, 40) == 9);
  CHECK(zoomForRange(0, 40) == zoomForRange(51, 40) + 1 || zoomForRange(0, 40) == zoomForRange(51, 40));
  CHECK(zoomForRange(51, 0) == 0);                     // nonsense in, safe answer out
  CHECK(zoomForRange(51, 0.001f) == MAX_ZOOM);
}

TEST(map_is_north_up_and_east_right) {
  FakeTiles src;
  auto m = compose(src);
  CHECK(m->ready && m->missing == 0);
  CHECK(src.zoom_seen == zoomForRange(51, 40));
  // 64 screen pixels is about 77 map pixels here, so the colour (one step per 16 map pixels) must have moved on by 4 or 5
  int east = (red(*m, 244, 180) - red(*m, 180, 180) + 32) % 32, south = (blue(*m, 180, 244) - blue(*m, 180, 180) + 32) % 32;
  CHECK(east >= 3 && east <= 6);
  CHECK(south >= 3 && south <= 6);
  CHECK(red(*m, 180, 100) == red(*m, 180, 260));       // going north or south does not change the east-west colour
  CHECK(blue(*m, 100, 180) == blue(*m, 260, 180));
}

TEST(map_centre_is_home) {
  FakeTiles src;
  auto m = compose(src);
  int z = zoomForRange(51, 40);
  int want_r = ((int)mercatorX(-1, z) / 16) % 32, want_b = ((int)mercatorY(51, z) / 16) % 32;
  CHECK_NEAR(red(*m, 180, 180), want_r, 1);
  CHECK_NEAR(blue(*m, 180, 180), want_b, 1);
}

TEST(map_is_blank_outside_the_glass) {
  FakeTiles src; src.use_flat = true; src.flat = 0xFFFF;
  auto m = compose(src);
  CHECK(m->px[0] == 0 && m->px[SCREEN_W - 1] == 0 && m->px[SCREEN_W * SCREEN_H - 1] == 0);
  CHECK(m->px[180 * SCREEN_W + 180] == 0xFFFF);
  CHECK(m->px[180 * SCREEN_W + 12] == 0xFFFF);         // just inside the left edge
}

TEST(missing_tiles_are_counted_and_left_black) {
  FakeTiles none; none.none = true;
  auto m = compose(none);
  CHECK(m->missing == none.asked && m->missing > 0);
  int lit = 0;
  for (uint16_t p : m->px) lit += p != 0;
  CHECK(lit == 0);

  FakeTiles some; some.use_flat = true; some.flat = 0xFFFF;
  some.only_x = (int)(mercatorX(-1, zoomForRange(51, 40)) / 256);
  auto part = compose(some);
  CHECK(part->missing > 0 && part->missing < some.asked);
  CHECK(part->px[180 * SCREEN_W + 180] == 0xFFFF);
}

TEST(a_sensible_number_of_tiles_is_requested) {
  for (float km : RANGES_KM) {
    FakeTiles src;
    compose(src, km);
    CHECK(src.asked >= 1 && src.asked <= 16);
  }
}

TEST(dark_style_inverts_a_light_map) {
  FakeTiles white; white.use_flat = true; white.flat = 0xFFFF;
  FakeTiles black; black.use_flat = true; black.flat = 0;
  auto land = compose(white, 40, MapStyle::Dark), road = compose(black, 40, MapStyle::Dark);
  int c = 180 * SCREEN_W + 180;
  CHECK(land->px[c] == 0);
  CHECK(road->px[c] != 0);
  auto green = compose(black, 40, MapStyle::Green);
  CHECK(((green->px[c] >> 5) & 63) > 2 * (green->px[c] >> 11));      // mostly green
  auto as_is = compose(white, 40, MapStyle::AsIs);
  CHECK(as_is->px[c] == 0xFFFF);
}

TEST(map_works_anywhere_on_earth) {
  const double spots[][2] = {{0, 0}, {64.1, -21.9}, {-33.9, 151.2}, {35.6, 179.99}, {-17.5, -179.99}, {84.9, 10}, {-84.9, 10}, {89.5, 0}};
  for (auto &s : spots)
    for (float km : {10.0f, 160.0f}) {
      FakeTiles src;
      auto m = compose(src, km, MapStyle::Dark, s[0], s[1]);
      CHECK(m->ready);
      CHECK(src.asked <= 64);
    }
}

TEST(radar_draws_over_the_map_the_same_in_strips) {
  FakeTiles src;
  auto m = compose(src, 40, MapStyle::Dark);
  std::unique_ptr<Radar> r(new Radar());
  r->set.auto_cycle = false;
  demoTraffic(*r, 9000); r->tick(9000);
  std::vector<uint16_t> plain(SCREEN_W * SCREEN_H), mapped(SCREEN_W * SCREEN_H), strips(SCREEN_W * SCREEN_H), strip(SCREEN_W * 40);
  { Canvas c(plain.data(), 0, SCREEN_H); r->draw(c); }
  r->map = m.get();
  snprintf(r->map_credit, sizeof r->map_credit, "(c) Test Maps");
  { Canvas c(mapped.data(), 0, SCREEN_H); r->draw(c); }
  for (int y0 = 0; y0 < SCREEN_H; y0 += 40) {
    Canvas c(strip.data(), y0, 40); r->draw(c);
    for (int i = 0; i < SCREEN_W * 40; i++) strips[(size_t)y0 * SCREEN_W + i] = strip[i];
  }
  CHECK(mapped != plain);
  CHECK(mapped == strips);
  r->set.map = false;                                  // the switch turns it off without unloading it
  std::vector<uint16_t> off(SCREEN_W * SCREEN_H);
  { Canvas c(off.data(), 0, SCREEN_H); r->draw(c); }
  CHECK(off == plain);
}
