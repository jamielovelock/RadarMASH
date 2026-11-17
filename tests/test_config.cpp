// RadarMASH - open firmware for the GeekMagic Radar 360.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../core/config.h"
#include "../core/json.h"
#include "test.h"
#include <stdio.h>
#include <string.h>
#include <string>

using namespace rm;

namespace {
bool load(Config &c, const char *json) { return configFromJson(json, strlen(json), c); }
std::string dump(const Config &c) { char buf[CONFIG_JSON_MAX]; size_t n = configToJson(c, buf, sizeof buf); return std::string(buf, n); }
}  // namespace

TEST(config_defaults_are_safe) {
  Config c;
  CHECK(!c.home_set && !c.live && !c.wantsLive());
  CHECK(strcmp(c.feed_url, FEED_ADSB_FI) == 0 && strcmp(c.map_url, MAP_OSM) == 0);
  CHECK(c.brightness == 100 && c.refresh_s == 5 && c.set.max_aircraft == 10);
}

TEST(config_survives_a_round_trip) {
  Config a;
  a.set.sweep = false; a.set.card = true; a.set.map_style = MapStyle::Green; a.set.map_brightness = 80; a.set.max_aircraft = 25;
  a.home_set = true; a.home_lat = -33.94611; a.home_lon = 151.17722; a.range_km = 80; a.live = true; a.refresh_s = 12;
  a.include_ground = true; a.brightness = 40;
  snprintf(a.feed_url, sizeof a.feed_url, "%s", FEED_ADSB_LOL);
  snprintf(a.map_url, sizeof a.map_url, "https://tiles.example.org/dark/{z}/{x}/{y}.png?key=abc");
  snprintf(a.map_credit, sizeof a.map_credit, "(c) Example \"Maps\"");
  std::string text = dump(a);
  CHECK(!text.empty());
  CHECK(json::isObject(json::parse(text.data(), text.size())));   // it is valid JSON, quotes and all
  Config b;
  CHECK(configFromJson(text.data(), text.size(), b));
  CHECK(dump(b) == text);
  CHECK(b.set.sweep == false && b.set.card == true && b.set.map_style == MapStyle::Green && b.set.max_aircraft == 25);
  CHECK(b.home_set && b.wantsLive());
  CHECK_NEAR(b.home_lat, -33.94611, 1e-5); CHECK_NEAR(b.home_lon, 151.17722, 1e-5);
  CHECK(strcmp(b.map_credit, "(c) Example \"Maps\"") == 0 && strcmp(b.map_url, a.map_url) == 0);
}

TEST(config_changes_only_what_is_sent) {
  Config c;
  c.set.trails = false; c.range_km = 80; c.brightness = 30;
  CHECK(load(c, "{\"sweep\":false}"));
  CHECK(c.set.sweep == false && c.set.trails == false && c.range_km == 80 && c.brightness == 30);
  CHECK(load(c, "{\"brightness\":75,\"an_unknown_setting\":123,\"tag_speed\":true}"));
  CHECK(c.brightness == 75 && c.set.tag_speed && c.set.sweep == false);
  CHECK(load(c, "{}"));
  CHECK(c.brightness == 75);
}

TEST(config_clamps_and_ignores_nonsense) {
  Config c;
  CHECK(load(c, "{\"brightness\":0,\"refresh_s\":0,\"max_aircraft\":9999,\"map_brightness\":-5,\"map_style\":7,\"range_km\":1e9}"));
  CHECK(c.brightness == 1 && c.refresh_s == 2 && c.set.max_aircraft == MAX_AIRCRAFT && c.set.map_brightness == 0);
  CHECK((int)c.set.map_style == (int)MapStyle::Count - 1 && c.range_km == 400);
  Config d;
  CHECK(load(d, "{\"brightness\":\"bright\",\"sweep\":1,\"refresh_s\":null,\"live\":\"yes\",\"range_km\":[40]}"));   // wrong types: left alone
  CHECK(d.brightness == 100 && d.set.sweep == true && d.refresh_s == 5 && !d.live && d.range_km == 40);
  CHECK(!load(d, "") && !load(d, "[1,2]") && !load(d, "not json") && !load(d, "{\"sweep\":fal"));
  CHECK(d.set.sweep == true);
}

