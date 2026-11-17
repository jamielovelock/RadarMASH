// RadarMASH - open firmware for the GeekMagic Radar 360.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../core/feed.h"
#include "../core/json.h"
#include "test.h"
#include <memory>
#include <string.h>
#include <string>
#include <vector>

using namespace rm;

namespace {
// A reply in the shape the live services send, with made-up aircraft around a made-up home at 51.0, -1.0.
const char *const SAMPLE = R"JSON({"ac":[
 {"hex":"4d2386","type":"adsb_icao","flight":"RDR239A ","r":"9H-VUY","t":"B38M","alt_baro":36000,"alt_geom":35850,"gs":374.5,
  "track":277.83,"squawk":"3103","emergency":"none","category":"A3","lat":51.103165,"lon":-1.261305,"nic":8,"seen_pos":0.252,
  "mlat":[],"tisb":[],"messages":7957,"seen":0.1,"rssi":-28.9,"nav_modes":["autopilot","vnav"]},
 {"hex":"425854","type":"adsb_icao_nt","r":"TWR","t":"TWR","alt_baro":"ground","gs":0.0,"lat":51.066357,"lon":-1.090892,"mlat":[],"tisb":[]},
 {"hex":"896451","flight":"HWK20   ","r":"A6-EVL","t":"A388","alt_baro":"ground","gs":15.5,"category":"A5","lat":51.05,"lon":-1.05},
 {"hex":"406b2c","flight":"KLG4JD  ","t":"A359","alt_baro":6825,"gs":285.7,"track":319.12,"category":"A5","lat":50.9,"lon":-0.8},
 {"hex":"43c6f4","flight":"POTTR01 ","t":"A400","alt_baro":14000,"gs":280,"true_heading":350.0,"dbFlags":1,"category":"A5","lat":50.8,"lon":-1.1},
 {"hex":"407f11","r":"G-SWMP","t":"EC35","alt_baro":1200,"gs":120,"track":-45,"category":"A7","lat":51.2,"lon":-1.3},
 {"hex":"4ca9d2","flight":"NOPOS1  ","t":"B738","alt_baro":30000,"gs":400,"track":90},
 {"hex":"3c66b1","t":"C172","alt_geom":2400,"gs":105,"track":95,"lat":51.01,"lon":-1.02},
 {"hex":"a0b1c2","flight":"VAN12   ","t":"GND","category":"C2","alt_baro":"ground","lat":51.06,"lon":-1.09}
],"msg":"No error","now":1794864235501,"total":9,"ctime":1794864235501,"ptime":0})JSON";

const char *const ROUTE = R"JSON({"response":{"flightroute":{"callsign":"RDR239A","callsign_icao":"RDR239A","airline":{"name":"Radar Air","icao":"RDR",
 "iata":"RA","country":"Nowhere"},"origin":{"country_name":"Poland","iata_code":"KRK","icao_code":"EPKK","municipality":"Kraków",
 "name":"Kraków John Paul II International Airport"},"destination":{"iata_code":"DUB","icao_code":"EIDW","name":"Dublin Airport"}}}})JSON";

const Aircraft *byCallsign(const std::vector<Aircraft> &v, int n, const char *cs) {
  for (int i = 0; i < n; i++) if (strcmp(v[i].callsign, cs) == 0) return &v[i];
  return nullptr;
}
int parseSample(std::vector<Aircraft> &out, bool ground = false, int max = MAX_AIRCRAFT) {
  out.assign(MAX_AIRCRAFT, Aircraft());
  return parseAircraft(SAMPLE, strlen(SAMPLE), 51.0, -1.0, ground, out.data(), max);
}
}  // namespace

// ---- JSON reader ----
TEST(json_picks_out_members) {
  const char *t = R"({"a": 1.5, "b": "two", "c": {"d": [10, 20, {"e": -3e2}]}, "f": null, "g": true})";
  json::Span root = json::parse(t, strlen(t)), v, w;
  double d; char s[8];
  CHECK(json::isObject(root));
  CHECK(json::member(root, "a", v) && json::number(v, d) && d == 1.5);
  CHECK(json::member(root, "b", v) && json::string(v, s, sizeof s) && strcmp(s, "two") == 0);
  CHECK(!json::number(v, d));                       // a string is not a number
  CHECK(json::member(root, "c", v) && json::member(v, "d", w) && json::isArray(w));
  const char *it = nullptr; json::Span e; int n = 0; double last = 0;
  while (json::element(w, it, e)) { n++; if (json::number(e, d)) last = d; }
  CHECK(n == 3 && last == 20);
  CHECK(json::member(e, "e", v) && json::number(v, d) && d == -300);
  CHECK(!json::member(root, "zz", v));
  CHECK(!json::member(root, "d", v));               // only looks at the top level
  CHECK(json::member(root, "f", v) && !json::number(v, d) && !json::string(v, s, sizeof s));
}

