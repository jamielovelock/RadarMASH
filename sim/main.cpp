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
#include "../core/alert.h"
#include "../core/config.h"
#include "../core/control.h"
#include "../core/pages.h"
#include "../core/radar.h"
#include "admin_html.h"
#include "wifi_html.h"
#include "live.h"
#include "logos.h"
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
static Watch watch;                        // the worldwide emergency watch
static PageData pages;                     // what the weather, board and log pages draw from
static Visitors visitors;                  // notices for unusual aircraft
static int shot_clock = -1;                // still frames: the time to show, or none
static SimLogos logos;                     // airline logos, if the settings name a source
static ScopeCache scope_cache;             // the same speed-up the device uses, so the simulator shows the same picture
static std::string fake_wifi_ssid;         // stand-ins for the device's Wi-Fi setup, so that page can be developed here
static bool fake_wifi_ok = false;
static time_t fake_wifi_at = 0;
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
  snprintf(k, sizeof k, "%.5f,%.5f,%.1f,%d,%d,", radar.home_lat, radar.home_lon, radar.range_km, (int)radar.set.map_style, (int)radar.set.map_detail);
  std::string key = k + tiles.url_template;
  if (key != map_key) {
    composeMap(map_image, tiles, radar.home_lat, radar.home_lon, radar.range_km, radar.set.map_style, radar.set.map_detail);
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
  watch.comeHome(radar);                   // settings are about home: if the scope is away looking at an emergency, come back
  bool was_live = live.enabled;
  bool moved = radar.home_lat != cfg.home_lat || radar.home_lon != cfg.home_lon;
  radar.set = cfg.set;
  radar.home_lat = cfg.home_lat; radar.home_lon = cfg.home_lon;
  radar.range_km = cfg.range_km;
  live.feed_url = cfg.feed_url; live.refresh_s = cfg.refresh_s; live.include_ground = cfg.include_ground;
  live.enabled = cfg.wantsLive();
  live.alerts = cfg.set.alerts; live.watch = &watch;
  // The weather service is only asked when something needs it: the clock's zone, the weather page or dimming by the sun.
  live.zone_wanted = cfg.home_set && (cfg.utc_auto || cfg.set.page_weather || cfg.night == 1); live.zone_lat = cfg.home_lat; live.zone_lon = cfg.home_lon;
  tiles.url_template = cfg.map_url;
  tiles.verify_certs = live.verify_certs = logos.verify_certs = cfg.verify_certs;
  logos.url_template = cfg.logo_url;
  pages.units = cfg.units; pages.overhead_km = cfg.overhead_km;
  snprintf(pages.airport, sizeof pages.airport, "%s", cfg.board_airport);
  radar.pages = &pages;
  pages.wx_wanted = live.zone_wanted;
  radar.logos = &logos;
  if (was_live != live.enabled || moved) { radar.count = 0; radar.selected = -1; live.last_count = -1; live.last_ok = 0; }   // a different sky
}

