// RadarMASH - open firmware for the GeekMagic Radar 360.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Simulator live data: a background thread that polls an aircraft feed and looks up routes, using curl.
#pragma once
#include "../core/alert.h"
#include "../core/feed.h"
#include <map>
#include <mutex>
#include <string>

struct Live {
  // settings (guard with `mu` once the thread is running)
  std::string feed_url = rm::FEED_ADSB_FI;
  std::string route_url = rm::ROUTE_ADSBDB;
  std::string user_agent;
  int refresh_s = 5;
  bool include_ground = false;
  bool routes = true;
  bool verify_certs = true;     // off: pass -k to curl
  bool enabled = false;         // poll only while this is set (live mode)
  bool alerts = false;          // keep the worldwide emergency watch (only while enabled)
  rm::Watch *watch = nullptr;   // where its findings go
  long watch_test_until = 0;    // a pretend emergency is being shown until this time: leave it alone
  bool zone_wanted = false;     // look up the time zone for zone_lat, zone_lon
  double zone_lat = 0, zone_lon = 0;
  bool zone_known = false;      // result: zone_min is minutes ahead of UTC there, now
  int zone_min = 0;
  void (*background)() = nullptr;   // extra work for this thread each cycle (the simulator fetches logos here)

  // status, for the page
  int last_count = -1;          // aircraft in the last good reply, or -1 before the first
  long last_ok = 0;             // unix time of the last good reply
  int errors = 0, lookups = 0, second_opinions = 0, routes_rejected = 0;
  int failing = 0;              // failed replies in a row
  std::string last_error;

  // Start polling. `radar` is updated under `mu`; `clock_ms` supplies the same clock the radar is ticked with.
  void start(rm::Radar *radar, std::mutex *mu, uint32_t (*clock_ms)());

private:
  struct Cached { rm::RouteInfo info; bool known; long when; };
  std::map<std::string, Cached> routes_;
  struct Airport { char iata[5]; double lat, lon; bool ok; };
  std::map<std::string, Airport> airports_;             // looked up for the second opinion, and kept
  bool airport(const std::string &icao, const std::string &ua, bool verify, Airport &out);
  rm::Radar *radar_ = nullptr;
  std::mutex *mu_ = nullptr;
  uint32_t (*clock_)() = nullptr;
  void run();
  void poll();
  void lookupSome();
  void watchWorld();
  void lookupZone();
  long watched_at_ = 0, zone_at_ = 0;
  double zone_for_lat_ = 1e9, zone_for_lon_ = 1e9;
};

// Fetch a URL with curl (no shell). Returns false if curl failed; `body` holds the reply either way.
bool httpGet(const std::string &url, const std::string &user_agent, std::string &body, bool verify_certs = true);