TEST(json_strings_are_unescaped_and_made_plain) {
  const char *t = R"({"q": "say \"hi\"\\n", "u": "Kraków", "utf": "Zürich Flughafen", "odd": "東京 X", "ctl": "a\tb"})";
  json::Span root = json::parse(t, strlen(t)), v;
  char s[32];
  CHECK(json::member(root, "q", v) && json::string(v, s, sizeof s) && strcmp(s, "say \"hi\"\\n") == 0);
  CHECK(json::member(root, "u", v) && json::string(v, s, sizeof s) && strcmp(s, "Krakow") == 0);
  CHECK(json::member(root, "utf", v) && json::string(v, s, sizeof s) && strcmp(s, "Zurich Flughafen") == 0);
  CHECK(json::member(root, "odd", v) && json::string(v, s, sizeof s) && strcmp(s, "?? X") == 0);
  CHECK(json::member(root, "ctl", v) && json::string(v, s, sizeof s) && strcmp(s, "a b") == 0);
  char tiny[4];
  CHECK(json::member(root, "utf", v) && json::string(v, tiny, sizeof tiny) && strcmp(tiny, "Zur") == 0);   // cut to fit
}

TEST(json_rejects_broken_text) {
  const char *bad[] = {"", "   ", "{", "{\"a\":", "{\"a\":1", "[1,2", "\"unterminated", "{\"a\" 1}", "{a:1}", "nul"};
  for (const char *t : bad) {
    json::Span root = json::parse(t, strlen(t)), v;
    CHECK(!json::member(root, "a", v) || !json::isObject(root) || strcmp(t, "{\"a\":1") != 0);
  }
  json::Span none = json::parse(nullptr, 0), v;
  double d; char s[4]; const char *it = nullptr;
  CHECK(!none.ok() && !json::member(none, "a", v) && !json::element(none, it, v) && !json::number(none, d) && !json::string(none, s, 4));
  std::string deep(200000, '[');                    // absurd nesting must not overflow the stack
  CHECK(!json::parse(deep.data(), deep.size()).ok());
}

// ---- positions ----
TEST(feed_reads_airborne_aircraft) {
  std::vector<Aircraft> ac;
  int n = parseSample(ac);
  CHECK(n == 5);                                    // tower, vehicle, parked A380 and the one with no position are left out
  const Aircraft *a = byCallsign(ac, n, "RDR239A");
  CHECK(a != nullptr);
  if (!a) return;
  CHECK(strcmp(a->hex, "4d2386") == 0 && strcmp(a->type, "B38M") == 0);
  CHECK(a->alt_ft == 36000 && !a->on_ground);
  CHECK_NEAR(a->speed_kt, 374.5, 0.01); CHECK_NEAR(a->track_deg, 277.83, 0.01);
  CHECK_NEAR(a->lat, 51.103165, 1e-6); CHECK_NEAR(a->lon, -1.261305, 1e-6);
  CHECK(a->shape == Shape::Narrow);
  CHECK(a->airline[0] == 0 && a->origin[0] == 0);   // routes come from the other service
}

TEST(feed_fills_gaps_sensibly) {
  std::vector<Aircraft> ac;
  int n = parseSample(ac);
  const Aircraft *heli = byCallsign(ac, n, "G-SWMP");   // no flight number: falls back to the registration
  CHECK(heli && heli->shape == Shape::Heli);
  if (heli) CHECK_NEAR(heli->track_deg, 315, 0.01);     // a negative track is brought into 0..360
  const Aircraft *cessna = byCallsign(ac, n, "3c66b1"); // neither flight nor registration: falls back to the hex address
  CHECK(cessna && cessna->alt_ft == 2400 && cessna->shape == Shape::Light);   // height from alt_geom, shape from the type
  const Aircraft *mil = byCallsign(ac, n, "POTTR01");
  CHECK(mil && mil->shape == Shape::Military);
  if (mil) CHECK_NEAR(mil->track_deg, 350, 0.01);       // no track: uses the heading
  CHECK(byCallsign(ac, n, "KLG4JD") && byCallsign(ac, n, "KLG4JD")->shape == Shape::Wide);
}

