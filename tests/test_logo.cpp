// RadarMASH - open firmware for the GeekMagic Radar 360.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../core/config.h"
#include "../core/feed.h"
#include "../core/logo.h"
#include "../core/radar.h"
#include "test.h"
#include <memory>
#include <string.h>
#include <string>
#include <vector>

using namespace rm;

namespace {
std::vector<uint8_t> picture(int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  std::vector<uint8_t> p((size_t)w * h * 4);
  for (size_t i = 0; i < p.size(); i += 4) { p[i] = r; p[i + 1] = g; p[i + 2] = b; p[i + 3] = a; }
  return p;
}
// Hands out one made-up logo (a solid magenta square) for the airline "RDR" and nothing for anyone else.
struct FakeLogos : LogoSource {
  Logo one;
  int asked = 0;
  bool loaded = true;
  FakeLogos() { auto p = picture(8, 8, 255, 0, 255, 255); makeLogo(p.data(), 8, 8, one); }
  const Logo *logo(const char *icao) override { asked++; return loaded && strcmp(icao, "RDR") == 0 ? &one : nullptr; }
};
typedef std::vector<uint16_t> Pixels;
Pixels render(const Radar &r, int band) {
  Pixels px((size_t)SCREEN_W * SCREEN_H), strip((size_t)SCREEN_W * band);
  for (int y0 = 0; y0 < SCREEN_H; y0 += band) {
    int rows = y0 + band > SCREEN_H ? SCREEN_H - y0 : band;
    Canvas c(strip.data(), y0, rows);
    r.draw(c);
    memcpy(px.data() + (size_t)y0 * SCREEN_W, strip.data(), (size_t)SCREEN_W * rows * 2);
  }
  return px;
}
int count(const Pixels &p, uint16_t colour) { int n = 0; for (uint16_t v : p) n += v == colour; return n; }
}  // namespace

TEST(logo_is_shrunk_to_fit) {
  std::unique_ptr<Logo> l(new Logo());
  auto big = picture(90, 90, 255, 0, 0, 255);
  CHECK(makeLogo(big.data(), 90, 90, *l));
  CHECK(l->px[0] == rgb(255, 0, 0) && l->alpha[0] == 255);
  CHECK(l->px[LOGO_SIZE * LOGO_SIZE - 1] == rgb(255, 0, 0) && l->alpha[LOGO_SIZE * LOGO_SIZE - 1] == 255);
  auto small = picture(10, 10, 0, 255, 0, 255);                 // smaller than the box: enlarged to fill it
  CHECK(makeLogo(small.data(), 10, 10, *l) && l->alpha[LOGO_SIZE * 20 + 20] == 255 && l->px[0] == rgb(0, 255, 0));
}

TEST(logo_keeps_its_shape) {
  std::unique_ptr<Logo> l(new Logo());
  auto wide = picture(200, 50, 0, 0, 255, 255);                 // four times as wide as tall: a band across the middle
  CHECK(makeLogo(wide.data(), 200, 50, *l));
  CHECK(l->alpha[0] == 0 && l->alpha[LOGO_SIZE * (LOGO_SIZE - 1)] == 0);          // clear above and below
  CHECK(l->alpha[LOGO_SIZE * 20] == 255 && l->alpha[LOGO_SIZE * 20 + LOGO_SIZE - 1] == 255);
  int solid_rows = 0;
  for (int y = 0; y < LOGO_SIZE; y++) solid_rows += l->alpha[y * LOGO_SIZE + 20] == 255;
  CHECK(solid_rows == 10);
  auto tall = picture(30, 120, 0, 0, 255, 255);
  CHECK(makeLogo(tall.data(), 30, 120, *l));
  CHECK(l->alpha[LOGO_SIZE * 20] == 0 && l->alpha[LOGO_SIZE * 20 + 20] == 255);
}

TEST(clear_pixels_do_not_darken_the_edges) {
  // Half the picture is solid red, half fully clear with a meaningless black colour. Where they are averaged together
  // the result must be half-solid red, not dark red.
  std::vector<uint8_t> p(80 * 80 * 4, 0);
  for (int y = 0; y < 80; y++)
    for (int x = 0; x < 80; x++)
      if (x % 2 == 0) { uint8_t *q = &p[(y * 80 + x) * 4]; q[0] = 255; q[3] = 255; }
  std::unique_ptr<Logo> l(new Logo());
  CHECK(makeLogo(p.data(), 80, 80, *l));
  CHECK(l->px[LOGO_SIZE * 10 + 10] == rgb(255, 0, 0));
  CHECK_NEAR(l->alpha[LOGO_SIZE * 10 + 10], 127, 2);
}

