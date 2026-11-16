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
  int x1 = x < 0 ? 0 : x, x2 = x + w > SCREEN_W ? SCREEN_W : x + w;
  int y1 = y < y0 ? y0 : y, y2 = y + h > y0 + rows ? y0 + rows : y + h;
  for (int yy = y1; yy < y2; yy++) {
    uint16_t *r = row(yy);
    if (alpha >= 255) for (int xx = x1; xx < x2; xx++) r[xx] = c;
    else for (int xx = x1; xx < x2; xx++) r[xx] = mix(r[xx], c, alpha);
  }
}

// Xiaolin Wu style: walk the major axis and split coverage between the two nearest pixels on the minor axis.
void Canvas::line(float xa, float ya, float xb, float yb, Color c) {
  float lo = ya < yb ? ya : yb, hi = ya < yb ? yb : ya;
  if (hi < y0 - 1 || lo > y0 + rows) return;                   // wholly outside this band
  bool steep = fabsf(yb - ya) > fabsf(xb - xa);
  if (steep) { float t = xa; xa = ya; ya = t; t = xb; xb = yb; yb = t; }
  if (xa > xb) { float t = xa; xa = xb; xb = t; t = ya; ya = yb; yb = t; }
  float dx = xb - xa, grad = dx < 1e-6f ? 0 : (yb - ya) / dx;
  int xs = (int)floorf(xa + 0.5f), xe = (int)floorf(xb + 0.5f);
  float y = ya + grad * (xs - xa);
  for (int x = xs; x <= xe; x++, y += grad) {
    int yi = (int)floorf(y);
    int a = (int)((y - yi) * 255);
    if (steep) { blend(yi, x, c, 255 - a); blend(yi + 1, x, c, a); }
    else { blend(x, yi, c, 255 - a); blend(x, yi + 1, c, a); }
  }
}

void Canvas::ring(float cx, float cy, float r, Color c) {
  int ya = (int)floorf(cy - r - 1), yb = (int)ceilf(cy + r + 1);
  if (ya < y0) ya = y0;
  if (yb >= y0 + rows) yb = y0 + rows - 1;
  for (int y = ya; y <= yb; y++) {
    float dy = y - cy, h2 = (r + 1) * (r + 1) - dy * dy;
    if (h2 < 0) continue;
    float half = sqrtf(h2);
    int xa = (int)floorf(cx - half), xb = (int)ceilf(cx + half);
    for (int x = xa; x <= xb; x++) {
      float d = fabsf(sqrtf((x - cx) * (x - cx) + dy * dy) - r);
      if (d < 1.0f) blend(x, y, c, (int)((1.0f - d) * 255));
    }
  }
}

void Canvas::disc(float cx, float cy, float r, Color c) {
  int ya = (int)floorf(cy - r - 1), yb = (int)ceilf(cy + r + 1);
  if (ya < y0) ya = y0;
  if (yb >= y0 + rows) yb = y0 + rows - 1;
  for (int y = ya; y <= yb; y++)
    for (int x = (int)floorf(cx - r - 1); x <= (int)ceilf(cx + r + 1); x++) {
      float d = r + 0.5f - sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy));
      if (d > 0) blend(x, y, c, d >= 1 ? 255 : (int)(d * 255));
    }
}

void Canvas::triangle(float ax, float ay, float bx, float by, float cx, float cy, Color c) {
  float minx = fminf(ax, fminf(bx, cx)), maxx = fmaxf(ax, fmaxf(bx, cx));
  float miny = fminf(ay, fminf(by, cy)), maxy = fmaxf(ay, fmaxf(by, cy));
  int ya = (int)floorf(miny), yb = (int)ceilf(maxy);
  if (ya < y0) ya = y0;
  if (yb >= y0 + rows) yb = y0 + rows - 1;
  float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
  if (fabsf(area) < 1e-6f) return;
  float s = area < 0 ? -1.0f : 1.0f;
  // 2x2 supersampling gives soft edges on what are very small shapes
  static const float off[2] = {0.25f, 0.75f};
  for (int y = ya; y <= yb; y++)
    for (int x = (int)floorf(minx); x <= (int)ceilf(maxx); x++) {
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

int Canvas::textWidth(const char *s, int scale) { return (int)strlen(s) * 6 * scale - scale; }

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