TEST(feed_can_include_aircraft_on_the_ground) {
  std::vector<Aircraft> ac;
  int n = parseSample(ac, true);
  CHECK(n == 6);
  const Aircraft *a = byCallsign(ac, n, "HWK20");
  CHECK(a && a->on_ground && a->alt_ft == 0);
  CHECK(!byCallsign(ac, n, "TWR") && !byCallsign(ac, n, "VAN12"));   // towers and vehicles never
}

TEST(feed_keeps_the_nearest_when_there_are_too_many) {
  std::vector<Aircraft> ac;
  int n = parseSample(ac, false, 2);
  CHECK(n == 2);
  CHECK(byCallsign(ac, n, "3c66b1") != nullptr);     // 1.8 km from home
  CHECK(byCallsign(ac, n, "KLG4JD") != nullptr);     // 18 km; the rest are further
  CHECK(parseSample(ac, false, 0) == 0);
}

TEST(feed_survives_being_cut_short_anywhere) {
  // A dropped connection can end the reply at any byte. Every prefix must be handled without reading out of bounds.
  std::vector<Aircraft> ac(MAX_AIRCRAFT);
  size_t len = strlen(SAMPLE);
  int usable = 0;
  for (size_t cut = 0; cut < len; cut++) {
    std::string part(SAMPLE, cut);                   // its own allocation, so an over-read is caught by the sanitizer
    int n = parseAircraft(part.data(), part.size(), 51, -1, true, ac.data(), MAX_AIRCRAFT);
    CHECK(n >= -1 && n <= 6);
    usable += n >= 0;
  }
  CHECK(usable == 0);                                // an incomplete reply is never mistaken for a short list
  const char *junk[] = {"<html>502 Bad Gateway</html>", "{\"ac\":5}", "{\"msg\":\"rate limited\"}", "[]", "{\"ac\":[1,\"x\",null,{}]}"};
  CHECK(parseAircraft(junk[0], strlen(junk[0]), 51, -1, true, ac.data(), 8) == -1);
  CHECK(parseAircraft(junk[1], strlen(junk[1]), 51, -1, true, ac.data(), 8) == -1);
  CHECK(parseAircraft(junk[2], strlen(junk[2]), 51, -1, true, ac.data(), 8) == -1);
  CHECK(parseAircraft(junk[3], strlen(junk[3]), 51, -1, true, ac.data(), 8) == -1);
  CHECK(parseAircraft(junk[4], strlen(junk[4]), 51, -1, true, ac.data(), 8) == 0);
}

TEST(feed_clamps_wild_values) {
  const char *t = R"({"ac":[{"hex":"abcdef0123456789","flight":"AVERYLONGCALLSIGN","t":"TOOLONG","alt_baro":9e9,"gs":-5,"track":1e6,"lat":51,"lon":-1},
                           {"hex":"bad","lat":91,"lon":0},{"hex":"bad2","lat":1e400,"lon":0}]})";
  std::vector<Aircraft> ac(4);
  CHECK(parseAircraft(t, strlen(t), 51, -1, true, ac.data(), 4) == 1);
  CHECK(strlen(ac[0].hex) == 6 && strlen(ac[0].callsign) == 8 && strlen(ac[0].type) == 4);
  CHECK(ac[0].alt_ft == 99900 && ac[0].speed_kt == 0 && ac[0].track_deg >= 0 && ac[0].track_deg < 360);
}

