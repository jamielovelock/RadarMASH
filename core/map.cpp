// RadarMASH - open firmware for the GeekMagic Radar 360.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "map.h"
#include "radar.h"
#include <math.h>
#include <string.h>

namespace rm {

static const double PI_D = 3.14159265358979323846;

double mercatorX(double lon, int zoom) { return (lon + 180.0) / 360.0 * TILE_SIZE * (double)(1 << zoom); }

double mercatorY(double lat, int zoom) {
  if (lat > 85.0511) lat = 85.0511;                 // the square map stops here
  if (lat < -85.0511) lat = -85.0511;
  double s = sin(lat * PI_D / 180.0);
  return (0.5 - log((1.0 + s) / (1.0 - s)) / (4.0 * PI_D)) * TILE_SIZE * (double)(1 << zoom);
}

int zoomForRange(double lat, float range_km, int detail) {
  double screen_m_per_px = (double)range_km * 1000.0 / RADAR_RADIUS;
  double world_m_per_px = 156543.03392 * cos(lat * PI_D / 180.0);   // at zoom 0
  if (!(screen_m_per_px > 0) || !(world_m_per_px > 0)) return 0;
  int z = (int)lround(log2(world_m_per_px / screen_m_per_px)) + (detail < 0 ? 0 : detail > 2 ? 2 : detail);
  return z < 0 ? 0 : z > MAX_ZOOM ? MAX_ZOOM : z;
}

static inline Color restyle(Color c, MapStyle style) {
  if (style == MapStyle::AsIs) return c;
  int r = (c >> 11) << 3, g = ((c >> 5) & 63) << 2, b = (c & 31) << 3;
  int v = 255 - (r * 30 + g * 59 + b * 11) / 100;   // inverted brightness: pale land goes dark, dark lines go bright
  v = v * v / 255 + v / 3;                          // keep the background down and let roads and text stand out
  if (v > 255) v = 255;
  return style == MapStyle::Green ? rgb(v / 6, v, v / 2) : rgb(v * 3 / 4, v * 13 / 16, v);
}

int composeMap(MapImage &out, TileSource &src, double home_lat, double home_lon, float range_km, MapStyle style, int detail) {
  const int zoom = zoomForRange(home_lat, range_km, detail);
  const int world_tiles = 1 << zoom;
  const double km_per_px = (double)range_km / RADAR_RADIUS;
  const double km_per_deg_lon = 111.32 * cos(home_lat * PI_D / 180.0);

  // The radar is drawn on a flat grid of kilometres around home, so each screen column is one longitude and each row one
  // latitude. Work the map position out once per column and per row, not per pixel.
  static double gx[SCREEN_W], gy[SCREEN_H];
  for (int x = 0; x < SCREEN_W; x++) gx[x] = mercatorX(home_lon + (x - CENTRE) * km_per_px / km_per_deg_lon, zoom);
  for (int y = 0; y < SCREEN_H; y++) gy[y] = mercatorY(home_lat + (CENTRE - y) * km_per_px / 110.57, zoom);

  const double step = gx[1] - gx[0];                 // tile pixels per screen pixel
  const int shrink = step > 3.2 ? 4 : step > 2.4 ? 3 : step > 1.5 ? 2 : 1;

  memset(out.px, 0, sizeof out.px);
  out.zoom = zoom;
  int missing = 0;

  int ty0 = (int)floor(gy[0] / TILE_SIZE), ty1 = (int)floor(gy[SCREEN_H - 1] / TILE_SIZE);
  int tx0 = (int)floor(gx[0] / TILE_SIZE), tx1 = (int)floor(gx[SCREEN_W - 1] / TILE_SIZE);
  const float limit = (RADAR_RADIUS + 1.5f) * (RADAR_RADIUS + 1.5f);
  for (int ty = ty0; ty <= ty1; ty++)
    for (int tx = tx0; tx <= tx1; tx++) {
      if (ty < 0 || ty >= world_tiles) continue;                          // beyond the poles
      int wrapped = ((tx % world_tiles) + world_tiles) % world_tiles;     // across the date line
      const uint16_t *t = src.tile(zoom, wrapped, ty);
      if (!t) { missing++; continue; }
      // Tile pixels rarely line up one-to-one with screen pixels (the zoom levels are a factor of two apart), so each
      // screen pixel is a blend of the four tile pixels around it. Taking just the nearest one looks blocky.
      for (int y = 0; y < SCREEN_H; y++) {
        if ((int)floor(gy[y]) - ty * TILE_SIZE < 0 || (int)floor(gy[y]) - ty * TILE_SIZE >= TILE_SIZE) continue;
        double sy = gy[y] - 0.5 - ty * TILE_SIZE;                         // in tile pixels, measured from pixel centres
        int y0 = (int)floor(sy), wy = (int)((sy - y0) * 256);
        int ya = y0 < 0 ? 0 : y0, yb = y0 + 1 > TILE_SIZE - 1 ? TILE_SIZE - 1 : y0 + 1;   // at a tile's edge, repeat its edge
        float dy = y - CENTRE;
        for (int x = 0; x < SCREEN_W; x++) {
          int px = (int)floor(gx[x]) - tx * TILE_SIZE;
          if (px < 0 || px >= TILE_SIZE) continue;
          float dx = x - CENTRE;
          if (dx * dx + dy * dy > limit) continue;                        // outside the glass
          if (shrink > 1) {                                               // several tile pixels per screen pixel: average them
            int r = 0, g = 0, b = 0;
            for (int j = 0; j < shrink; j++) {
              int qy = (int)floor(gy[y] + ((j + 0.5) / shrink - 0.5) * step) - ty * TILE_SIZE;
              qy = qy < 0 ? 0 : qy > TILE_SIZE - 1 ? TILE_SIZE - 1 : qy;
              for (int i = 0; i < shrink; i++) {
                int qx = (int)floor(gx[x] + ((i + 0.5) / shrink - 0.5) * step) - tx * TILE_SIZE;
                qx = qx < 0 ? 0 : qx > TILE_SIZE - 1 ? TILE_SIZE - 1 : qx;
                uint16_t q = t[qy * TILE_SIZE + qx];
                r += q >> 11; g += (q >> 5) & 63; b += q & 31;
              }
            }
            int n = shrink * shrink;
            out.px[y * SCREEN_W + x] = restyle((Color)(((r + n / 2) / n << 11) | ((g + n / 2) / n << 5) | ((b + n / 2) / n)), style);
            continue;
          }
          double sx = gx[x] - 0.5 - tx * TILE_SIZE;
          int x0 = (int)floor(sx), wx = (int)((sx - x0) * 256);
          int xa = x0 < 0 ? 0 : x0, xb = x0 + 1 > TILE_SIZE - 1 ? TILE_SIZE - 1 : x0 + 1;
          uint16_t p00 = t[ya * TILE_SIZE + xa], p01 = t[ya * TILE_SIZE + xb], p10 = t[yb * TILE_SIZE + xa], p11 = t[yb * TILE_SIZE + xb];
          int w00 = (256 - wx) * (256 - wy), w01 = wx * (256 - wy), w10 = (256 - wx) * wy, w11 = wx * wy;   // sum to 65536
          int r = ((p00 >> 11) * w00 + (p01 >> 11) * w01 + (p10 >> 11) * w10 + (p11 >> 11) * w11 + 32768) >> 16;
          int g = (((p00 >> 5) & 63) * w00 + ((p01 >> 5) & 63) * w01 + ((p10 >> 5) & 63) * w10 + ((p11 >> 5) & 63) * w11 + 32768) >> 16;
          int b = ((p00 & 31) * w00 + (p01 & 31) * w01 + (p10 & 31) * w10 + (p11 & 31) * w11 + 32768) >> 16;
          out.px[y * SCREEN_W + x] = restyle((Color)((r << 11) | (g << 5) | b), style);
        }
      }
    }
  out.missing = missing;
  out.version++;
  out.ready = true;
  return missing;
}

}  // namespace rm
