// RadarMASH - open firmware for the GeekMagic Radar 360.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Map underlay. Builds a north-up picture centred on home from standard 256-pixel web map tiles ("slippy map" tiles, as
// served by OpenStreetMap and most other providers). Portable: fetching and decoding tiles is left to a TileSource, which
// the simulator and the device each implement in their own way.
#pragma once
#include "canvas.h"
#include <stdint.h>

namespace rm {

constexpr int TILE_SIZE = 256;
constexpr int MAX_ZOOM = 19;

// Web Mercator: position in pixels on the whole-world map at a zoom level (the world is 256 * 2^zoom pixels square).
double mercatorX(double lon, int zoom);
double mercatorY(double lat, int zoom);

// The zoom level whose tiles are closest to one tile pixel per screen pixel for this radar range. `detail` adds that
// many zoom levels: each one doubles the detail drawn (and halves the size of the map's own labels), at four times the
// tiles to download.
int zoomForRange(double lat, float range_km, int detail = 0);

// How tile colours are turned into the underlay.
enum class MapStyle : uint8_t {
  AsIs,       // use the tile's own colours (for sources that are already dark)
  Dark,       // grey, inverted: turns an ordinary light map into a dark one
  Green,      // as Dark, tinted to match the scope
  Count
};

struct TileSource {
  virtual ~TileSource() {}
  // 256x256 RGB565 pixels for this tile, valid until the next call, or nullptr if it is not available.
  virtual const uint16_t *tile(int zoom, int x, int y) = 0;
};

struct MapImage {
  uint16_t px[SCREEN_W * SCREEN_H];
  bool ready = false;       // holds a picture worth drawing
  int zoom = 0;
  int missing = 0;          // tiles the source could not supply last time
  uint32_t version = 0;     // goes up each time the picture is rebuilt
};

// Fill `out` for this home and range. Returns the number of tiles that were missing (their area is left black).
int composeMap(MapImage &out, TileSource &src, double home_lat, double home_lon, float range_km, MapStyle style, int detail = 0);

}  // namespace rm