TEST(shapes_from_category_and_type) {
  CHECK(shapeFor("A1", "C172", 0) == Shape::Light);
  CHECK(shapeFor("A2", "DH8D", 0) == Shape::Light);
  CHECK(shapeFor("A3", "A320", 0) == Shape::Narrow);
  CHECK(shapeFor("A3", "A332", 0) == Shape::Wide);       // category says large, type says twin-aisle
  CHECK(shapeFor("A5", "B77W", 0) == Shape::Wide);
  CHECK(shapeFor("A7", "EC35", 0) == Shape::Heli);
  CHECK(shapeFor("A3", "B738", 1) == Shape::Military);   // the military flag wins
  CHECK(shapeFor("B1", "GLID", 0) == Shape::Light);
  // no category: decided by type alone
  CHECK(shapeFor("", "A319", 0) == Shape::Narrow && shapeFor("", "A21N", 0) == Shape::Narrow && shapeFor("", "B38M", 0) == Shape::Narrow);
  CHECK(shapeFor("", "A306", 0) == Shape::Wide && shapeFor("", "B789", 0) == Shape::Wide && shapeFor("", "C17", 0) == Shape::Wide);
  CHECK(shapeFor("", "C172", 0) == Shape::Light && shapeFor("", "P28A", 0) == Shape::Light && shapeFor("", "H25B", 0) == Shape::Light);
  CHECK(shapeFor("", "EC45", 0) == Shape::Heli && shapeFor("", "A139", 0) == Shape::Heli && shapeFor("", "R44", 0) == Shape::Heli);
  CHECK(shapeFor("", "MD90", 0) == Shape::Narrow);
  CHECK(shapeFor("", "", 0) == Shape::Unknown && shapeFor("", "ZZZZ", 0) == Shape::Unknown);
}

// ---- routes ----
TEST(route_lookup_is_read) {
  RouteInfo r;
  CHECK(parseRoute(ROUTE, strlen(ROUTE), r) == RouteResult::Found);
  CHECK(strcmp(r.airline, "Radar Air") == 0 && strcmp(r.origin, "KRK") == 0 && strcmp(r.dest, "DUB") == 0);
  const char *unknown = "{\"response\":\"unknown callsign\"}";
  CHECK(parseRoute(unknown, strlen(unknown), r) == RouteResult::Unknown);
  const char *bad[] = {"", "<html>", "{\"error\":1}", "{\"response\":"};
  for (const char *t : bad) CHECK(parseRoute(t, strlen(t), r) == RouteResult::Bad);
  for (size_t cut = 0; cut < strlen(ROUTE); cut++) {
    std::string part(ROUTE, cut);
    CHECK(parseRoute(part.data(), part.size(), r) != RouteResult::Found);
  }
  const char *long_name = R"({"response":{"flightroute":{"airline":{"name":"An Extremely Long Airline Name Incorporated"},"origin":{"icao_code":"EGLL"},"destination":{}}}})";
  CHECK(parseRoute(long_name, strlen(long_name), r) == RouteResult::Found);
  CHECK(strlen(r.airline) == sizeof r.airline - 1 && strcmp(r.origin, "EGLL") == 0 && r.dest[0] == 0);   // no IATA code: falls back to ICAO
}

// ---- does a route fit the aircraft? ----
namespace {
RouteInfo route(double olat, double olon, double dlat, double dlon) {
  RouteInfo r; r.have_pos = true; r.o_lat = olat; r.o_lon = olon; r.d_lat = dlat; r.d_lon = dlon; return r;
}
const double SKG[2] = {40.5197, 22.9709}, LGW[2] = {51.1481, -0.19028}, ACE[2] = {28.9455, -13.6052}, MAN[2] = {53.3537, -2.27495};
const double LHR[2] = {51.4706, -0.461941}, JFK[2] = {40.6398, -73.7789};
}  // namespace

TEST(route_positions_are_read_from_the_first_service) {
  const char *t = R"({"response":{"flightroute":{"airline":{"name":"Radar Air"},"origin":{"iata_code":"SKG","latitude":40.5197,"longitude":22.9709},
                      "destination":{"iata_code":"LGW","latitude":51.1481,"longitude":-0.19028}}}})";
  RouteInfo r;
  CHECK(parseRoute(t, strlen(t), r) == RouteResult::Found && r.have_pos);
  CHECK_NEAR(r.o_lat, 40.5197, 1e-6); CHECK_NEAR(r.d_lon, -0.19028, 1e-6);
  CHECK(parseRoute(ROUTE, strlen(ROUTE), r) == RouteResult::Found && !r.have_pos);   // the older sample has no positions
  CHECK(routeFit(r, 51, -1, 90) == RouteFit::CannotTell);
}

