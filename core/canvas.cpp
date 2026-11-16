// RadarMASH - open firmware for the GeekMagic Radar 360.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "canvas.h"
#include "font5x7.h"
#include <math.h>
#include <string.h>

namespace rm {

static inline Color mix(Color d, Color c, int a) {
  int ia = 255 - a;
  int r = ((d >> 11) * ia + (c >> 11) * a + 127) / 255;
  int g = (((d >> 5) & 63) * ia + ((c >> 5) & 63) * a + 127) / 255;
  int b = ((d & 31) * ia + (c & 31) * a + 127) / 255;
  return (Color)((r << 11) | (g << 5) | b);
}

// Float to int, limited to [lo, hi]. Anything not a number becomes lo. Keeps wild coordinates (bad data) from
// overflowing an int or turning a short loop into billions of iterations.
static inline int clampInt(float v, int lo, int hi) { return !(v > lo) ? lo : v > hi ? hi : (int)v; }
static inline bool finite4(float a, float b, float c, float d) { return isfinite(a) && isfinite(b) && isfinite(c) && isfinite(d); }

void Canvas::pixel(int x, int y, Color c) {
  if (x < 0 || x >= SCREEN_W || !hasRow(y)) return;
  row(y)[x] = c;
}

void Canvas::blend(int x, int y, Color c, int alpha) {
  if (alpha <= 0 || x < 0 || x >= SCREEN_W || !hasRow(y)) return;
  uint16_t *p = row(y) + x;
  *p = alpha >= 255 ? c : mix(*p, c, alpha);
}

void Canvas::fill(Color c) {
  for (int i = 0; i < SCREEN_W * rows; i++) px[i] = c;
}

void Canvas::rect(int x, int y, int w, int h, Color c) { blendRect(x, y, w, h, c, 255); }

void Canvas::blendRect(int x, int y, int w, int h, Color c, int alpha) {
  long long xe = (long long)x + w, ye = (long long)y + h;        // 64-bit: x + w may not fit in an int
  int x1 = x < 0 ? 0 : x, x2 = xe > SCREEN_W ? SCREEN_W : (int)xe;
  int y1 = y < y0 ? y0 : y, y2 = ye > y0 + rows ? y0 + rows : (int)ye;
  for (int yy = y1; yy < y2; yy++) {
    uint16_t *r = row(yy);
    if (alpha >= 255) for (int xx = x1; xx < x2; xx++) r[xx] = c;
    else for (int xx = x1; xx < x2; xx++) r[xx] = mix(r[xx], c, alpha);
  }
}

// Xiaolin Wu style: walk the major axis and split coverage between the two nearest pixels on the minor axis.
void Canvas::line(float xa, float ya, float xb, float yb, Color c, int alpha) {
  if (!finite4(xa, ya, xb, yb)) return;
  float lo = ya < yb ? ya : yb, hi = ya < yb ? yb : ya;
  // Wholly outside this band. The margin covers the half pixel the ends are rounded out by, plus the second row
  // each step blends into.
  if (hi < y0 - 2 || lo > y0 + rows + 1) return;
  bool steep = fabsf(yb - ya) > fabsf(xb - xa);
  if (steep) { float t = xa; xa = ya; ya = t; t = xb; xb = yb; yb = t; }
  if (xa > xb) { float t = xa; xa = xb; xb = t; t = ya; ya = yb; yb = t; }
  float dx = xb - xa, grad = dx < 1e-6f ? 0 : (yb - ya) / dx;
  // only the part of the major axis that can be on screen (both axes are 360 long)
  int xs = clampInt(floorf(xa + 0.5f), -1, SCREEN_W), xe = clampInt(floorf(xb + 0.5f), -1, SCREEN_W);
  float y = ya + grad * (xs - xa);
  for (int x = xs; x <= xe; x++, y += grad) {
    if (!(y > -2.0f && y < SCREEN_W + 1.0f)) continue;
    int yi = (int)floorf(y);
    int a = (int)((y - yi) * 255);
    int a0 = (255 - a) * alpha / 255, a1 = a * alpha / 255;
    if (steep) { blend(yi, x, c, a0); blend(yi + 1, x, c, a1); }
    else { blend(x, yi, c, a0); blend(x, yi + 1, c, a1); }
  }
}

void Canvas::ring(float cx, float cy, float r, Color c) {
  if (!finite4(cx, cy, r, 0)) return;
  int ya = clampInt(floorf(cy - r - 1), y0, y0 + rows), yb = clampInt(ceilf(cy + r + 1), y0 - 1, y0 + rows - 1);
  for (int y = ya; y <= yb; y++) {
    float dy = y - cy, h2 = (r + 1) * (r + 1) - dy * dy;
    if (h2 < 0) continue;
    float half = sqrtf(h2);
    int xa = clampInt(floorf(cx - half), 0, SCREEN_W), xb = clampInt(ceilf(cx + half), -1, SCREEN_W - 1);
    for (int x = xa; x <= xb; x++) {
      float d = fabsf(sqrtf((x - cx) * (x - cx) + dy * dy) - r);
      if (d < 1.0f) blend(x, y, c, (int)((1.0f - d) * 255));
    }
  }
}

void Canvas::disc(float cx, float cy, float r, Color c) {
  if (!finite4(cx, cy, r, 0)) return;
  int ya = clampInt(floorf(cy - r - 1), y0, y0 + rows), yb = clampInt(ceilf(cy + r + 1), y0 - 1, y0 + rows - 1);
  int xa = clampInt(floorf(cx - r - 1), 0, SCREEN_W), xb = clampInt(ceilf(cx + r + 1), -1, SCREEN_W - 1);
  for (int y = ya; y <= yb; y++)
    for (int x = xa; x <= xb; x++) {
      float d = r + 0.5f - sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy));
      if (d > 0) blend(x, y, c, d >= 1 ? 255 : (int)(d * 255));
    }
}

