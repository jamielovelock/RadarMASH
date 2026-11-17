// RadarMASH - open firmware for the GeekMagic Radar 360.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "feed.h"
#include "json.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace rm {

static bool startsWith(const char *s, const char *prefix) { return strncmp(s, prefix, strlen(prefix)) == 0; }
// An entry matches as a prefix, or exactly if it ends in '$' (so "C17$" is the C-17 but not the Cessna C172).
static bool anyPrefix(const char *s, const char *const *list) {
  for (; *list; list++) {
    size_t n = strlen(*list);
    if ((*list)[n - 1] == '$' ? (strlen(s) == n - 1 && strncmp(s, *list, n - 1) == 0) : startsWith(s, *list)) return true;
  }
  return false;
}

// ICAO type code prefixes, used when the feed gives no category. Not exhaustive: unknown types get the plain arrowhead.
static const char *const HELI_TYPES[] = {"EC", "H160", "H175", "H60", "H64", "H47", "H53", "AS3", "AS5", "AS6", "R22", "R44", "R66", "B06",
                                         "B40", "B41", "B42", "B47", "A109", "A119", "A139", "A169", "A189", "A149", "S61", "S76", "S92",
                                         "MD52", "MD60", "EXPL", "G2CA", "LYNX", "PUMA", "NH90", "EH10", "AW", nullptr};
static const char *const NARROW_TYPES[] = {"A19", "A20", "A21", "A318", "A319", "A32", "B37", "B38", "B39", "B73", "B75", "B71", "B72", "BCS",
                                           "E17", "E19", "E29", "E75", "CRJ", "RJ", "MD8", "MD9", "F70", "F100", "B46", "SU95", "C919", "T204",
                                           "A22", "DC9", nullptr};
static const char *const WIDE_TYPES[] = {"A30", "A31", "A33", "A34", "A35", "A38", "B74", "B76", "B77", "B78", "MD11", "DC10", "IL96", "IL86",
                                         "IL76", "A124", "A225", "A400", "C17$", "C5M", "C130", "C30J", "L101", "B52", "K35", "KC", nullptr};
static const char *const LIGHT_TYPES[] = {"C1", "C2", "C3", "C4", "C5", "C6", "C7", "P28", "PA", "SR2", "DA4", "DA6", "DR4", "RV", "G115", "TB",
                                          "M20", "BE", "B350", "PC12", "PC24", "TBM", "AT4", "AT7", "DH8", "SF34", "E110", "E120", "L410", "DHC",
                                          "LJ", "H25", "F2TH", "F900", "FA", "GL", "CL", "GLID", "ULAC", "TUCA", nullptr};

Shape shapeFor(const char *category, const char *type, int db_flags) {
  if (db_flags & 1) return Shape::Military;
  if (category[0] == 'A') {
    switch (category[1]) {
      case '7': return Shape::Heli;
      case '5': return Shape::Wide;                       // heavy, over 136 t
      case '3': case '4': case '6': return anyPrefix(type, WIDE_TYPES) ? Shape::Wide : Shape::Narrow;
      case '1': case '2': return Shape::Light;            // up to about 34 t: light aircraft, turboprops, small jets
      default: break;
    }
  }
  if (category[0] == 'B') return Shape::Light;            // gliders, balloons, ultralights and the like
  if (!type[0]) return Shape::Unknown;
  if (anyPrefix(type, HELI_TYPES)) return Shape::Heli;
  if (anyPrefix(type, NARROW_TYPES)) return Shape::Narrow;   // before WIDE so A318-A321 do not match the A31 wide-body prefix
  if (anyPrefix(type, WIDE_TYPES)) return Shape::Wide;
  if (anyPrefix(type, LIGHT_TYPES)) return Shape::Light;
  return Shape::Unknown;
}

static void trim(char *s) {
  size_t n = strlen(s);
  while (n && s[n - 1] == ' ') s[--n] = 0;
  size_t lead = strspn(s, " ");
  if (lead) memmove(s, s + lead, n - lead + 1);
}

static bool str(json::Span obj, const char *key, char *out, size_t n) {
  json::Span v;
  out[0] = 0;
  if (!json::member(obj, key, v) || !json::string(v, out, n)) return false;
  trim(out);
  return out[0] != 0;
}
static bool num(json::Span obj, const char *key, double &out) {
  json::Span v;
  return json::member(obj, key, v) && json::number(v, out) && isfinite(out);
}

