// RadarMASH - open firmware for the GeekMagic Radar 360.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "live.h"
#include <chrono>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>

extern char **environ;
using namespace rm;

static void replaceAll(std::string &s, const std::string &from, const std::string &to) {
  for (size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + to.size())) s.replace(p, from.size(), to);
}

bool httpGet(const std::string &url, const std::string &ua, std::string &body, bool verify) {
  body.clear();
  if (url.compare(0, 8, "https://") != 0 && url.compare(0, 7, "http://") != 0) return false;
  char tmp[] = "/tmp/radarmash-XXXXXX";
  int fd = mkstemp(tmp);
  if (fd < 0) return false;
  close(fd);
  const char *argv[] = {"curl", verify ? "-sSL" : "-sSLk", "--compressed", "--max-time", "12", "-A", ua.c_str(), "-o", tmp, "--", url.c_str(), nullptr};
  pid_t pid;
  bool ok = posix_spawnp(&pid, "curl", nullptr, nullptr, (char *const *)argv, environ) == 0;
  if (ok) {
    int status = 0;
    waitpid(pid, &status, 0);
    ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
  }
  if (FILE *f = fopen(tmp, "rb")) {
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0 && body.size() < 4 * 1024 * 1024) body.append(buf, n);
    fclose(f);
  }
  unlink(tmp);
  return ok;
}

void Live::start(Radar *radar, std::mutex *mu, uint32_t (*clock_ms)()) {
  radar_ = radar; mu_ = mu; clock_ = clock_ms;
  std::thread([this] { run(); }).detach();
}

void Live::run() {
  for (;;) {
    bool on;
    { std::lock_guard<std::mutex> lock(*mu_); on = enabled; }
    if (background) background();
    lookupZone();
    if (on) watchWorld();
    if (!on) { std::this_thread::sleep_for(std::chrono::milliseconds(300)); continue; }
    auto began = std::chrono::steady_clock::now();
    poll();
    int wait_s;
    bool want_routes;
    { std::lock_guard<std::mutex> lock(*mu_); wait_s = refresh_s < 2 ? 2 : refresh_s > 300 ? 300 : refresh_s; want_routes = routes; }
    if (want_routes) lookupSome();
    std::this_thread::sleep_until(began + std::chrono::seconds(wait_s));
  }
}

void Live::poll() {
  std::string url, ua, url_before;
  double lat, lon; bool ground, verify; int limit;
  {
    std::lock_guard<std::mutex> lock(*mu_);
    verify = verify_certs;
    url = url_before = feed_url; ua = user_agent; lat = radar_->home_lat; lon = radar_->home_lon; ground = include_ground;
    limit = radar_->set.max_aircraft < 1 ? 1 : radar_->set.max_aircraft > MAX_AIRCRAFT ? MAX_AIRCRAFT : radar_->set.max_aircraft;
    char num[32];
    snprintf(num, sizeof num, "%.4f", lat); replaceAll(url, "{lat}", num);
    snprintf(num, sizeof num, "%.4f", lon); replaceAll(url, "{lon}", num);
    replaceAll(url, "{nm}", std::to_string(feedRadiusNm(radar_->feedRangeKm())));
  }
  std::string body;
  bool got = httpGet(url, ua, body, verify);
  std::vector<Aircraft> fresh(MAX_AIRCRAFT);
  int n = got ? parseAircraft(body.data(), body.size(), lat, lon, ground, fresh.data(), limit) : -1;

  std::lock_guard<std::mutex> lock(*mu_);
  if (n < 0) {
    errors++;
    last_error = got ? "the feed's reply was not understood" : "could not reach the feed";
    // The two built-in feeds serve the same data. If one is down, swap to the other after a couple of misses.
    if (++failing >= 2 && feed_url == url_before) {
      if (feed_url == FEED_ADSB_FI) { feed_url = FEED_ADSB_LOL; failing = 0; last_error += "; trying adsb.lol"; }
      else if (feed_url == FEED_ADSB_LOL) { feed_url = FEED_ADSB_FI; failing = 0; last_error += "; trying adsb.fi"; }
    }
    return;                                      // keep showing what we have; aircraft coast on for a while
  }
  for (int i = 0; i < n; i++) {                  // routes already looked up
    auto hit = routes_.find(fresh[i].callsign);
    if (hit == routes_.end() || !hit->second.known) continue;
    snprintf(fresh[i].airline, sizeof fresh[i].airline, "%s", hit->second.info.airline);
    snprintf(fresh[i].origin, sizeof fresh[i].origin, "%s", hit->second.info.origin);
    snprintf(fresh[i].dest, sizeof fresh[i].dest, "%s", hit->second.info.dest);
  }
  radar_->update(fresh.data(), n, clock_());
  if (after_update) after_update();
  last_count = n; last_ok = (long)time(nullptr); last_error.clear(); failing = 0;
}