TEST(logo_refuses_nonsense) {
  std::unique_ptr<Logo> l(new Logo());
  auto p = picture(4, 4, 1, 2, 3, 255);
  CHECK(!makeLogo(nullptr, 4, 4, *l) && !makeLogo(p.data(), 0, 4, *l) && !makeLogo(p.data(), 4, -1, *l) && !makeLogo(p.data(), 5000, 4, *l));
  CHECK(l->alpha[0] == 0);
  auto dot = picture(1, 1, 9, 9, 9, 255);
  CHECK(makeLogo(dot.data(), 1, 1, *l) && l->alpha[LOGO_SIZE * 20 + 20] == 255);
  auto sliver = picture(1000, 1, 9, 9, 9, 255);                // extreme shapes still give something, and stay in bounds
  CHECK(makeLogo(sliver.data(), 1000, 1, *l));
}

TEST(airline_comes_from_the_callsign) {
  char a[4];
  airlineFromCallsign("RYR8HV", a); CHECK(strcmp(a, "RYR") == 0);
  airlineFromCallsign("TOM3ME", a); CHECK(strcmp(a, "TOM") == 0);
  airlineFromCallsign("BAW1", a); CHECK(strcmp(a, "BAW") == 0);
  const char *none[] = {"", "GBNLN", "G-SWMP", "N123AB", "RYR", "RY8", "POTTR1", "ryr8hv", "12345", "A1B2"};
  for (const char *n : none) { airlineFromCallsign(n, a); CHECK(a[0] == 0); }
  const char *t = R"({"ac":[{"hex":"a1","flight":"EZY16AX ","lat":51,"lon":-1,"alt_baro":9000},{"hex":"a2","r":"G-ABCD","lat":51,"lon":-1,"alt_baro":900}]})";
  std::vector<Aircraft> ac(4);
  CHECK(parseAircraft(t, strlen(t), 51, -1, false, ac.data(), 4) == 2);
  CHECK(strcmp(ac[0].airline_icao, "EZY") == 0 && ac[1].airline_icao[0] == 0);
  const char *r = R"({"response":{"flightroute":{"airline":{"name":"Radar Air","icao":"RDR","iata":"RA"},"origin":{"iata_code":"AAA"},"destination":{"iata_code":"BBB"}}}})";
  RouteInfo info;
  CHECK(parseRoute(r, strlen(r), info) == RouteResult::Found && strcmp(info.airline_icao, "RDR") == 0);
}

TEST(logo_source_is_a_setting_and_empty_by_default) {
  Config c;
  CHECK(c.logo_url[0] == 0);                                    // no logos unless the user points at some
  const char *set = "{\"logo_url\":\"https://logos.example.org/{icao}.png\"}";
  CHECK(configFromJson(set, strlen(set), c) && strcmp(c.logo_url, "https://logos.example.org/{icao}.png") == 0);
  char buf[CONFIG_JSON_MAX];
  size_t n = configToJson(c, buf, sizeof buf);
  Config d;
  CHECK(configFromJson(buf, n, d) && strcmp(d.logo_url, c.logo_url) == 0);
  const char *bad = "{\"logo_url\":\"javascript:alert(1)\"}";
  CHECK(configFromJson(bad, strlen(bad), c) && strcmp(c.logo_url, "https://logos.example.org/{icao}.png") == 0);
  const char *off = "{\"logo_url\":\"\"}";
  CHECK(configFromJson(off, strlen(off), c) && c.logo_url[0] == 0);
}

TEST(tag_box_shows_the_logo) {
  std::unique_ptr<Radar> r(new Radar());
  r->home_lat = 51; r->home_lon = -1; r->set.auto_cycle = false; r->set.sweep = false;
  demoTraffic(*r, 9000); r->tick(9000);
  r->selected = 0;                                              // RDR4077
  const uint16_t magenta = rgb(255, 0, 255);
  Pixels plain = render(*r, SCREEN_H);
  CHECK(count(plain, magenta) == 0);

  FakeLogos logos;
  r->logos = &logos;
  r->tick(9001);
  Pixels with = render(*r, SCREEN_H);
  CHECK(count(with, magenta) == LOGO_SIZE * LOGO_SIZE);          // the whole logo, drawn once
  CHECK(with == render(*r, 40) && with == render(*r, 7));        // and the same drawn in strips
  CHECK(logos.asked > 0);

  r->set.logos = false;                                         // switched off
  r->tick(9002);
  CHECK(count(render(*r, SCREEN_H), magenta) == 0);
  r->set.logos = true;
  logos.loaded = false;                                         // not fetched yet: the tag is drawn without it
  r->tick(9003);
  CHECK(count(render(*r, SCREEN_H), magenta) == 0);
  logos.loaded = true;                                          // arrives while the aircraft is still highlighted
  r->tick(9004);
  CHECK(count(render(*r, SCREEN_H), magenta) == LOGO_SIZE * LOGO_SIZE);

  r->selected = 4;                                              // POTTR1: not an airline callsign, so nothing is asked for
  int before = logos.asked;
  r->tick(9005);
  CHECK(count(render(*r, SCREEN_H), magenta) == 0 && logos.asked == before);
}
