// RadarMASH - open firmware for the GeekMagic Radar 360.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../core/canvas.h"
#include "../core/font.h"
#include "test.h"
#include <algorithm>
#include <chrono>
#include <string.h>
#include <vector>

using namespace rm;

namespace {
struct Frame {                                   // a canvas with its own buffer, cleared to black
  std::vector<uint16_t> px;
  Canvas c;
  Frame(int y0 = 0, int rows = SCREEN_H) : px((size_t)SCREEN_W * rows, 0), c(px.data(), y0, rows) {}
  uint16_t at(int x, int y) { return c.row(y)[x]; }
  int lit() { int n = 0; for (uint16_t p : px) n += p != 0; return n; }
};
const Color WHITE = rgb(255, 255, 255), RED = rgb(255, 0, 0);
double msSince(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}
}  // namespace

TEST(rgb_packs_to_rgb565) {
  CHECK(rgb(255, 0, 0) == 0xF800);
  CHECK(rgb(0, 255, 0) == 0x07E0);
  CHECK(rgb(0, 0, 255) == 0x001F);
  CHECK(rgb(255, 255, 255) == 0xFFFF);
  CHECK(rgb(0, 0, 0) == 0);
}

TEST(pixel_sets_and_clips) {
  Frame f;
  f.c.pixel(10, 20, WHITE);
  CHECK(f.at(10, 20) == WHITE);
  f.c.pixel(-1, 0, WHITE); f.c.pixel(SCREEN_W, 0, WHITE); f.c.pixel(0, -1, WHITE); f.c.pixel(0, SCREEN_H, WHITE);
  CHECK(f.lit() == 1);
}

TEST(pixel_respects_band) {
  Frame f(80, 40);                               // rows 80..119 only
  f.c.pixel(5, 79, WHITE); f.c.pixel(5, 120, WHITE);
  CHECK(f.lit() == 0);
  f.c.pixel(5, 80, WHITE); f.c.pixel(5, 119, WHITE);
  CHECK(f.lit() == 2);
  CHECK(f.at(5, 80) == WHITE);
}

TEST(blend_extremes_and_middle) {
  Frame f;
  f.c.blend(1, 1, WHITE, 0);
  CHECK(f.at(1, 1) == 0);
  f.c.blend(1, 1, WHITE, 255);
  CHECK(f.at(1, 1) == WHITE);
  f.c.blend(2, 2, WHITE, 128);
  uint16_t m = f.at(2, 2);
  CHECK_NEAR(m >> 11, 16, 1); CHECK_NEAR((m >> 5) & 63, 32, 1); CHECK_NEAR(m & 31, 16, 1);
  f.c.blend(-5, 2, WHITE, 255); f.c.blend(2, 9999, WHITE, 255);   // must not touch memory
}

TEST(fill_and_rect) {
  Frame f(0, 10);
  f.c.fill(RED);
  CHECK(f.lit() == SCREEN_W * 10);
  f.c.fill(0);
  f.c.rect(-10, -10, 20, 15, WHITE);             // partly off the top-left corner
  CHECK(f.lit() == 10 * 5);
  f.c.fill(0);
  f.c.rect(SCREEN_W - 3, 8, 50, 50, WHITE);      // partly off the right and the bottom of the band
  CHECK(f.lit() == 3 * 2);
  f.c.rect(100, 100, 5, 5, WHITE);               // wholly below the band
  CHECK(f.lit() == 3 * 2);
}

TEST(text_width) {
  CHECK(Canvas::textWidth("", 2) == 0);
  CHECK(Canvas::textWidth("A", 1) == 5);
  CHECK(Canvas::textWidth("AB", 1) == 11);
  CHECK(Canvas::textWidth("AB", 2) == 22);
}