int parseAircraft(const char *text, size_t len, double home_lat, double home_lon, bool include_ground, Aircraft *out, int max) {
  json::Span root = json::parse(text, len), list;
  if (!json::isObject(root)) return -1;
  if (!json::member(root, "ac", list) && !json::member(root, "aircraft", list)) return -1;
  if (!json::isArray(list)) return -1;

  const double kx = 111.32 * cos(home_lat * 3.14159265358979 / 180.0), ky = 110.57;
  int count = 0;
  const char *it = nullptr;
  json::Span item;
  while (json::element(list, it, item)) {
    if (!json::isObject(item)) continue;
    double lat, lon, v;
    if (!num(item, "lat", lat) || !num(item, "lon", lon) || lat < -90 || lat > 90 || lon < -180 || lon > 180) continue;

    Aircraft a;
    char category[4];
    str(item, "category", category, sizeof category);
    str(item, "t", a.type, sizeof a.type);
    if (category[0] == 'C' || strcmp(a.type, "TWR") == 0) continue;      // vehicles, masts and control towers

    json::Span alt;
    if (json::member(item, "alt_baro", alt)) {
      if (json::number(alt, v)) a.alt_ft = (int)(v < -2000 ? -2000 : v > 99900 ? 99900 : v);
      else a.on_ground = true;                                          // the feed sends the word "ground"
    } else if (num(item, "alt_geom", v)) a.alt_ft = (int)(v < -2000 ? -2000 : v > 99900 ? 99900 : v);
    if (a.on_ground && !include_ground) continue;

    str(item, "hex", a.hex, sizeof a.hex);
    if (!str(item, "flight", a.callsign, sizeof a.callsign) && !str(item, "r", a.callsign, sizeof a.callsign))
      snprintf(a.callsign, sizeof a.callsign, "%s", a.hex);
    if (num(item, "gs", v)) a.speed_kt = (float)(v < 0 ? 0 : v > 9999 ? 9999 : v);
    if (num(item, "baro_rate", v) || num(item, "geom_rate", v)) a.climb_fpm = (int16_t)(v < -30000 ? -30000 : v > 30000 ? 30000 : v);
    if (num(item, "track", v) || num(item, "true_heading", v) || num(item, "mag_heading", v)) a.track_deg = (float)fmod(fmod(v, 360.0) + 360.0, 360.0);
    double flags = 0;
    num(item, "dbFlags", flags);
    a.shape = shapeFor(category, a.type, (int)flags);
    a.lat = lat; a.lon = lon;

    if (count < max) { out[count++] = a; continue; }
    // full: keep whichever is nearer home, this one or the farthest we hold
    auto dist2 = [&](const Aircraft &q) { double e = (q.lon - home_lon) * kx, n = (q.lat - home_lat) * ky; return e * e + n * n; };
    int far = 0;
    for (int i = 1; i < max; i++) if (dist2(out[i]) > dist2(out[far])) far = i;
    if (max > 0 && dist2(a) < dist2(out[far])) out[far] = a;
  }
  return count;
}

RouteResult parseRoute(const char *text, size_t len, RouteInfo &out) {
  json::Span root = json::parse(text, len), resp, route, v;
  if (!json::isObject(root) || !json::member(root, "response", resp)) return RouteResult::Bad;
  if (!json::isObject(resp)) return RouteResult::Unknown;               // "unknown callsign"
  if (!json::member(resp, "flightroute", route) || !json::isObject(route)) return RouteResult::Unknown;
  out = RouteInfo();
  if (json::member(route, "airline", v)) str(v, "name", out.airline, sizeof out.airline);
  bool o_pos = false, d_pos = false;
  if (json::member(route, "origin", v)) {
    if (!str(v, "iata_code", out.origin, sizeof out.origin)) str(v, "icao_code", out.origin, sizeof out.origin);
    o_pos = num(v, "latitude", out.o_lat) && num(v, "longitude", out.o_lon);
  }
  if (json::member(route, "destination", v)) {
    if (!str(v, "iata_code", out.dest, sizeof out.dest)) str(v, "icao_code", out.dest, sizeof out.dest);
    d_pos = num(v, "latitude", out.d_lat) && num(v, "longitude", out.d_lon);
  }
  out.have_pos = o_pos && d_pos;
  return RouteResult::Found;
}

