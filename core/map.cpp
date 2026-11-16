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

int zoomForRange(double lat, float range_km) {
  double screen_m_per_px = (double)range_km * 1000.0 / RADAR_RADIUS;
  double world_m_per_px = 156543.03392 * cos(lat * PI_D / 180.0);   // at zoom 0
  if (!(screen_m_per_px > 0) || !(world_m_per_px > 0)) return 0;
  int z = (int)lround(log2(world_m_per_px / screen_m_per_px));
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

int composeMap(MapImage &out, TileSource &src, double home_lat, double home_lon, float range_km, MapStyle style) {
  const int zoom = zoomForRange(home_lat, range_km);
  const int world_tiles = 1 << zoom;
  const double km_per_px = (double)range_km / RADAR_RADIUS;
  const double km_per_deg_lon = 111.32 * cos(home_lat * PI_D / 180.0);

  // The radar is drawn on a flat grid of kilometres around home, so each screen column is one longitude and each row one
  // latitude. Work the map position out once per column and per row, not per pixel.
  static double gx[SCREEN_W], gy[SCREEN_H];
  for (int x = 0; x < SCREEN_W; x++) gx[x] = mercatorX(home_lon + (x - CENTRE) * km_per_px / km_per_deg_lon, zoom);
  for (int y = 0; y < SCREEN_H; y++) gy[y] = mercatorY(home_lat + (CENTRE - y) * km_per_px / 110.57, zoom);

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
      for (int y = 0; y < SCREEN_H; y++) {
        int py = (int)floor(gy[y]) - ty * TILE_SIZE;
        if (py < 0 || py >= TILE_SIZE) continue;
        float dy = y - CENTRE;
        for (int x = 0; x < SCREEN_W; x++) {
          int px = (int)floor(gx[x]) - tx * TILE_SIZE;
          if (px < 0 || px >= TILE_SIZE) continue;
          float dx = x - CENTRE;
          if (dx * dx + dy * dy > limit) continue;                        // outside the glass
          out.px[y * SCREEN_W + x] = restyle(t[py * TILE_SIZE + px], style);
        }
      }
    }
  out.missing = missing;
  out.version++;
  out.ready = true;
  return missing;
}

}  // namespace rm