TEST(text_stays_in_its_cell) {
  Frame f;
  f.c.text(50, 60, "RADAR", 2, WHITE);
  int w = Canvas::textWidth("RADAR", 2), lit = 0;
  for (int y = 0; y < SCREEN_H; y++)
    for (int x = 0; x < SCREEN_W; x++)
      if (f.at(x, y)) { lit++; CHECK(x >= 50 && x < 50 + w && y >= 60 && y < 60 + 16); }
  CHECK(lit > 50);
  Frame g;
  g.c.textCentred(180, 10, "RADAR", 2, WHITE);
  int minx = SCREEN_W, maxx = -1;
  for (int y = 0; y < SCREEN_H; y++)
    for (int x = 0; x < SCREEN_W; x++)
      if (g.at(x, y)) { if (x < minx) minx = x; if (x > maxx) maxx = x; }
  CHECK_NEAR((minx + maxx) / 2.0, 180, 1.5);
}

TEST(text_off_screen_is_safe) {
  Frame f(100, 20);
  f.c.text(-40, 95, "CLIPPED AT THE LEFT AND TOP", 3, WHITE);
  f.c.text(340, 110, "CLIPPED AT THE RIGHT AND BOTTOM", 3, WHITE);
  f.c.text(0, 5000, "NOWHERE", 2, WHITE);
  CHECK(f.lit() > 0);
}

TEST(line_reaches_both_ends) {
  Frame f;
  f.c.line(10, 10, 100, 10, WHITE);
  CHECK(f.at(10, 10) == WHITE); CHECK(f.at(55, 10) == WHITE); CHECK(f.at(100, 10) == WHITE);
  CHECK(f.at(9, 10) == 0); CHECK(f.at(101, 10) == 0); CHECK(f.at(55, 12) == 0);
  f.c.line(200, 50, 200, 150, WHITE);            // vertical
  CHECK(f.at(200, 50) == WHITE); CHECK(f.at(200, 150) == WHITE);
  f.c.line(300, 300, 250, 250, WHITE);           // diagonal, drawn backwards
  CHECK(f.at(275, 275) != 0);
}

TEST(line_alpha_dims) {
  Frame f;
  f.c.line(10, 10, 100, 10, WHITE, 100);
  uint16_t p = f.at(50, 10);
  CHECK(p != 0 && p != WHITE);
}

TEST(shapes_far_off_screen_are_fast_and_safe) {
  // Bad data must never hang the device or scribble outside the buffer.
  Frame f;
  auto t0 = std::chrono::steady_clock::now();
  f.c.line(-1e9f, -1e9f, 1e9f, 1e9f, WHITE);
  f.c.line(180, 180, 1e12f, -3e11f, WHITE);
  f.c.line(NAN, 0, 10, 10, WHITE);
  f.c.ring(180, 180, 1e8f, WHITE);
  f.c.ring(1e9f, -1e9f, 50, WHITE);
  f.c.disc(180, 180, 1e7f, WHITE);
  f.c.disc(NAN, NAN, 5, WHITE);
  f.c.triangle(-1e9f, -1e9f, 1e9f, -1e9f, 0, 1e9f, WHITE);
  f.c.triangle(0, 0, NAN, 5, 9, 9, WHITE);
  f.c.rect(-2000000000, -2000000000, 2000000000, 2000000000, WHITE);
  CHECK(msSince(t0) < 2000);
}

TEST(disc_and_ring) {
  Frame f;
  f.c.disc(100, 100, 10, WHITE);
  CHECK(f.at(100, 100) == WHITE); CHECK(f.at(108, 100) == WHITE); CHECK(f.at(113, 100) == 0);
  Frame g;
  g.c.ring(100, 100, 20, WHITE);
  CHECK(g.at(120, 100) == WHITE); CHECK(g.at(100, 80) == WHITE);
  CHECK(g.at(100, 100) == 0); CHECK(g.at(110, 100) == 0); CHECK(g.at(125, 100) == 0);
}

TEST(triangle_fills_inside_only) {
  Frame f;
  f.c.triangle(100, 100, 140, 100, 100, 140, WHITE);
  CHECK(f.at(110, 110) == WHITE);
  CHECK(f.at(135, 135) == 0);
  CHECK(f.at(95, 110) == 0);
  Frame g;                                       // opposite winding gives the same result
  g.c.triangle(100, 100, 100, 140, 140, 100, WHITE);
  CHECK(g.at(110, 110) == WHITE);
  g.c.triangle(50, 50, 60, 60, 70, 70, WHITE);   // zero area: draws nothing, does not crash
  CHECK(g.at(60, 60) == 0);
}

