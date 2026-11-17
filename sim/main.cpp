// RadarMASH - open firmware for the GeekMagic Radar 360.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Desktop simulator. Builds the same drawing code as the device and shows it in a web browser, with the same settings
// page the device serves. No dependencies beyond a C++ compiler (and curl, for the map and live aircraft):
//   radarmash-sim                       serve the screen and settings at http://localhost:8360
//   radarmash-sim --live --home LAT,LON start with real aircraft around that position
//   radarmash-sim --shot out.bmp [--t ms] [--select n] [--range km] [--map] [--style 0-2]   write one frame and exit
// Settings are kept in ~/.config/radarmash/simulator.json, map tiles in ~/.cache/radarmash. Still frames ignore the
// saved settings and stay offline unless --map, so they always come out the same.
#include "../core/config.h"
#include "../core/radar.h"
#include "admin_html.h"
#include "live.h"
#include "tiles.h"
#include <arpa/inet.h>
#include <chrono>
#include <ctype.h>
#include <mutex>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <vector>

using namespace rm;

static uint16_t frame[SCREEN_W * SCREEN_H];
static Radar radar;
static std::mutex radar_mu;                // the live-data thread and the web requests both touch the radar
static Config cfg;                         // what the settings page shows and what is saved
static Live live;
static ScopeCache scope_cache;             // the same speed-up the device uses, so the simulator shows the same picture
static bool save_settings = false;         // only when serving; still frames never touch the saved file

static uint32_t nowMs() {
  static const auto t0 = std::chrono::steady_clock::now();
  return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
}

// ---- map underlay ----
static SimTiles tiles;
static MapImage map_image;
static bool map_allowed = false;           // may the simulator go to the network for tiles?
static std::string map_key;                // what the current picture was built for

static void ensureMap() {
  if (!map_allowed || !radar.set.map) { radar.map = nullptr; return; }
  char k[160];
  snprintf(k, sizeof k, "%.5f,%.5f,%.1f,%d,", radar.home_lat, radar.home_lon, radar.range_km, (int)radar.set.map_style);
  std::string key = k + tiles.url_template;
  if (key != map_key) {
    composeMap(map_image, tiles, radar.home_lat, radar.home_lon, radar.range_km, radar.set.map_style);
    if (map_image.missing == 0) map_key = key;          // otherwise try again on the next frame (failed tiles wait a minute)
  }
  radar.map = &map_image;
  snprintf(radar.map_credit, sizeof radar.map_credit, "%s", cfg.map_credit);
}

// ---- settings ----
static std::string settingsPath() {
  const char *home = getenv("HOME");
  return std::string(home ? home : ".") + "/.config/radarmash/simulator.json";
}

// Push the configuration into the running radar, feed and map.
static void applyConfig() {
  bool was_live = live.enabled;
  bool moved = radar.home_lat != cfg.home_lat || radar.home_lon != cfg.home_lon;
  radar.set = cfg.set;
  radar.home_lat = cfg.home_lat; radar.home_lon = cfg.home_lon;
  radar.range_km = cfg.range_km;
  live.feed_url = cfg.feed_url; live.refresh_s = cfg.refresh_s; live.include_ground = cfg.include_ground;
  live.enabled = cfg.wantsLive();
  tiles.url_template = cfg.map_url;
  if (was_live != live.enabled || moved) { radar.count = 0; radar.selected = -1; live.last_count = -1; live.last_ok = 0; }   // a different sky
}

static void saveConfig() {
  if (!save_settings) return;
  char buf[CONFIG_JSON_MAX];
  size_t n = configToJson(cfg, buf, sizeof buf);
  if (!n) return;
  std::string path = settingsPath(), dir = path.substr(0, path.rfind('/'));
  mkdir(dir.substr(0, dir.rfind('/')).c_str(), 0700); mkdir(dir.c_str(), 0700);
  std::string tmp = path + ".tmp";
  if (FILE *f = fopen(tmp.c_str(), "w")) { fwrite(buf, 1, n, f); fclose(f); rename(tmp.c_str(), path.c_str()); }
}

static void loadConfig() {
  FILE *f = fopen(settingsPath().c_str(), "r");
  if (!f) return;
  char buf[CONFIG_JSON_MAX * 2];
  size_t n = fread(buf, 1, sizeof buf, f);
  fclose(f);
  configFromJson(buf, n, cfg);
}

static void render(uint32_t t) {
  if (!live.enabled) demoTraffic(radar, t);
  ensureMap();
  radar.tick(t);
  radar.cache = &scope_cache;
  radar.prepare();
  Canvas c(frame, 0, SCREEN_H);          // one band covering the whole screen
  radar.draw(c);
}