static void saveConfig() {
  if (!save_settings) return;
  char buf[CONFIG_JSON_MAX];
  size_t n = configToJson(cfg, buf, sizeof buf, true);
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

// What happens each time the list of aircraft is refreshed: the log of what has been seen, and the visitor notice.
static void afterUpdate() {
  if (watch.away) return;
  pages.seen.note(radar, (long long)time(nullptr), nowMs());
  if (radar.set.visitors) visitors.check(radar, cfg.visitor_types, radar.set.visitor_heli, nowMs());
}

static void render(uint32_t t) {
  if (!live.enabled) {
    demoTraffic(radar, t);
    static uint32_t noted = 0;
    if (save_settings && t - noted > 2000) { noted = t; afterUpdate(); }
  }
  watch.tick(radar, t, radar.set.alerts && (live.enabled || !save_settings));
  visitors.tick(radar, t, radar.set.visitors);
  if (save_settings) {
    pages.wx = live.wx;
    pages.utc_offset_min = cfg.utc_auto ? (live.zone_known ? live.zone_min : 0) : cfg.utc_offset_min;
    pageTick(radar, watch, cfg.page_cycle_s, t);
  }
  if (save_settings) {                     // serving: the real time. Automatic zone not known yet: UTC, marked Z
    bool zoned = !cfg.utc_auto || live.zone_known;
    radar.clock_min = (int16_t)clockMinutes((long long)time(nullptr), cfg.utc_auto ? (live.zone_known ? live.zone_min : 0) : cfg.utc_offset_min);
    radar.clock_utc = !zoned;
  } else { radar.clock_min = (int16_t)shot_clock; radar.clock_utc = false; }
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
  cfg.set = radar.set;
  if (!watch.away) cfg.range_km = radar.range_km;       // the button can change the range behind the page's back
  char buf[CONFIG_JSON_MAX];
  return std::string(buf, configToJson(cfg, buf, sizeof buf));          // the form for a web page: no control key in it
}

static std::string statusJson() {
  char j[800];
  long age = live.last_ok ? (long)time(nullptr) - live.last_ok : -1;
  snprintf(j, sizeof j, "{\"device\":false,\"version\":\"simulator\",\"live\":%s,\"aircraft\":%d,\"feed_age_s\":%ld,\"feed_error\":\"%s\",\"map_missing\":%d,\"range_km\":%d,"
           "\"lookups\":%d,\"second_opinions\":%d,\"routes_rejected\":%d,\"logos\":%d,\"logos_missing\":%d,\"emergency\":\"%s\",\"away\":%s,\"zone_min\":%d,\"page\":\"%s\",\"night\":%s,\"weather\":%s,\"key_set\":%s}",
           live.enabled ? "true" : "false", radar.count, age, jsonEscape(live.last_error).c_str(), radar.map ? map_image.missing : 0, (int)radar.range_km,
           live.lookups, live.second_opinions, live.routes_rejected, logos.fetched, logos.missing,
           watch.have ? jsonEscape(watch.target.callsign).c_str() : "", watch.away ? "true" : "false", live.zone_known ? live.zone_min : -9999,
           pageName((Page)radar.page), isNight(cfg.night, radar.clock_min, pages.wx, cfg.night_start_min, cfg.night_end_min) ? "true" : "false",
           pages.wx.have ? "true" : "false", cfg.api_key[0] ? "true" : "false");
  return j;
}

// Read one request: the header, then as much body as Content-Length says (settings are small).
static bool readRequest(int fd, std::string &method, std::string &path, std::string &body, std::string &key) {
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
  {                                        // the X-Key header, whatever its capitals
    std::string head = req.substr(0, header_end), low = head;
    for (char &ch : low) ch = (char)tolower((unsigned char)ch);
    size_t k = low.find("\r\nx-key:");
    if (k != std::string::npos) {
      size_t a = k + 8, b = head.find("\r\n", a);
      key = head.substr(a, b == std::string::npos ? std::string::npos : b - a);
      while (!key.empty() && key[0] == ' ') key.erase(0, 1);
    }
  }
  body = req.substr(header_end + 4, want);
  return true;
}

static std::string urlDecode(const std::string &in) {
  std::string out;
  for (size_t i = 0; i < in.size(); i++) {
    if (in[i] == '+') out += ' ';
    else if (in[i] == '%' && i + 2 < in.size() + 0 && isxdigit((unsigned char)in[i + 1]) && isxdigit((unsigned char)in[i + 2])) {
      out += (char)strtol(in.substr(i + 1, 2).c_str(), nullptr, 16); i += 2;
    } else out += in[i];
  }
  return out;
}
static std::vector<std::pair<std::string, std::string>> queryPairs(const std::string &query) {
  std::vector<std::pair<std::string, std::string>> out;
  for (size_t a = 0; a < query.size();) {
    size_t b = query.find('&', a);
    if (b == std::string::npos) b = query.size();
    std::string part = query.substr(a, b - a);
    size_t eq = part.find('=');
    if (!part.empty()) out.push_back({urlDecode(part.substr(0, eq)), eq == std::string::npos ? "" : urlDecode(part.substr(eq + 1))});
    a = b + 1;
  }
  return out;
}

static void startAlertTest() {
  Aircraft a;
  snprintf(a.hex, sizeof a.hex, "test01"); snprintf(a.callsign, sizeof a.callsign, "TEST7700"); snprintf(a.squawk, sizeof a.squawk, "7700");
  a.lat = 40.64; a.lon = -73.78; a.alt_ft = 12000; a.speed_kt = 250; a.track_deg = 40; a.shape = Shape::Narrow;
  watch.comeHome(radar, "test"); watch = Watch(); watch.found(&a, 1); live.watch_test_until = (long)time(nullptr) + 180;
}

// One-line control: /api/do?page=weather&brightness=40. Every pair is tried; the first that fails stops it.
static std::string doRequest(const std::vector<std::pair<std::string, std::string>> &pairs, int &code) {
  bool settings = false;
  code = 200;
  for (const auto &kv : pairs) {
    if (kv.first == "key") continue;
    if (kv.first == "alert" && kv.second == "test") { startAlertTest(); continue; }
    char json[96];
    Cmd c = command(kv.first.c_str(), kv.second.c_str(), radar, watch, nowMs(), json, sizeof json);
    if (c == Cmd::Settings) {
      cfg.set = radar.set;
      if (!watch.away) cfg.range_km = radar.range_km;
      configFromJson(json, strlen(json), cfg);
      applyConfig();
      settings = true;
    } else if (c != Cmd::Done) {
      code = 400;
      return std::string("{\"error\":\"") + (c == Cmd::Unknown ? "no such control: " : "bad value for ") + jsonEscape(kv.first) + "\"}";
    }
  }
  if (!watch.away) cfg.range_km = radar.range_km;
  (void)settings;
  saveConfig();
  char j[160];
  snprintf(j, sizeof j, "{\"ok\":true,\"page\":\"%s\",\"range_km\":%d,\"away\":%s}", pageName((Page)radar.page), (int)radar.range_km, watch.away ? "true" : "false");
  return j;
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
    std::string method, path, body, key;
    if (readRequest(fd, method, path, body, key)) {
      std::lock_guard<std::mutex> lock(radar_mu);
      bool post = method == "POST";
      std::string query;
      if (size_t q = path.find('?'); q != std::string::npos) { query = path.substr(q + 1); path.erase(q); }
      auto pairs = queryPairs(query);
      for (const auto &kv : pairs) if (kv.first == "key") key = kv.second;
      // With a control key set, only looking is free: the screen, the pages themselves and the status.
      const bool open_to_all = !post && method != "PUT" && (path == "/" || path == "/frame" || path == "/admin" || path == "/wifi" || path == "/api/status" || path == "/api/fonts");
      if (!open_to_all && !keyOk(cfg.api_key, key.c_str())) respondText(fd, "{\"error\":\"key needed\"}", "application/json", 401);
      else if (path == "/api/do") { int code; std::string out = doRequest(pairs, code); respondText(fd, out, "application/json", code); }
      else if (path == "/frame") { render(nowMs()); auto b = bmp(); respond(fd, "image/bmp", b.data(), b.size()); }
      else if (path == "/admin") respond(fd, "text/html", ADMIN_HTML, sizeof ADMIN_HTML - 1);
      else if (path == "/api/settings" && !post) respondText(fd, settingsJson(), "application/json");
      else if (path == "/api/settings") {
        cfg.set = radar.set;
        if (!watch.away) cfg.range_km = radar.range_km;
        if (!configFromJson(body.data(), body.size(), cfg)) respondText(fd, "{\"error\":\"not JSON\"}", "application/json", 400);
        else { applyConfig(); saveConfig(); respondText(fd, settingsJson(), "application/json"); }
      }
      else if (path == "/api/status") respondText(fd, statusJson(), "application/json");
      else if (path == "/api/fonts") { char b[600]; respond(fd, "application/json", b, fontsJson(b, sizeof b)); }
      else if (path == "/api/press" && post) { doPress(Press::Short, radar, watch, nowMs()); respondText(fd, "ok"); }
      else if (path == "/api/double" && post) { doPress(Press::Double, radar, watch, nowMs()); respondText(fd, "ok"); }
      else if (path == "/api/hold" && post) { doPress(Press::Long, radar, watch, nowMs()); if (!watch.away) { cfg.range_km = radar.range_km; saveConfig(); } respondText(fd, "ok"); }
      // Emergencies are rare, so this pretends there is one (over New York, where there is always traffic) for three
      // minutes, to try the notice and the button.
      else if (path == "/api/alert-test" && post) { startAlertTest(); respondText(fd, "ok"); }
      else if (path == "/api/reboot" && post) respondText(fd, "ok");            // nothing to restart here
      // The simulator has no Wi-Fi to set up; these stand in for the device so the setup page can be worked on here.
      // Any password joins, except "wrongpass", which fails the way a bad password does.
      else if (path == "/wifi") respond(fd, "text/html", WIFI_HTML, sizeof WIFI_HTML - 1);
      else if (path.rfind("/api/wifi/scan", 0) == 0)
        respondText(fd, "{\"scanning\":false,\"networks\":[{\"ssid\":\"Swamp 4077\",\"rssi\":-48,\"secure\":true},{\"ssid\":\"Rosie's Bar\",\"rssi\":-71,\"secure\":false},"
                        "{\"ssid\":\"Officers \\\"Club\\\"\",\"rssi\":-83,\"secure\":true}]}", "application/json");
      else if (path == "/api/wifi" && post) {
        char ssid[33], pass[65];
        if (!parseWifiRequest(body.data(), body.size(), ssid, pass)) respondText(fd, "{\"error\":\"bad request\"}", "application/json", 400);
        else { fake_wifi_ssid = ssid; fake_wifi_ok = strcmp(pass, "wrongpass") != 0; fake_wifi_at = time(nullptr); respondText(fd, "{\"ok\":true}", "application/json"); }
      }
      else if (path == "/api/wifi/status") {
        char esc[80]; jsonEscape(fake_wifi_ssid.c_str(), esc, sizeof esc);
        bool pending = time(nullptr) - fake_wifi_at < 3;
        std::string st = fake_wifi_ssid.empty() ? "idle" : pending ? "joining" : fake_wifi_ok ? "joined" : "failed";
        respondText(fd, std::string("{\"state\":\"") + st + "\",\"ssid\":\"" + esc + "\",\"ip\":\"192.168.1.77\",\"kept\":\"Swamp 4077\"}", "application/json");
      }
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
  int detail = -1, alert_demo = -1;
  bool want_card = false;
  const char *want_page = nullptr; int visitor_demo = -1;
  const char *on[8]; int n_on = 0;
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
    else if (a == "--detail" && i + 1 < argc) detail = atoi(argv[++i]);
    else if (a == "--live") want_live = true;
    else if (a == "--card") want_card = true;
    else if (a == "--on" && i + 1 < argc && n_on < 8) on[n_on++] = argv[++i];   // still frames: a switch that is off by default
    else if (a == "--page" && i + 1 < argc) want_page = argv[++i];           // still frames: weather, board or log, with made-up contents
    else if (a == "--visitor" && i + 1 < argc) visitor_demo = atoi(argv[++i]);  // still frames: this demo aircraft is an unusual visitor
    else if (a == "--alert" && i + 1 < argc) alert_demo = atoi(argv[++i]);      // still frames: this demo aircraft squawks 7700
    else if (a == "--clock" && i + 1 < argc) { int hh, mm; if (sscanf(argv[++i], "%d:%d", &hh, &mm) == 2) shot_clock = (hh % 24) * 60 + mm % 60; }
    else if (a == "--home" && i + 1 < argc) have_home = sscanf(argv[++i], "%lf,%lf", &home_lat, &home_lon) == 2;
    else {
      fprintf(stderr, "usage: %s [--port n] [--live --home lat,lon] [--no-map]\n       %s --shot out.bmp [--t ms] [--select n] [--range km] [--map] [--style 0-2] [--font id] [--on switch]\n", argv[0], argv[0]);
      return 2;
    }
  }
  if (want_live && shot) { fprintf(stderr, "--live cannot be used with --shot\n"); return 2; }
  const char *ua = getenv("RADARMASH_UA");
  tiles.user_agent = live.user_agent = logos.user_agent = ua && *ua ? ua : "RadarMASH-simulator/0.2 (+https://github.com/jamielovelock/RadarMASH)";

  if (!shot) { save_settings = true; loadConfig(); }      // a still frame always starts from the defaults
  if (have_home) { cfg.home_lat = home_lat; cfg.home_lon = home_lon; cfg.home_set = true; }
  if (want_live) cfg.live = true;
  if (cfg.live && !cfg.home_set && want_live) { fprintf(stderr, "--live needs --home lat,lon (or a position saved from the settings page)\n"); return 2; }
  if (range > 0) cfg.range_km = range;
  if (style >= 0) cfg.set.map_style = (MapStyle)(style % (int)MapStyle::Count);
  if (no_map) cfg.set.map = false;
  if (detail >= 0) cfg.set.map_detail = (uint8_t)(detail > 1 ? 1 : detail);
  if (font) {
    int f = fontIndex(font);
    if (f < 0) { fprintf(stderr, "no font called %s\n", font); return 2; }
    cfg.set.font = (uint8_t)f;
  }
  map_allowed = shot ? want_map : true;
  applyConfig();

  if (!shot) {
    live.background = [] { logos.fetchPending(radar_mu); };
    live.after_update = afterUpdate;
    live.start(&radar, &radar_mu, nowMs);
    return serve(port);
  }
  radar.set.auto_cycle = false;          // a still frame shows exactly the aircraft asked for
  demoTraffic(radar, t);
  radar.selected = select;
  if (want_card) radar.set.card = true;
  for (int i = 0; i < n_on; i++) {
    bool *sw = toggleByName(radar.set, on[i]);
    if (!sw) { fprintf(stderr, "no switch called %s\n", on[i]); return 2; }
    *sw = true;
  }
  if (alert_demo >= 0 && alert_demo < radar.count) {
    snprintf(radar.ac[alert_demo].squawk, sizeof radar.ac[alert_demo].squawk, "7700");
    watch.found(&radar.ac[alert_demo], 1);
  }
  if (visitor_demo >= 0 && visitor_demo < radar.count) {
    radar.ac[visitor_demo].shape = Shape::Military;
    snprintf(radar.ac[visitor_demo].type, sizeof radar.ac[visitor_demo].type, "A400");
    visitors.check(radar, cfg.visitor_types, false, t);
  }
  if (want_page) {
    Page p;
    if (!pageByName(want_page, p)) { fprintf(stderr, "no page called %s\n", want_page); return 2; }
    radar.page = (uint8_t)p;
    // made-up weather, and the demo traffic as if it had all just been seen passing
    static const char WX[] = "{\"utc_offset_seconds\":0,\"current\":{\"temperature_2m\":11.4,\"apparent_temperature\":8.2,\"relative_humidity_2m\":74,"
        "\"precipitation\":0.4,\"weather_code\":61,\"cloud_cover\":85,\"wind_speed_10m\":22.0,\"wind_direction_10m\":240,\"wind_gusts_10m\":41.0,"
        "\"pressure_msl\":1004.2,\"is_day\":1},\"daily\":{\"time\":[\"2026-01-07\",\"2026-01-08\",\"2026-01-09\"],\"weather_code\":[61,2,71],"
        "\"temperature_2m_max\":[12.3,9.2,3.8],\"temperature_2m_min\":[7.7,4.9,-1.6],\"precipitation_probability_max\":[90,20,65],"
        "\"sunrise\":[\"2026-01-07T08:05\",\"2026-01-08T08:05\",\"2026-01-09T08:04\"],\"sunset\":[\"2026-01-07T16:12\",\"2026-01-08T16:13\",\"2026-01-09T16:15\"]}}";
    parseWeather(WX, sizeof WX - 1, pages.wx);
    pages.seen.note(radar, 1800000000LL - 1800000000LL % 86400 + 14 * 3600 + 300, t);
    for (int i = 0; i < pages.seen.n; i++) { pages.seen.e[i].min_km = 0.6f + 0.7f * i; pages.seen.e[i].closest_at -= 240 * i; pages.seen.e[i].last -= 240 * i; pages.seen.e[i].last_ms -= 1000 * i; }
  }
  render(t);
  auto b = bmp();
  FILE *f = fopen(shot, "wb");
  if (!f) { perror(shot); return 1; }
  fwrite(b.data(), 1, b.size(), f); fclose(f);
  return 0;
}
