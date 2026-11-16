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

bool httpGet(const std::string &url, const std::string &ua, std::string &body) {
  body.clear();
  if (url.compare(0, 8, "https://") != 0 && url.compare(0, 7, "http://") != 0) return false;
  char tmp[] = "/tmp/radarmash-XXXXXX";
  int fd = mkstemp(tmp);
  if (fd < 0) return false;
  close(fd);
  const char *argv[] = {"curl", "-sSL", "--compressed", "--max-time", "12", "-A", ua.c_str(), "-o", tmp, "--", url.c_str(), nullptr};
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
  double lat, lon; bool ground;
  {
    std::lock_guard<std::mutex> lock(*mu_);
    url = url_before = feed_url; ua = user_agent; lat = radar_->home_lat; lon = radar_->home_lon; ground = include_ground;
    char num[32];
    snprintf(num, sizeof num, "%.4f", lat); replaceAll(url, "{lat}", num);
    snprintf(num, sizeof num, "%.4f", lon); replaceAll(url, "{lon}", num);
    replaceAll(url, "{nm}", std::to_string(feedRadiusNm(radar_->range_km)));
  }
  std::string body;
  bool got = httpGet(url, ua, body);
  std::vector<Aircraft> fresh(MAX_AIRCRAFT);
  int n = got ? parseAircraft(body.data(), body.size(), lat, lon, ground, fresh.data(), MAX_AIRCRAFT) : -1;

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
  last_count = n; last_ok = (long)time(nullptr); last_error.clear(); failing = 0;
}

// Look up a few callsigns per cycle, nearest the top of the list first, so a busy sky fills in over a minute or so
// without hammering the route service.
void Live::lookupSome() {
  std::vector<std::string> todo;
  std::string tmpl, ua;
  long now = (long)time(nullptr);
  {
    std::lock_guard<std::mutex> lock(*mu_);
    tmpl = route_url; ua = user_agent;
    for (int i = 0; i < radar_->count && todo.size() < 3; i++) {
      const char *cs = radar_->ac[i].callsign;
      float x, y;
      if (!lookupableCallsign(cs) || !radar_->project(radar_->ac[i], x, y)) continue;
      auto hit = routes_.find(cs);
      if (hit != routes_.end() && (hit->second.known || now - hit->second.when < 1800)) continue;   // retry unknowns after 30 min
      todo.push_back(cs);
    }
  }
  for (const std::string &cs : todo) {
    std::string url = tmpl, body;
    replaceAll(url, "{callsign}", cs);
    httpGet(url, ua, body);
    RouteInfo info;
    RouteResult res = parseRoute(body.data(), body.size(), info);
    std::lock_guard<std::mutex> lock(*mu_);
    lookups++;
    if (res == RouteResult::Bad) continue;       // network trouble: ask again next cycle
    routes_[cs] = Cached{info, res == RouteResult::Found, now};
    if (res != RouteResult::Found) continue;
    for (int i = 0; i < radar_->count; i++) {    // show it straight away
      Aircraft &a = radar_->ac[i];
      if (cs != a.callsign) continue;
      snprintf(a.airline, sizeof a.airline, "%s", info.airline);
      snprintf(a.origin, sizeof a.origin, "%s", info.origin);
      snprintf(a.dest, sizeof a.dest, "%s", info.dest);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
  }
}