// Once a minute, ask whether any aircraft anywhere is squawking 7700.
void Live::watchWorld() {
  std::string ua; bool verify, want; double lat, lon;
  long now = (long)time(nullptr);
  { std::lock_guard<std::mutex> lock(*mu_); want = alerts && watch && now >= watch_test_until; ua = user_agent; verify = verify_certs;
    lat = watch && watch->away ? watch->back_lat : radar_->home_lat; lon = watch && watch->away ? watch->back_lon : radar_->home_lon; }
  if (!want || now - watched_at_ < WATCH_EVERY_S) return;
  watched_at_ = now;
  std::string body;
  std::vector<Aircraft> list(16);
  int n = httpGet(WATCH_ADSB_FI, ua, body, verify) ? parseAircraft(body.data(), body.size(), lat, lon, false, list.data(), 16) : -1;
  if (n < 0) n = httpGet(WATCH_ADSB_LOL, ua, body, verify) ? parseAircraft(body.data(), body.size(), lat, lon, false, list.data(), 16) : -1;
  if (n < 0) return;                             // no answer is not the same as all clear
  // nearest home first, so the one offered is the closest
  for (int i = 1; i < n; i++)
    if (distanceKm(lat, lon, list[i].lat, list[i].lon) < distanceKm(lat, lon, list[0].lat, list[0].lon)) std::swap(list[0], list[i]);
  std::lock_guard<std::mutex> lock(*mu_);
  if ((long)time(nullptr) >= watch_test_until) watch->found(list.data(), n);
}

// The weather where the radar is, and with it the time zone: asked for when the position changes and every quarter of
// an hour after that.
void Live::lookupZone() {
  std::string ua; bool verify, want; double lat, lon;
  long now = (long)time(nullptr);
  { std::lock_guard<std::mutex> lock(*mu_); want = zone_wanted; lat = zone_lat; lon = zone_lon; ua = user_agent; verify = verify_certs;
    if (!want) { zone_known = false; wx = Weather(); } }
  bool moved = lat != zone_for_lat_ || lon != zone_for_lon_;
  if (!want || (!moved && now - zone_at_ < WEATHER_EVERY_S)) return;
  zone_at_ = now; zone_for_lat_ = lat; zone_for_lon_ = lon;
  std::string url = WEATHER_OPEN_METEO, body;
  char num[32];
  snprintf(num, sizeof num, "%.2f", lat); replaceAll(url, "{lat}", num);
  snprintf(num, sizeof num, "%.2f", lon); replaceAll(url, "{lon}", num);
  Weather fresh;
  bool ok = httpGet(url, ua, body, verify) && parseWeather(body.data(), body.size(), fresh);
  std::lock_guard<std::mutex> lock(*mu_);
  if (ok) { wx = fresh; zone_known = true; zone_min = fresh.utc_offset_min; }
  else { if (moved) { zone_known = false; wx = Weather(); } zone_at_ = now - WEATHER_EVERY_S + 300; }   // try again in five minutes
}

