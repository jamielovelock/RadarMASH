// RadarMASH - open firmware for the GeekMagic Radar 360.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Simulator live data: a background thread that polls an aircraft feed and looks up routes, using curl.
#pragma once
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
  bool enabled = false;         // poll only while this is set (live mode)

  // status, for the page
  int last_count = -1;          // aircraft in the last good reply, or -1 before the first
  long last_ok = 0;             // unix time of the last good reply
  int errors = 0, lookups = 0;
  int failing = 0;              // failed replies in a row
  std::string last_error;

  // Start polling. `radar` is updated under `mu`; `clock_ms` supplies the same clock the radar is ticked with.
  void start(rm::Radar *radar, std::mutex *mu, uint32_t (*clock_ms)());

private:
  struct Cached { rm::RouteInfo info; bool known; long when; };
  std::map<std::string, Cached> routes_;
  rm::Radar *radar_ = nullptr;
  std::mutex *mu_ = nullptr;
  uint32_t (*clock_)() = nullptr;
  void run();
  void poll();
  void lookupSome();
};

// Fetch a URL with curl (no shell). Returns false if curl failed; `body` holds the reply either way.
bool httpGet(const std::string &url, const std::string &user_agent, std::string &body);