// 24-bit top-down BMP of the frame. Pixels outside the round glass are left as drawn; the web page masks them.
static std::vector<uint8_t> bmp() {
  const uint32_t data = SCREEN_W * SCREEN_H * 3, size = 54 + data;
  std::vector<uint8_t> b(size, 0);
  auto put32 = [&](int o, uint32_t v) { b[o] = v; b[o + 1] = v >> 8; b[o + 2] = v >> 16; b[o + 3] = v >> 24; };
  b[0] = 'B'; b[1] = 'M'; put32(2, size); put32(10, 54); put32(14, 40);
  put32(18, SCREEN_W); put32(22, (uint32_t)-SCREEN_H); b[26] = 1; b[28] = 24; put32(34, data);
  uint8_t *p = b.data() + 54;
  for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
    uint16_t c = frame[i];
    int r = (c >> 11) & 31, g = (c >> 5) & 63, bl = c & 31;
    *p++ = (bl << 3) | (bl >> 2); *p++ = (g << 2) | (g >> 4); *p++ = (r << 3) | (r >> 2);
  }
  return b;
}

static std::string jsonEscape(const std::string &in) {
  std::string out;
  for (char ch : in) { if (ch == '"' || ch == '\\') out += '\\'; if ((unsigned char)ch >= 32) out += ch; }
  return out;
}