TEST(a_wrong_route_is_spotted_from_where_the_aircraft_is) {
  // The real case: an aircraft arriving at Manchester from the south, which one service had as Thessaloniki to Gatwick.
  CHECK(routeFit(route(SKG[0], SKG[1], LGW[0], LGW[1]), 53.40, -2.10, 330) == RouteFit::DoesNotFit);
  CHECK(routeFit(route(ACE[0], ACE[1], MAN[0], MAN[1]), 53.40, -2.10, 330) == RouteFit::Fits);
  // and from further out: over mid-Wales heading north-east for Manchester
  CHECK(routeFit(route(ACE[0], ACE[1], MAN[0], MAN[1]), 52.3, -3.6, 40) == RouteFit::Fits);
  CHECK(routeFit(route(SKG[0], SKG[1], LGW[0], LGW[1]), 52.3, -3.6, 40) == RouteFit::DoesNotFit);
}

TEST(a_right_route_is_not_rejected) {
  RouteInfo lhr_jfk = route(LHR[0], LHR[1], JFK[0], JFK[1]);
  CHECK(routeFit(lhr_jfk, 51.6, -2.5, 280) == RouteFit::Fits);          // climbing out westbound over Bristol
  CHECK(routeFit(lhr_jfk, 53.5, -20.0, 270) == RouteFit::Fits);         // mid-Atlantic, north of the direct line
  CHECK(routeFit(lhr_jfk, 51.3, -0.6, 90) == RouteFit::Fits);           // just departed, still pointing the wrong way
  RouteInfo ace_man = route(ACE[0], ACE[1], MAN[0], MAN[1]);
  CHECK(routeFit(ace_man, 53.6, -2.6, 140) == RouteFit::Fits);          // being steered round for the approach
  CHECK(routeFit(route(MAN[0], MAN[1], MAN[0], MAN[1]), 53.5, -2.0, 10) == RouteFit::Fits);   // a local flight out and back
}

TEST(out_and_back_flights_fit_near_home) {
  // Real case: a transport on a training flight from and to Brize Norton, 190 km away, was being rejected.
  RouteInfo bzz = route(51.75, -1.5836, 51.75, -1.5836);
  CHECK(routeFit(bzz, 53.4, -2.3, 266) == RouteFit::Fits);
  CHECK(routeFit(bzz, 51.76, -1.6, 90) == RouteFit::Fits);
  CHECK(routeFit(bzz, 35.0, 20.0, 90) == RouteFit::DoesNotFit);         // but not on the other side of Europe
}

TEST(climb_and_descent_tell_arrivals_from_departures) {
  // Real case: a freighter descending eastbound near Manchester. One service said East Midlands to Cologne (it would be
  // climbing), the other Philadelphia to East Midlands (it would be descending).
  const double EMA[2] = {52.8311, -1.32806}, CGN[2] = {50.8659, 7.14274}, PHL[2] = {39.8719, -75.2411};
  RouteInfo dep = route(EMA[0], EMA[1], CGN[0], CGN[1]), arr = route(PHL[0], PHL[1], EMA[0], EMA[1]);
  CHECK(routeFit(dep, 53.3, -2.0, 93, -1500) == RouteFit::DoesNotFit);
  CHECK(routeFit(arr, 53.3, -2.0, 93, -1500) == RouteFit::Fits);
  CHECK(routeFit(dep, 52.9, -1.0, 93, 2000) == RouteFit::Fits);          // and the departure fits when it really is one
  CHECK(routeFit(arr, 52.9, -1.0, 93, 2000) == RouteFit::DoesNotFit);
  CHECK(routeFit(dep, 53.3, -2.0, 93, 0) == RouteFit::Fits);             // level, or rate not reported: no opinion
  CHECK(routeFit(dep, 53.3, -2.0, 93, -300) == RouteFit::Fits);          // a gentle drift down is not a descent
  RouteInfo hop = route(MAN[0], MAN[1], 54.6575, -6.2158);               // a short hop: up and down are both normal throughout
  CHECK(routeFit(hop, 53.6, -3.2, 300, -1200) == RouteFit::Fits);
}