TEST(config_home_needs_a_real_pair) {
  Config c;
  CHECK(load(c, "{\"home_lat\":53.5}"));               // half a position is not a position
  CHECK(!c.home_set && c.home_lat == 51.0);
  CHECK(load(c, "{\"home_lat\":95,\"home_lon\":10}"));  // out of range is refused, not clamped to somewhere else
  CHECK(!c.home_set);
  CHECK(load(c, "{\"home_lat\":-45.5,\"home_lon\":170.25,\"live\":true}"));
  CHECK(c.home_set && c.wantsLive() && c.home_lat == -45.5 && c.home_lon == 170.25);
  CHECK(load(c, "{\"home_set\":false}"));               // forget it again
  CHECK(!c.home_set && !c.wantsLive() && c.live);
}

TEST(config_only_accepts_real_web_addresses) {
  Config c;
  const char *bad[] = {"{\"map_url\":\"javascript:alert(1)\"}", "{\"map_url\":\"ftp://x/{z}\"}", "{\"map_url\":\"\"}", "{\"map_url\":\"https://a b/{z}\"}",
                       "{\"feed_url\":\"file:///etc/passwd\"}", "{\"feed_url\":42}"};
  for (const char *t : bad) CHECK(load(c, t));
  CHECK(strcmp(c.map_url, MAP_OSM) == 0 && strcmp(c.feed_url, FEED_ADSB_FI) == 0);
  std::string too_long = "{\"map_url\":\"https://example.org/" + std::string(200, 'a') + "\"}";
  CHECK(load(c, too_long.c_str()));
  CHECK(strcmp(c.map_url, MAP_OSM) == 0);               // too long to store: keep the old one rather than half of the new
  CHECK(load(c, "{\"map_url\":\"http://tiles.local/{z}/{x}/{y}.png\",\"map_credit\":\"A very long credit line that will not fit\"}"));
  CHECK(strcmp(c.map_url, "http://tiles.local/{z}/{x}/{y}.png") == 0);
  CHECK(strlen(c.map_credit) == sizeof c.map_credit - 1);
}

TEST(config_sweep_speed_and_map_detail) {
  Config c;
  CHECK(c.set.sweep_seconds == 4 && c.set.map_detail == 0);
  CHECK(load(c, "{\"sweep_seconds\":12,\"map_detail\":1}"));
  CHECK(c.set.sweep_seconds == 12 && c.set.map_detail == 1);
  Config d;
  std::string text = dump(c);
  CHECK(configFromJson(text.data(), text.size(), d) && d.set.sweep_seconds == 12 && d.set.map_detail == 1);
  CHECK(load(c, "{\"sweep_seconds\":0,\"map_detail\":9}"));
  CHECK(c.set.sweep_seconds == 2 && c.set.map_detail == 1);     // clamped to what the radar can do
}

TEST(config_certificate_checking_is_on_unless_turned_off) {
  Config c;
  CHECK(c.verify_certs);
  CHECK(dump(c).find("\"verify_certs\":true") != std::string::npos);
  CHECK(load(c, "{\"verify_certs\":\"no\"}") && c.verify_certs);     // only a real false turns it off
  CHECK(load(c, "{\"verify_certs\":false}") && !c.verify_certs);
  Config d;
  std::string text = dump(c);
  CHECK(configFromJson(text.data(), text.size(), d) && !d.verify_certs);
}