// Great-circle distance in km, and the initial bearing from one point to another in degrees.
static double distanceKm(double lat1, double lon1, double lat2, double lon2) {
  const double R = 6371.0, k = 3.14159265358979 / 180.0;
  double a = sin((lat2 - lat1) * k / 2), b = sin((lon2 - lon1) * k / 2);
  double h = a * a + cos(lat1 * k) * cos(lat2 * k) * b * b;
  return 2 * R * asin(sqrt(h > 1 ? 1 : h));
}
static double bearingDeg(double lat1, double lon1, double lat2, double lon2) {
  const double k = 3.14159265358979 / 180.0;
  double y = sin((lon2 - lon1) * k) * cos(lat2 * k), x = cos(lat1 * k) * sin(lat2 * k) - sin(lat1 * k) * cos(lat2 * k) * cos((lon2 - lon1) * k);
  return fmod(atan2(y, x) / k + 360.0, 360.0);
}

RouteFit routeFit(const RouteInfo &r, double lat, double lon, float track_deg, int climb_fpm) {
  if (!r.have_pos) return RouteFit::CannotTell;
  double whole = distanceKm(r.o_lat, r.o_lon, r.d_lat, r.d_lon);
  double from = distanceKm(r.o_lat, r.o_lon, lat, lon), to = distanceKm(lat, lon, r.d_lat, r.d_lon);
  // Out and back to the same airport (training, survey, air ambulance): all that can be said is that it should not be
  // far from home.
  if (whole < 50.0) return from <= 600.0 ? RouteFit::Fits : RouteFit::DoesNotFit;
  // Going via the aircraft's position should not add much to the trip. The allowance covers routes that bend round
  // airspace and aircraft being steered about near an airport.
  if (from + to > whole * 1.15 + 100.0) return RouteFit::DoesNotFit;
  // Well away from both airports an aircraft points roughly at where it is going.
  if (from > 200.0 && to > 200.0) {
    double off = fabs(bearingDeg(lat, lon, r.d_lat, r.d_lon) - track_deg);
    if (off > 180) off = 360 - off;
    if (off > 100) return RouteFit::DoesNotFit;
  }
  // Near the start of a long trip an aircraft is climbing, and near the end it is descending. One that is coming down
  // close to its supposed origin is really arriving from somewhere else (and the reverse).
  if (from < 250.0 && to > 300.0 && climb_fpm < -600) return RouteFit::DoesNotFit;
  if (to < 250.0 && from > 300.0 && climb_fpm > 600) return RouteFit::DoesNotFit;
  return RouteFit::Fits;
}

static bool icaoCode(const char *s, size_t n, char out[5]) {
  if (n < 3 || n > 4) return false;
  for (size_t i = 0; i < n; i++) if (!((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9'))) return false;
  memcpy(out, s, n); out[n] = 0;
  return true;
}

bool parseHexdbRoute(const char *text, size_t len, char origin[5], char dest[5]) {
  origin[0] = 0; dest[0] = 0;
  while (len && (text[len - 1] == '\n' || text[len - 1] == '\r' || text[len - 1] == ' ')) len--;
  const char *dash = (const char *)memchr(text, '-', len);
  if (!dash) return false;                                              // "n/a" for a callsign it does not know
  char o[5], d[5];
  if (!icaoCode(text, (size_t)(dash - text), o) || !icaoCode(dash + 1, len - (size_t)(dash - text) - 1, d)) return false;
  memcpy(origin, o, 5); memcpy(dest, d, 5);
  return true;
}

bool parseHexdbAirport(const char *text, size_t len, char iata[5], double &lat, double &lon) {
  json::Span root = json::parse(text, len);
  double la, lo;
  if (!json::isObject(root) || !num(root, "latitude", la) || !num(root, "longitude", lo) || la < -90 || la > 90 || lo < -180 || lo > 180) return false;
  if (!str(root, "iata", iata, 5)) str(root, "icao", iata, 5);          // a few airfields have no IATA code
  lat = la; lon = lo;
  return true;
}

bool lookupableCallsign(const char *c) {
  size_t n = strlen(c);
  if (n < 3 || n > 8) return false;
  bool digit = false;
  for (size_t i = 0; i < n; i++) {
    bool up = c[i] >= 'A' && c[i] <= 'Z', dg = c[i] >= '0' && c[i] <= '9';
    if (!up && !dg) return false;
    digit = digit || dg;
  }
  return digit;                                                         // airline flights have a number; bare words do not
}

int feedRadiusNm(float range_km) {
  int nm = (int)ceilf(range_km / 1.852f * 1.05f);
  return nm < 1 ? 1 : nm > 250 ? 250 : nm;                              // the services cap the radius at 250
}

}  // namespace rm