TEST(climb_rate_is_read_from_the_feed) {
  const char *t = R"({"ac":[{"hex":"a1","flight":"UP1","lat":51,"lon":-1,"alt_baro":9000,"baro_rate":1856},
                           {"hex":"a2","flight":"DN1","lat":51,"lon":-1,"alt_baro":9000,"geom_rate":-1216},
                           {"hex":"a3","flight":"LV1","lat":51,"lon":-1,"alt_baro":9000},
                           {"hex":"a4","flight":"MAD1","lat":51,"lon":-1,"alt_baro":9000,"baro_rate":-9e9}]})";
  std::vector<Aircraft> ac(8);
  int n = parseAircraft(t, strlen(t), 51, -1, false, ac.data(), 8);
  CHECK(n == 4);
  CHECK(byCallsign(ac, n, "UP1")->climb_fpm == 1856 && byCallsign(ac, n, "DN1")->climb_fpm == -1216);
  CHECK(byCallsign(ac, n, "LV1")->climb_fpm == 0 && byCallsign(ac, n, "MAD1")->climb_fpm == -30000);
}

TEST(a_route_pointing_the_other_way_does_not_fit) {
  RouteInfo lhr_jfk = route(LHR[0], LHR[1], JFK[0], JFK[1]);
  CHECK(routeFit(lhr_jfk, 53.5, -20.0, 90) == RouteFit::DoesNotFit);    // mid-Atlantic but heading east: it is the return flight
  CHECK(routeFit(lhr_jfk, 40.0, 10.0, 270) == RouteFit::DoesNotFit);    // over the Mediterranean: nowhere near
}

TEST(second_service_replies_are_read) {
  char o[5], d[5];
  CHECK(parseHexdbRoute("GCRR-EGCC", 9, o, d) && strcmp(o, "GCRR") == 0 && strcmp(d, "EGCC") == 0);
  CHECK(parseHexdbRoute("GCRR-EGCC\n", 10, o, d) && strcmp(d, "EGCC") == 0);
  const char *bad[] = {"n/a", "", "GCRR", "GCRR-", "-EGCC", "gcrr-egcc", "GCRR-EGCC-EGLL", "TOOLONG-EGCC", "<html>error</html>", "GC RR-EGCC"};
  for (const char *b : bad) { CHECK(!parseHexdbRoute(b, strlen(b), o, d)); CHECK(o[0] == 0 && d[0] == 0); }

  const char *ap = R"({"country_code": "GB", "region_name": "England", "iata": "MAN", "icao": "EGCC", "airport": "Manchester Airport", "latitude": 53.3537, "longitude": -2.27495})";
  char iata[5]; double lat = 0, lon = 0;
  CHECK(parseHexdbAirport(ap, strlen(ap), iata, lat, lon) && strcmp(iata, "MAN") == 0);
  CHECK_NEAR(lat, 53.3537, 1e-6); CHECK_NEAR(lon, -2.27495, 1e-6);
  const char *no_iata = R"({"iata": "", "icao": "EGCB", "latitude": 53.47, "longitude": -2.39})";
  CHECK(parseHexdbAirport(no_iata, strlen(no_iata), iata, lat, lon) && strcmp(iata, "EGCB") == 0);
  const char *junk[] = {"", "n/a", "{}", "{\"latitude\": 99, \"longitude\": 0}", "{\"latitude\": \"x\", \"longitude\": 1}", "[1,2]"};
  for (const char *j : junk) CHECK(!parseHexdbAirport(j, strlen(j), iata, lat, lon));
  for (size_t cut = 0; cut < strlen(ap); cut++) { std::string part(ap, cut); CHECK(!parseHexdbAirport(part.data(), part.size(), iata, lat, lon)); }
}

TEST(only_plain_callsigns_are_looked_up) {
  CHECK(lookupableCallsign("RYR239A") && lookupableCallsign("BAW1") && lookupableCallsign("N123AB"));
  CHECK(!lookupableCallsign("G-SWMP") && !lookupableCallsign("") && !lookupableCallsign("AB"));
  CHECK(!lookupableCallsign("A/../B1") && !lookupableCallsign("AB 12") && !lookupableCallsign("abc123") && !lookupableCallsign("TOOLONG123"));
  CHECK(!lookupableCallsign("POTTR"));               // no number: not an airline flight
}

TEST(aircraft_limit_defaults_to_ten) {
  Settings s;
  CHECK(s.max_aircraft == 10);
  CHECK(s.max_aircraft <= MAX_AIRCRAFT);
}

TEST(feed_radius_covers_the_scope) {
  CHECK(feedRadiusNm(40) == 23);                     // 40 km is 21.6 nm, plus a little
  CHECK(feedRadiusNm(160) >= 87 && feedRadiusNm(160) <= 92);
  CHECK(feedRadiusNm(0) == 1 && feedRadiusNm(100000) == 250);
}