// The simulator's own page: the round screen, with the shared settings page underneath.
static const char PAGE[] = R"HTML(<!doctype html><meta charset=utf-8><title>RadarMASH simulator</title>
<style>
body{background:#16181c;color:#bbb;font:14px system-ui;text-align:center;margin:0;padding:24px 0 0}
.case{display:inline-block;padding:14px;border-radius:50%;background:#050505;box-shadow:0 0 0 3px #2a2a2a}
img{display:block;width:540px;height:540px;border-radius:50%;image-rendering:pixelated;background:#000}
button{font:inherit;margin:14px 6px 6px;padding:6px 14px} iframe{display:block;width:100%;height:1500px;border:0;margin-top:10px}
</style>
<div class=case><img id=s width=360 height=360 alt="radar screen"></div>
<div><button id=px>Actual size</button></div>
<iframe src="/admin" title="Settings"></iframe>
<script>
const s=document.getElementById('s');let big=true;
document.getElementById('px').onclick=()=>{big=!big;s.style.width=s.style.height=(big?540:360)+'px'};
async function loop(){for(;;){try{const b=await(await fetch('/frame')).blob();const u=URL.createObjectURL(b);
await new Promise(r=>{s.onload=s.onerror=r;s.src=u});URL.revokeObjectURL(u)}catch(e){await new Promise(r=>setTimeout(r,500))}
await new Promise(r=>setTimeout(r,40))}}loop();
</script>)HTML";

static void sendAll(int fd, const void *d, size_t n) {
  const char *p = (const char *)d;
  while (n) { ssize_t w = send(fd, p, n, 0); if (w <= 0) return; p += w; n -= (size_t)w; }
}
static void respond(int fd, const char *type, const void *body, size_t n, int code = 200) {
  char h[200];
  int hl = snprintf(h, sizeof h, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
                    code, code == 200 ? "OK" : "Error", type, n);
  sendAll(fd, h, (size_t)hl); sendAll(fd, body, n);
}
static void respondText(int fd, const std::string &s, const char *type = "text/plain", int code = 200) { respond(fd, type, s.data(), s.size(), code); }

static std::string settingsJson() {
  cfg.set = radar.set; cfg.range_km = radar.range_km;       // the button can change the range behind the page's back
  char buf[CONFIG_JSON_MAX];
  return std::string(buf, configToJson(cfg, buf, sizeof buf));
}

static std::string statusJson() {
  char j[600];
  long age = live.last_ok ? (long)time(nullptr) - live.last_ok : -1;
  snprintf(j, sizeof j, "{\"device\":false,\"version\":\"simulator\",\"live\":%s,\"aircraft\":%d,\"feed_age_s\":%ld,\"feed_error\":\"%s\",\"map_missing\":%d,\"range_km\":%d}",
           live.enabled ? "true" : "false", radar.count, age, jsonEscape(live.last_error).c_str(), radar.map ? map_image.missing : 0, (int)radar.range_km);
  return j;
}

// Read one request: the header, then as much body as Content-Length says (settings are small).
static bool readRequest(int fd, std::string &method, std::string &path, std::string &body) {
  std::string req;
  char buf[4096];
  size_t header_end = std::string::npos, want = 0;
  while (req.size() < 64 * 1024) {
    ssize_t n = recv(fd, buf, sizeof buf, 0);
    if (n <= 0) break;
    req.append(buf, (size_t)n);
    if (header_end == std::string::npos) {
      header_end = req.find("\r\n\r\n");
      if (header_end == std::string::npos) continue;
      std::string head = req.substr(0, header_end);
      for (char &ch : head) ch = (char)tolower((unsigned char)ch);
      size_t cl = head.find("content-length:");
      want = cl == std::string::npos ? 0 : (size_t)strtoul(head.c_str() + cl + 15, nullptr, 10);
    }
    if (req.size() >= header_end + 4 + want) break;
  }
  if (header_end == std::string::npos) return false;
  size_t sp1 = req.find(' '), sp2 = req.find(' ', sp1 + 1);
  if (sp1 == std::string::npos || sp2 == std::string::npos) return false;
  method = req.substr(0, sp1); path = req.substr(sp1 + 1, sp2 - sp1 - 1);
  body = req.substr(header_end + 4, want);
  return true;
}

static int serve(int port) {
  signal(SIGPIPE, SIG_IGN);
  int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(s, (sockaddr *)&a, sizeof a) || listen(s, 16)) { perror("bind/listen"); return 1; }
  printf("RadarMASH simulator: http://localhost:%d\n", port); fflush(stdout);
  for (;;) {
    int fd = accept(s, nullptr, nullptr);
    if (fd < 0) continue;
    std::string method, path, body;
    if (readRequest(fd, method, path, body)) {
      std::lock_guard<std::mutex> lock(radar_mu);
      bool post = method == "POST";
      if (path == "/frame") { render(nowMs()); auto b = bmp(); respond(fd, "image/bmp", b.data(), b.size()); }
      else if (path == "/admin") respond(fd, "text/html", ADMIN_HTML, sizeof ADMIN_HTML - 1);
      else if (path == "/api/settings" && !post) respondText(fd, settingsJson(), "application/json");
      else if (path == "/api/settings") {
        cfg.set = radar.set; cfg.range_km = radar.range_km;
        if (!configFromJson(body.data(), body.size(), cfg)) respondText(fd, "{\"error\":\"not JSON\"}", "application/json", 400);
        else { applyConfig(); saveConfig(); respondText(fd, settingsJson(), "application/json"); }
      }
      else if (path == "/api/status") respondText(fd, statusJson(), "application/json");
      else if (path == "/api/fonts") { char b[600]; respond(fd, "application/json", b, fontsJson(b, sizeof b)); }
      else if (path == "/api/press" && post) { radar.shortPress(); respondText(fd, "ok"); }
      else if (path == "/api/hold" && post) { radar.longPress(); cfg.range_km = radar.range_km; saveConfig(); respondText(fd, "ok"); }
      else if (path == "/api/reboot" && post) respondText(fd, "ok");            // nothing to restart here
      else if (path == "/") respond(fd, "text/html", PAGE, sizeof PAGE - 1);
      else respondText(fd, "not found", "text/plain", 404);
    }
    close(fd);
  }
}

int main(int argc, char **argv) {
  const char *shot = nullptr; uint32_t t = 2600; int select = -1, port = 8360, style = -1; float range = 0;
  bool want_map = false, no_map = false, want_live = false, have_home = false;
  const char *font = nullptr;
  double home_lat = 0, home_lon = 0;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--shot" && i + 1 < argc) shot = argv[++i];
    else if (a == "--t" && i + 1 < argc) t = (uint32_t)strtoul(argv[++i], nullptr, 10);
    else if (a == "--select" && i + 1 < argc) select = atoi(argv[++i]);
    else if (a == "--range" && i + 1 < argc) range = (float)atof(argv[++i]);
    else if (a == "--port" && i + 1 < argc) port = atoi(argv[++i]);
    else if (a == "--map") want_map = true;
    else if (a == "--no-map") no_map = true;
    else if (a == "--style" && i + 1 < argc) style = atoi(argv[++i]);
    else if (a == "--font" && i + 1 < argc) font = argv[++i];
    else if (a == "--live") want_live = true;
    else if (a == "--home" && i + 1 < argc) have_home = sscanf(argv[++i], "%lf,%lf", &home_lat, &home_lon) == 2;
    else {
      fprintf(stderr, "usage: %s [--port n] [--live --home lat,lon] [--no-map]\n       %s --shot out.bmp [--t ms] [--select n] [--range km] [--map] [--style 0-2] [--font id]\n", argv[0], argv[0]);
      return 2;
    }
  }
  if (want_live && shot) { fprintf(stderr, "--live cannot be used with --shot\n"); return 2; }
  const char *ua = getenv("RADARMASH_UA");
  tiles.user_agent = live.user_agent = ua && *ua ? ua : "RadarMASH-simulator/0.1";

  if (!shot) { save_settings = true; loadConfig(); }      // a still frame always starts from the defaults
  if (have_home) { cfg.home_lat = home_lat; cfg.home_lon = home_lon; cfg.home_set = true; }
  if (want_live) cfg.live = true;
  if (cfg.live && !cfg.home_set && want_live) { fprintf(stderr, "--live needs --home lat,lon (or a position saved from the settings page)\n"); return 2; }
  if (range > 0) cfg.range_km = range;
  if (style >= 0) cfg.set.map_style = (MapStyle)(style % (int)MapStyle::Count);
  if (no_map) cfg.set.map = false;
  if (font) {
    int f = fontIndex(font);
    if (f < 0) { fprintf(stderr, "no font called %s\n", font); return 2; }
    cfg.set.font = (uint8_t)f;
  }
  map_allowed = shot ? want_map : true;
  applyConfig();

  if (!shot) {
    live.start(&radar, &radar_mu, nowMs);
    return serve(port);
  }
  radar.set.auto_cycle = false;          // a still frame shows exactly the aircraft asked for
  demoTraffic(radar, t);
  radar.selected = select;
  render(t);
  auto b = bmp();
  FILE *f = fopen(shot, "wb");
  if (!f) { perror(shot); return 1; }
  fwrite(b.data(), 1, b.size(), f); fclose(f);
  return 0;
}
