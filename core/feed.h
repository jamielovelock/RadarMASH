// RadarMASH - open firmware for the GeekMagic Radar 360.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reading the free aircraft data services. Portable: the caller does the HTTP request and hands the text in.
//  - Positions: the "readsb" JSON format served by api.adsb.lol and opendata.adsb.fi (and many self-hosted receivers).
//  - Routes and airline names: api.adsbdb.com callsign lookups.
#pragma once
#include "radar.h"
#include <stddef.h>

namespace rm {

// Address patterns. {lat} {lon} are the home position, {nm} the radius in nautical miles, {callsign} the flight.
constexpr const char *FEED_ADSB_LOL = "https://api.adsb.lol/v2/point/{lat}/{lon}/{nm}";
constexpr const char *FEED_ADSB_FI = "https://opendata.adsb.fi/api/v3/lat/{lat}/lon/{lon}/dist/{nm}";
constexpr const char *ROUTE_ADSBDB = "https://api.adsbdb.com/v0/callsign/{callsign}";
// A second opinion, asked only when the first has no route or one that does not fit where the aircraft is.
constexpr const char *ROUTE_HEXDB = "https://hexdb.io/callsign-route?callsign={callsign}";
constexpr const char *AIRPORT_HEXDB = "https://hexdb.io/api/v1/airport/icao/{icao}";

// Which silhouette suits an aircraft, from the feed's emitter category ("A1".."A7", "B1"..), its ICAO type code and
// database flags (bit 0 = military). Any of the strings may be empty.
Shape shapeFor(const char *category, const char *type, int db_flags);

// Read a positions reply into `out`. Keeps the `max` aircraft nearest home if there are more. Returns how many were
// stored, or -1 if the text is not a usable reply. Aircraft on the ground are dropped unless include_ground.
int parseAircraft(const char *text, size_t len, double home_lat, double home_lon, bool include_ground, Aircraft *out, int max);

struct RouteInfo {
  char airline[26] = "";
  char airline_icao[4] = "";          // e.g. RYR, when the service gives it
  char origin[5] = "";
  char dest[5] = "";
  bool have_pos = false;              // are the two airports' positions known?
  double o_lat = 0, o_lon = 0, d_lat = 0, d_lon = 0;
};
enum class RouteResult { Found, Unknown, Bad };     // Unknown: the service has no route for this callsign. Bad: try again later.
RouteResult parseRoute(const char *text, size_t len, RouteInfo &out);

// The route services hold what a flight number usually flies, and flight numbers get reused, so some answers are simply
// wrong for the aircraft overhead. This asks whether a route makes sense for an aircraft at this position on this track:
// it should be roughly on the way from one airport to the other; when far from both, heading for the destination; and
// not descending just after its supposed departure or climbing just before its supposed arrival.
enum class RouteFit { Fits, DoesNotFit, CannotTell };
RouteFit routeFit(const RouteInfo &r, double lat, double lon, float track_deg, int climb_fpm = 0);

// The second service's replies: a route as "GCRR-EGCC" (ICAO codes, plain text), and an airport as JSON.
bool parseHexdbRoute(const char *text, size_t len, char origin_icao[5], char dest_icao[5]);
bool parseHexdbAirport(const char *text, size_t len, char iata[5], double &lat, double &lon);

// True if a callsign is safe and sensible to look up (letters and digits only), so feed data never ends up in a URL raw.
bool lookupableCallsign(const char *callsign);

// Nautical-mile radius to ask the feed for, so the whole scope is covered.
int feedRadiusNm(float range_km);

}  // namespace rm
