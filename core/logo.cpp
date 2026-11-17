// RadarMASH - open firmware for the GeekMagic Radar 360.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "logo.h"
#include "canvas.h"
#include <string.h>

namespace rm {

bool makeLogo(const uint8_t *rgba, int w, int h, Logo &out) {
  memset(&out, 0, sizeof out);
  if (!rgba || w < 1 || h < 1 || w > 1024 || h > 1024) return false;
  // the largest size with the picture's shape that fits the square, centred
  int tw = w >= h ? LOGO_SIZE : (LOGO_SIZE * w + h / 2) / h, th = h >= w ? LOGO_SIZE : (LOGO_SIZE * h + w / 2) / w;
  if (tw < 1) tw = 1;
  if (th < 1) th = 1;
  const int ox = (LOGO_SIZE - tw) / 2, oy = (LOGO_SIZE - th) / 2;
  for (int ty = 0; ty < th; ty++) {
    int y0 = ty * h / th, y1 = (ty + 1) * h / th;
    if (y1 <= y0) y1 = y0 + 1;
    for (int tx = 0; tx < tw; tx++) {
      int x0 = tx * w / tw, x1 = (tx + 1) * w / tw;
      if (x1 <= x0) x1 = x0 + 1;
      // Average the block of source pixels this one covers. Colours are weighted by how solid they are, so clear
      // pixels (whose colour is meaningless) do not darken the edges.
      uint32_t r = 0, g = 0, b = 0, a = 0, n = 0;
      for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
          const uint8_t *p = rgba + ((size_t)y * w + x) * 4;
          r += (uint32_t)p[0] * p[3]; g += (uint32_t)p[1] * p[3]; b += (uint32_t)p[2] * p[3]; a += p[3]; n++;
        }
      int i = (oy + ty) * LOGO_SIZE + ox + tx;
      out.alpha[i] = (uint8_t)(a / n);
      out.px[i] = a ? rgb((int)(r / a), (int)(g / a), (int)(b / a)) : 0;
    }
  }
  return true;
}

void airlineFromCallsign(const char *cs, char out[4]) {
  out[0] = 0;
  for (int i = 0; i < 3; i++) if (cs[i] < 'A' || cs[i] > 'Z') return;
  if (cs[3] < '0' || cs[3] > '9') return;
  memcpy(out, cs, 3); out[3] = 0;
}

}  // namespace rm
