// RadarMASH - open firmware for the GeekMagic Radar 360.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Canvas: portable drawing into an RGB565 pixel buffer. No hardware or Arduino dependencies, so the same code runs on
// the device and in the desktop simulator.
//
// Coordinates are "logical": (0,0) is the top-left as the viewer sees the screen. A canvas covers the full width but only
// a band of rows [y0, y0+rows), so the device can draw a frame in strips without holding a whole framebuffer in RAM. The
// simulator simply uses one band of 360 rows. Everything clips to the band.
#pragma once
#include <stdint.h>

namespace rm {

constexpr int SCREEN_W = 360, SCREEN_H = 360;
constexpr float CENTRE = 179.5f;          // centre of the round screen in pixel coordinates

typedef uint16_t Color;                   // RGB565, native byte order
constexpr Color rgb(int r, int g, int b) { return (Color)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)); }

struct Canvas {
  uint16_t *px = nullptr;                 // SCREEN_W * rows pixels
  int y0 = 0, rows = 0;

  Canvas(uint16_t *buf, int band_y0, int band_rows) : px(buf), y0(band_y0), rows(band_rows) {}

  bool hasRow(int y) const { return y >= y0 && y < y0 + rows; }
  uint16_t *row(int y) { return px + (y - y0) * SCREEN_W; }   // caller checks hasRow

  void pixel(int x, int y, Color c);
  void blend(int x, int y, Color c, int alpha);                // alpha 0..255
  void fill(Color c);
  void rect(int x, int y, int w, int h, Color c);
  void blendRect(int x, int y, int w, int h, Color c, int alpha);
  void line(float x0, float y0, float x1, float y1, Color c, int alpha = 255);  // anti-aliased
  void ring(float cx, float cy, float r, Color c);             // anti-aliased 1 px circle outline
  void disc(float cx, float cy, float r, Color c);             // anti-aliased filled circle
  void triangle(float ax, float ay, float bx, float by, float cx, float cy, Color c);   // filled

  // Text in the current font (see font.h) at size 1 (small), 2 (medium) or 3 (large); y is the top of the line.
  // Transparent background. The byte 0xF8 is the degree sign.
  void text(int x, int y, const char *s, int size, Color c);
  void textCentred(int cx, int y, const char *s, int size, Color c);
  static int textWidth(const char *s, int size);
};

}  // namespace rm
