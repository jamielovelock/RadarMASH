// RadarMASH - open firmware for the GeekMagic Radar 360.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Airline logos. RadarMASH contains none: logos are the airlines' trademarks. If the user gives a web address pattern for
// them (a setting, empty by default), the radar fetches each airline's logo once and keeps it in memory. This file is
// the portable part: what a logo is once loaded, and shrinking a downloaded picture to that size.
#pragma once
#include <stdint.h>

namespace rm {

constexpr int LOGO_SIZE = 40;             // logos are kept as LOGO_SIZE x LOGO_SIZE, however big the original

struct Logo {
  uint16_t px[LOGO_SIZE * LOGO_SIZE];     // colour, RGB565
  uint8_t alpha[LOGO_SIZE * LOGO_SIZE];   // how solid each pixel is, 0 (clear) to 255
};

// Supplied by the simulator and the device. Must answer at once: the logo if it is already loaded, otherwise nullptr
// (and it may start fetching it in the background for next time).
struct LogoSource {
  virtual ~LogoSource() {}
  virtual const Logo *logo(const char *airline_icao) = 0;
};

// Shrink (or enlarge) a picture of w x h pixels, 4 bytes each (red, green, blue, alpha), to fit a Logo, keeping its
// shape and centring it. Returns false if the size is not usable.
bool makeLogo(const uint8_t *rgba, int w, int h, Logo &out);

// The airline a callsign belongs to, when it is in the usual form of three letters then a flight number ("RYR8HV" ->
// "RYR"). Writes an empty string otherwise (registrations, military callsigns and so on).
void airlineFromCallsign(const char *callsign, char out[4]);

}  // namespace rm