void Canvas::triangle(float ax, float ay, float bx, float by, float cx, float cy, Color c) {
  float minx = fminf(ax, fminf(bx, cx)), maxx = fmaxf(ax, fmaxf(bx, cx));
  float miny = fminf(ay, fminf(by, cy)), maxy = fmaxf(ay, fmaxf(by, cy));
  if (!finite4(minx, maxx, miny, maxy)) return;
  int ya = clampInt(floorf(miny), y0, y0 + rows), yb = clampInt(ceilf(maxy), y0 - 1, y0 + rows - 1);
  int xa = clampInt(floorf(minx), 0, SCREEN_W), xb = clampInt(ceilf(maxx), -1, SCREEN_W - 1);
  float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
  if (fabsf(area) < 1e-6f) return;
  float s = area < 0 ? -1.0f : 1.0f;
  // 2x2 supersampling gives soft edges on what are very small shapes
  static const float off[2] = {0.25f, 0.75f};
  for (int y = ya; y <= yb; y++)
    for (int x = xa; x <= xb; x++) {
      int hit = 0;
      for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
          float qx = x + off[i] - 0.5f, qy = y + off[j] - 0.5f;
          float w0 = s * ((bx - ax) * (qy - ay) - (by - ay) * (qx - ax));
          float w1 = s * ((cx - bx) * (qy - by) - (cy - by) * (qx - bx));
          float w2 = s * ((ax - cx) * (qy - cy) - (ay - cy) * (qx - cx));
          if (w0 >= 0 && w1 >= 0 && w2 >= 0) hit++;
        }
      if (hit) blend(x, y, c, hit * 255 / 4);
    }
}

int Canvas::textWidth(const char *s, int scale) { return *s ? (int)strlen(s) * 6 * scale - scale : 0; }

void Canvas::text(int x, int y, const char *s, int scale, Color c) {
  if (y + 8 * scale <= y0 || y >= y0 + rows) return;
  for (; *s; s++, x += 6 * scale) {
    const uint8_t *g = FONT5X7 + (uint8_t)*s * 5;
    for (int col = 0; col < 5; col++)
      for (int bit = 0; bit < 8; bit++)
        if (g[col] & (1 << bit)) {
          if (scale == 1) pixel(x + col, y + bit, c);
          else rect(x + col * scale, y + bit * scale, scale, scale, c);
        }
  }
}

void Canvas::textCentred(int cx, int y, const char *s, int scale, Color c) { text(cx - textWidth(s, scale) / 2, y, s, scale, c); }

}  // namespace rm