TEST(config_font_is_stored_by_name) {
  Config c;
  CHECK(dump(c).find("\"font\":\"classic\"") != std::string::npos);
  CHECK(load(c, "{\"font\":\"b612mono\"}"));
  CHECK(c.set.font == fontIndex("b612mono") && c.set.font > 0);
  CHECK(dump(c).find("\"font\":\"b612mono\"") != std::string::npos);
  CHECK(load(c, "{\"font\":\"a-font-from-a-newer-version\"}") && load(c, "{\"font\":3}"));
  CHECK(c.set.font == fontIndex("b612mono"));               // unknown: keep what we have
  c.set.font = 200;                                         // a corrupt index never escapes into the JSON
  CHECK(dump(c).find("\"font\":\"classic\"") != std::string::npos);
}

TEST(config_json_fits_its_buffer) {
  Config c;
  memset(c.feed_url, 'x', sizeof c.feed_url - 1); c.feed_url[sizeof c.feed_url - 1] = 0;
  memset(c.map_url, '"', sizeof c.map_url - 1); c.map_url[sizeof c.map_url - 1] = 0;         // every character needs escaping
  memset(c.map_credit, '\\', sizeof c.map_credit - 1); c.map_credit[sizeof c.map_credit - 1] = 0;
  c.home_lat = -89.99999; c.home_lon = -179.99999; c.range_km = 400; c.refresh_s = 300;
  char buf[CONFIG_JSON_MAX];
  size_t n = configToJson(c, buf, sizeof buf);
  CHECK(n > 0 && n < sizeof buf && buf[n] == 0);
  CHECK(json::isObject(json::parse(buf, n)));
  char tiny[40];
  CHECK(configToJson(c, tiny, sizeof tiny) == 0 && tiny[0] == 0);   // too small: says so, writes nothing misleading
  CHECK(configToJson(c, nullptr, 0) == 0);
}

TEST(wifi_request_is_checked) {
  char ssid[33], pass[65];
  const char *good = "{\"ssid\":\"Home Network\",\"pass\":\"correct horse\"}";
  CHECK(parseWifiRequest(good, strlen(good), ssid, pass));
  CHECK(strcmp(ssid, "Home Network") == 0 && strcmp(pass, "correct horse") == 0);
  const char *open = "{\"ssid\":\"Cafe\"}";
  CHECK(parseWifiRequest(open, strlen(open), ssid, pass) && pass[0] == 0);
  const char *quoted = "{\"ssid\":\"Bob\\\"s \\\\ net\",\"pass\":\"\"}";
  CHECK(parseWifiRequest(quoted, strlen(quoted), ssid, pass) && strcmp(ssid, "Bob\"s \\ net") == 0);
  const char *bad[] = {"", "[]", "{\"pass\":\"12345678\"}", "{\"ssid\":\"\"}", "{\"ssid\":\"x\",\"pass\":\"short\"}",
                       "{\"ssid\":\"this network name is far too long to be real\"}", "{\"ssid\":5}", "{\"ssid\":\"x\",\"pass\":12345678}",
                       "{\"ssid\":\"x\",\"pass\":\"0123456789012345678901234567890123456789012345678901234567890123\"}"};
  for (const char *b : bad) {
    snprintf(ssid, sizeof ssid, "stale"); snprintf(pass, sizeof pass, "stale");
    CHECK(!parseWifiRequest(b, strlen(b), ssid, pass));
    CHECK(ssid[0] == 0 && pass[0] == 0);                 // a refused request leaves nothing behind
  }
}

TEST(network_names_are_escaped_for_json) {
  char out[64];
  CHECK(jsonEscape("plain", out, sizeof out) == 5 && strcmp(out, "plain") == 0);
  jsonEscape("a\"b\\c\nd", out, sizeof out);
  CHECK(strcmp(out, "a\\\"b\\\\cd") == 0);
  char tiny[5];
  jsonEscape("\"\"\"\"\"\"", tiny, sizeof tiny);
  CHECK(strlen(tiny) < sizeof tiny && strlen(tiny) % 2 == 0);    // never cut in the middle of an escape
  CHECK(jsonEscape("x", out, 0) == 0);
}
