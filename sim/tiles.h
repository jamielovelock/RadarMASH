// RadarMASH - open firmware for the GeekMagic Radar 360.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Simulator tile source: downloads map tiles with curl, keeps them on disk, and decodes them for core/map.
#pragma once
#include "../core/map.h"
#include <map>
#include <string>
#include <vector>

struct SimTiles : rm::TileSource {
  std::string url_template;     // e.g. https://tile.openstreetmap.org/{z}/{x}/{y}.png
  std::string user_agent;       // tile servers want to know which program is asking
  int fetched = 0, failed = 0;  // since start, for the status line
  bool verify_certs = true;     // off: pass -k to curl

  const uint16_t *tile(int zoom, int x, int y) override;

private:
  std::map<std::string, std::vector<uint16_t>> decoded_;   // small in-memory cache
  std::map<std::string, long> failed_at_;                  // do not re-ask for a failed tile straight away
  std::string cacheDir() const;
};
