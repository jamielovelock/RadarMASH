// RadarMASH - open firmware for the GeekMagic Radar 360.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "canvas.h"
#include "font.h"
#include "font5x7.h"
#include <math.h>
#include <stdio.h>
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

// ---- fonts ----
static int active_font = 0;

int fontIndex(const char *id) {
  for (int i = 0; i < FONT_COUNT; i++) if (strcmp(FONTS[i].id, id) == 0) return i;
  return -1;
}
void setFont(int index) { active_font = index >= 0 && index < FONT_COUNT ? index : 0; }
int currentFont() { return active_font; }

static inline int tier(int size) { return size < 1 ? 0 : size > 3 ? 2 : size - 1; }
static inline const FontFace *faceFor(int size) { return FONTS[active_font].face[tier(size)]; }
// Which of a face's 96 glyphs draws this byte: ASCII, the degree sign (0xF8), or a question mark for anything else.
static inline const Glyph &glyphFor(const FontFace *f, unsigned char ch) {
  return f->glyphs[ch >= 32 && ch <= 126 ? ch - 32 : ch == 0xF8 ? 95 : '?' - 32];
}

int lineHeight(int size) {
  const FontFace *f = faceFor(size);
  return f ? f->height : 8 * (size < 1 ? 1 : size);
}

size_t fontsJson(char *out, size_t n) {
  size_t used = 0;
  for (int i = 0; i < FONT_COUNT; i++) {
    int w = snprintf(out + used, n - used, "%s{\"id\":\"%s\",\"name\":\"%s\"}", i ? "," : "[", FONTS[i].id, FONTS[i].name);
    if (w < 0 || (size_t)w >= n - used) { if (n) out[0] = 0; return 0; }
    used += (size_t)w;
  }
  if (used + 2 > n) { if (n) out[0] = 0; return 0; }
  out[used++] = ']'; out[used] = 0;
  return used;
}

int Canvas::textWidth(const char *s, int size) {
  const FontFace *f = faceFor(size);
  if (!f) return *s ? (int)strlen(s) * 6 * size - size : 0;
  int w = 0;
  for (; *s; s++) w += glyphFor(f, (unsigned char)*s).advance;
  return w;
}

// A typeface glyph: each pixel carries how much of it the letter covers, blended over what is already there.
static void drawGlyph(Canvas &c, const FontFace *f, const Glyph &g, int x, int y, Color col) {
  const int stride = (g.w + 1) / 2;
  for (int row = 0; row < f->height; row++) {
    int py = y + row;
    if (!c.hasRow(py)) continue;
    const uint8_t *bits = f->data + g.offset + row * stride;
    for (int i = 0; i < g.w; i++) {
      int a = (i & 1) ? bits[i >> 1] & 15 : bits[i >> 1] >> 4;
      if (a) c.blend(x + g.left + i, py, col, a * 17);
    }
  }
}

void Canvas::text(int x, int y, const char *s, int scale, Color c) {
  if (scale < 1) return;
  if (const FontFace *f = faceFor(scale)) {
    if (y + f->height <= y0 || y >= y0 + rows) return;
    for (; *s; s++) {
      const Glyph &g = glyphFor(f, (unsigned char)*s);
      if (x + g.left + g.w > 0 && x + g.left < SCREEN_W) drawGlyph(*this, f, g, x, y, c);
      x += g.advance;
    }
    return;
  }
  if (y + 8 * scale <= y0 || y >= y0 + rows) return;
  // The classic pixel font: work out once which of the glyph's eight rows fall in this strip, then write pixels
  // straight into the buffer.
  for (; *s; s++, x += 6 * scale) {
    if (x + 5 * scale <= 0 || x >= SCREEN_W) continue;
    const uint8_t *g = FONT5X7 + (uint8_t)*s * 5;
    for (int bit = 0; bit < 8; bit++) {
      uint8_t mask = (uint8_t)(1 << bit);
      if (!((g[0] | g[1] | g[2] | g[3] | g[4]) & mask)) continue;
      for (int sy = 0; sy < scale; sy++) {
        int py = y + bit * scale + sy;
        if (!hasRow(py)) continue;
        uint16_t *r = row(py);
        for (int col = 0; col < 5; col++) {
          if (!(g[col] & mask)) continue;
          int px = x + col * scale;
          for (int sx = 0; sx < scale; sx++, px++)
            if (px >= 0 && px < SCREEN_W) r[px] = c;
        }
      }
    }
  }
}

void Canvas::textCentred(int cx, int y, const char *s, int scale, Color c) { text(cx - textWidth(s, scale) / 2, y, s, scale, c); }

}  // namespace rm