bool Live::airport(const std::string &icao, const std::string &ua, bool verify, Airport &out) {
  auto hit = airports_.find(icao);
  if (hit != airports_.end()) { out = hit->second; return out.ok; }
  std::string url = AIRPORT_HEXDB, body;
  replaceAll(url, "{icao}", icao);
  if (!httpGet(url, ua, body, verify)) return false;                    // network trouble: do not remember a failure
  out.ok = parseHexdbAirport(body.data(), body.size(), out.iata, out.lat, out.lon);
  airports_[icao] = out;
  return out.ok;
}

// Look up a few callsigns per cycle, nearest the top of the list first, so a busy sky fills in over a minute or so
// without hammering the route services. The first service's answer is used if it fits where the aircraft is; if it has
// none, or one that does not fit, a second service is asked and its answer held to the same test. A route that fails
// is not shown: no route is better than a wrong one.
void Live::lookupSome() {
  struct Want { std::string cs; double lat, lon; float track; int climb; };
  std::vector<Want> todo;
  std::string tmpl, ua;
  bool verify;
  long now = (long)time(nullptr);
  {
    std::lock_guard<std::mutex> lock(*mu_);
    tmpl = route_url; ua = user_agent; verify = verify_certs;
    for (int i = 0; i < radar_->count && todo.size() < 3; i++) {
      const Aircraft &a = radar_->ac[i];
      float x, y;
      if (!lookupableCallsign(a.callsign) || !radar_->project(a, x, y)) continue;
      auto hit = routes_.find(a.callsign);
      if (hit != routes_.end() && (hit->second.known || now - hit->second.when < 1800)) continue;   // retry unknowns after 30 min
      todo.push_back(Want{a.callsign, a.lat, a.lon, a.track_deg, a.climb_fpm});
    }
  }
  for (const Want &w : todo) {
    std::string url = tmpl, body;
    replaceAll(url, "{callsign}", w.cs);
    httpGet(url, ua, body, verify);
    RouteInfo info;
    RouteResult res = parseRoute(body.data(), body.size(), info);
    if (res == RouteResult::Bad) { std::lock_guard<std::mutex> lock(*mu_); lookups++; continue; }   // network trouble: ask again next cycle

    bool rejected = false, asked_second = false, retry = false;
    if (res != RouteResult::Found || routeFit(info, w.lat, w.lon, w.track, w.climb) == RouteFit::DoesNotFit) {
      rejected = res == RouteResult::Found;
      asked_second = true;
      info.origin[0] = info.dest[0] = 0; info.have_pos = false;         // keep the airline name, drop the route
      std::string url2 = ROUTE_HEXDB, body2;
      replaceAll(url2, "{callsign}", w.cs);
      char o[5], d[5];
      if (!httpGet(url2, ua, body2, verify)) retry = true;
      else if (parseHexdbRoute(body2.data(), body2.size(), o, d)) {
        Airport ao, ad;
        RouteInfo second = info;
        if (airport(o, ua, verify, ao) && airport(d, ua, verify, ad)) {
          snprintf(second.origin, sizeof second.origin, "%s", ao.iata); snprintf(second.dest, sizeof second.dest, "%s", ad.iata);
          second.o_lat = ao.lat; second.o_lon = ao.lon; second.d_lat = ad.lat; second.d_lon = ad.lon; second.have_pos = true;
          if (routeFit(second, w.lat, w.lon, w.track, w.climb) == RouteFit::Fits) info = second;
        }
      }
    }
    std::lock_guard<std::mutex> lock(*mu_);
    lookups++; second_opinions += asked_second; routes_rejected += rejected;
    if (retry) continue;
    bool anything = info.airline[0] || info.origin[0];
    routes_[w.cs] = Cached{info, anything, now};
    if (!anything) continue;
    for (int i = 0; i < radar_->count; i++) {    // show it straight away
      Aircraft &a = radar_->ac[i];
      if (w.cs != a.callsign) continue;
      snprintf(a.airline, sizeof a.airline, "%s", info.airline);
      snprintf(a.origin, sizeof a.origin, "%s", info.origin);
      snprintf(a.dest, sizeof a.dest, "%s", info.dest);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
  }
}