// ---- merging updates into the radar ----
TEST(update_carries_trails_routes_and_highlight) {
  std::unique_ptr<Radar> r(new Radar());
  r->home_lat = 51; r->home_lon = -1; r->set.auto_cycle = false;
  std::vector<Aircraft> ac;
  int n = parseSample(ac);
  r->update(ac.data(), n, 1000);
  CHECK(r->count == 5);
  int rdr = -1;
  for (int i = 0; i < r->count; i++) if (strcmp(r->ac[i].callsign, "RDR239A") == 0) rdr = i;
  CHECK(rdr >= 0);
  if (rdr < 0) return;
  CHECK(r->ac[rdr].trail_len == 0 && r->ac[rdr].live);
  snprintf(r->ac[rdr].airline, sizeof r->ac[rdr].airline, "Radar Air");      // as if the route lookup had come back
  snprintf(r->ac[rdr].origin, sizeof r->ac[rdr].origin, "KRK");
  snprintf(r->ac[rdr].dest, sizeof r->ac[rdr].dest, "DUB");
  r->selected = rdr;

  // next reply: same aircraft a little further west, in a different order, and one has gone
  std::vector<Aircraft> next;
  n = parseSample(next);
  std::vector<Aircraft> shuffled;
  for (int i = n - 1; i >= 0; i--) if (strcmp(next[i].callsign, "KLG4JD") != 0) shuffled.push_back(next[i]);
  for (Aircraft &a : shuffled) a.lon -= 0.02;
  r->update(shuffled.data(), (int)shuffled.size(), 6000);
  CHECK(r->count == 4);
  CHECK(r->selected >= 0 && strcmp(r->ac[r->selected].callsign, "RDR239A") == 0);   // the highlight followed the aircraft
  const Aircraft &a = r->ac[r->selected];
  CHECK(strcmp(a.airline, "Radar Air") == 0 && strcmp(a.origin, "KRK") == 0 && strcmp(a.dest, "DUB") == 0);
  CHECK(a.trail_len == 1);
  CHECK_NEAR(a.trail_lon[0], -1.261305, 1e-4);       // the trail starts where it was last reported

  for (int k = 0; k < 40; k++) {                     // many updates: the trail fills up and stops growing
    for (Aircraft &s : shuffled) s.lon -= 0.02;
    r->update(shuffled.data(), (int)shuffled.size(), 11000 + k * 5000);
  }
  CHECK(r->ac[r->selected].trail_len == TRAIL_POINTS);

  std::vector<Aircraft> still = shuffled;            // not moving: no new trail points, and none lost
  r->update(still.data(), (int)still.size(), 300000);
  CHECK(r->ac[r->selected].trail_len == TRAIL_POINTS);

  r->update(still.data(), 0, 305000);                // empty sky
  CHECK(r->count == 0 && r->selected == -1);
}

TEST(live_aircraft_glide_between_updates) {
  std::unique_ptr<Radar> r(new Radar());
  r->home_lat = 51; r->home_lon = -1; r->set.auto_cycle = false;
  Aircraft a;
  snprintf(a.hex, sizeof a.hex, "abc123"); snprintf(a.callsign, sizeof a.callsign, "EAST1");
  a.lat = 51; a.lon = -1; a.track_deg = 90; a.speed_kt = 360;       // due east at 360 kt = 0.1852 km per second
  Aircraft copy = a;
  r->update(&copy, 1, 10000);
  r->tick(10000);
  CHECK_NEAR(r->ac[0].lon, -1, 1e-9);
  r->tick(20000);                                     // ten seconds on: 1.852 km east, no change north
  double km_per_deg = 111.32 * cos(51 * 3.14159265358979 / 180);
  CHECK_NEAR((r->ac[0].lon + 1) * km_per_deg, 1.852, 0.01);
  CHECK_NEAR(r->ac[0].lat, 51, 1e-6);
  r->tick(10000 + 600000);                            // reports stopped ten minutes ago: it coasts 30 s and then holds
  CHECK_NEAR((r->ac[0].lon + 1) * km_per_deg, 1.852 * 3, 0.02);

  Aircraft parked = a;                                // on the ground or barely moving: stays put
  parked.on_ground = true;
  r->update(&parked, 1, 700000);
  r->tick(720000);
  CHECK_NEAR(r->ac[0].lon, -1, 1e-9);
}