// ---- fonts ----
namespace {
struct UseFont { int before; UseFont(int i) : before(currentFont()) { setFont(i); } ~UseFont() { setFont(before); } };
}

TEST(fonts_are_listed_and_found_by_id) {
  CHECK(FONT_COUNT >= 5);
  CHECK(fontIndex("classic") == 0);
  for (int i = 0; i < FONT_COUNT; i++) {
    CHECK(fontIndex(FONTS[i].id) == i);
    CHECK(FONTS[i].name[0] != 0 && strlen(FONTS[i].id) < 16);
  }
  CHECK(fontIndex("no-such-font") == -1 && fontIndex("") == -1);
  char buf[600];
  size_t n = fontsJson(buf, sizeof buf);
  CHECK(n > 20 && buf[0] == '[' && buf[n - 1] == ']' && strstr(buf, "\"id\":\"classic\""));
  char tiny[8];
  CHECK(fontsJson(tiny, sizeof tiny) == 0);
}

TEST(choosing_a_font_that_does_not_exist_falls_back) {
  UseFont f(0);
  setFont(999); CHECK(currentFont() == 0);
  setFont(-3); CHECK(currentFont() == 0);
  setFont(FONT_COUNT - 1); CHECK(currentFont() == FONT_COUNT - 1);
}

TEST(every_font_has_sensible_sizes) {
  for (int i = 0; i < FONT_COUNT; i++) {
    UseFont f(i);
    CHECK(lineHeight(1) >= 8 && lineHeight(1) <= 12);
    CHECK(lineHeight(2) == 16);                              // layouts are built around these two
    CHECK(lineHeight(3) == 24);
    CHECK(lineHeight(0) == lineHeight(1) && lineHeight(9) == lineHeight(3) * (i == 0 ? 3 : 1));
    int narrow = Canvas::textWidth("RDR4077", 2), wide = Canvas::textWidth("RDR4077", 3);
    CHECK(narrow > 30 && narrow <= 7 * 12 && wide > narrow);  // never wider than the classic font, so labels still fit
    CHECK(Canvas::textWidth("", 2) == 0);
    CHECK(Canvas::textWidth("\xF8", 2) > 0 && Canvas::textWidth("\x01\xFF", 2) > 0);   // degree sign; unknown bytes become '?'
  }
}

TEST(every_font_draws_inside_its_line) {
  for (int i = 0; i < FONT_COUNT; i++)
    for (int size = 1; size <= 3; size++) {
      UseFont f(i);
      Frame fr;
      const char *s = "Agjy|RDR4077 LHR>JFK 285\xF8";
      fr.c.text(20, 100, s, size, WHITE);
      int w = Canvas::textWidth(s, size), h = lineHeight(size), lit = 0;
      for (int y = 0; y < SCREEN_H; y++)
        for (int x = 0; x < SCREEN_W; x++)
          if (fr.at(x, y)) { lit++; CHECK(y >= 100 && y < 100 + h); CHECK(x >= 18 && x <= 20 + w + 2); }
      CHECK(lit > 60);
    }
}

TEST(every_font_draws_the_same_in_strips) {
  for (int i = 0; i < FONT_COUNT; i++) {
    UseFont f(i);
    Frame whole;
    whole.c.text(-6, 93, "Clipped left and split by a strip edge 0123456789", 3, WHITE);
    whole.c.text(250, 111, "runs off the right", 2, RED);
    std::vector<uint16_t> joined((size_t)SCREEN_W * SCREEN_H, 0), strip((size_t)SCREEN_W * 7);
    for (int y0 = 0; y0 < SCREEN_H; y0 += 7) {
      int rows = y0 + 7 > SCREEN_H ? SCREEN_H - y0 : 7;
      std::fill(strip.begin(), strip.end(), 0);
      Canvas c(strip.data(), y0, rows);
      c.text(-6, 93, "Clipped left and split by a strip edge 0123456789", 3, WHITE);
      c.text(250, 111, "runs off the right", 2, RED);
      memcpy(joined.data() + (size_t)y0 * SCREEN_W, strip.data(), (size_t)SCREEN_W * rows * 2);
    }
    CHECK(joined == whole.px);
  }
}
