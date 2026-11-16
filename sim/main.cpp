// RadarMASH - open firmware for the GeekMagic Radar 360.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Desktop simulator. Builds the same drawing code as the device and shows it in a web browser, with no dependencies
// beyond a C++ compiler:
//   radarmash-sim                       serve the screen at http://localhost:8360 with made-up demo traffic
//   radarmash-sim --live --home LAT,LON real aircraft around that position, from the free feeds
//   radarmash-sim --shot out.bmp [--t ms] [--select n] [--range km] [--map]     write one frame and exit
// The map underlay downloads tiles with curl and keeps them in ~/.cache/radarmash. Still frames stay offline unless --map.
#include "../core/radar.h"
#include "live.h"
#include "tiles.h"
#include <arpa/inet.h>
#include <chrono>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <mutex>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

using namespace rm;

static uint16_t frame[SCREEN_W * SCREEN_H];
static Radar radar;
static std::mutex radar_mu;                // the live-data thread and the web requests both touch the radar
static Live live;
static bool live_mode = false;             // real aircraft instead of demo traffic
static ScopeCache scope_cache;             // the same speed-up the device uses, so the simulator shows the same picture

static uint32_t nowMs() {
  static const auto t0 = std::chrono::steady_clock::now();
  return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
}

// ---- map underlay ----
static SimTiles tiles;
static MapImage map_image;
static bool map_allowed = false;           // may the simulator go to the network for tiles?
static std::string map_credit = "(c) OpenStreetMap";
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
  snprintf(radar.map_credit, sizeof radar.map_credit, "%s", map_credit.c_str());
}

static std::string urlDecode(const std::string &in) {
  std::string out;
  for (size_t i = 0; i < in.size(); i++) {
    if (in[i] == '%' && i + 2 < in.size() + 0 && isxdigit((unsigned char)in[i + 1]) && isxdigit((unsigned char)in[i + 2])) {
      out += (char)strtol(in.substr(i + 1, 2).c_str(), nullptr, 16); i += 2;
    } else out += in[i] == '+' ? ' ' : in[i];
  }
  return out;
}
static std::string queryValue(const std::string &path, const std::string &name) {
  size_t q = path.find('?');
  if (q == std::string::npos) return "";
  std::string rest = path.substr(q + 1);
  for (size_t p = 0; p < rest.size();) {
    size_t e = rest.find('&', p); if (e == std::string::npos) e = rest.size();
    std::string kv = rest.substr(p, e - p); size_t eq = kv.find('=');
    if (eq != std::string::npos && kv.substr(0, eq) == name) return urlDecode(kv.substr(eq + 1));
    p = e + 1;
  }
  return "";
}
static std::string jsonEscape(const std::string &in) {
  std::string out;
  for (char ch : in) { if (ch == '"' || ch == '\\') out += '\\'; if ((unsigned char)ch >= 32) out += ch; }
  return out;
}

static void render(uint32_t t) {
  if (!live_mode) demoTraffic(radar, t);
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

static std::string settingsJson() {
  std::string j = "[";
  for (int i = 0; i < TOGGLE_COUNT; i++) {
    const Toggle &t = TOGGLES[i];
    if (j.size() > 1) j += ",";
    j += std::string("{\"name\":\"") + t.name + "\",\"label\":\"" + t.label + "\",\"on\":" + (radar.set.*t.field ? "true" : "false") + "}";
  }
  return j + "]";
}

static const char PAGE[] = R"HTML(<!doctype html><meta charset=utf-8><title>RadarMASH simulator</title>
<style>
body{background:#16181c;color:#bbb;font:14px system-ui;text-align:center;margin:0;padding:24px}
.case{display:inline-block;padding:14px;border-radius:50%;background:#050505;box-shadow:0 0 0 3px #2a2a2a}
img{display:block;width:540px;height:540px;border-radius:50%;image-rendering:pixelated;background:#000}
button{font:inherit;margin:14px 6px;padding:8px 16px} label{margin:0 8px;white-space:nowrap;display:inline-block;line-height:1.9} #t{margin:0 0 12px} #m,#f{margin:0 0 12px} input,select{font:inherit;background:#22252b;color:#ddd;border:1px solid #444;padding:3px} small{display:block;opacity:.6}
</style>
<div class=case><img id=s width=360 height=360 alt="radar screen"></div>
<div><button id=sp>Short press</button><button id=lp>Long press</button><button id=px>Actual pixels</button></div>
<div id=t></div>
<div id=m>Map source <input id=mu size=44> credit <input id=mc size=16>
 style <select id=ms><option value=0>As is<option value=1>Dark<option value=2>Green</select>
 brightness <input id=mb type=range min=0 max=100 style="vertical-align:middle;width:90px"> <button id=ma style="margin:4px">Apply</button>
 <span id=mi></span></div>
<div id=f>Aircraft feed <select id=fu><option value="https://opendata.adsb.fi/api/v3/lat/{lat}/lon/{lon}/dist/{nm}">adsb.fi<option value="https://api.adsb.lol/v2/point/{lat}/{lon}/{nm}">adsb.lol</select>
 refresh every <input id=fr type=number min=2 max=300 style="width:4em"> s, show at most <input id=fm type=number min=1 max=64 style="width:4em"> nearest
 <label><input id=fg type=checkbox> on the ground</label> <button id=fa style="margin:4px">Apply</button> <span id=fi></span></div>
<small>Short press: next aircraft. Long press: next range (zoom). Auto-cycle moves on every 5 s. <span id=dm>Demo traffic runs 20x faster than life.</span></small>
<script>
const s=document.getElementById('s');let big=true;
document.getElementById('sp').onclick=()=>fetch('/press');
document.getElementById('lp').onclick=()=>fetch('/hold');
document.getElementById('px').onclick=()=>{big=!big;s.style.width=s.style.height=(big?540:360)+'px'};
fetch('/settings').then(r=>r.json()).then(a=>{const t=document.getElementById('t');for(const o of a){const l=document.createElement('label'),
c=document.createElement('input');c.type='checkbox';c.checked=o.on;c.onchange=()=>fetch('/toggle/'+o.name);l.append(c,' '+o.label);t.append(l)}});
const $=i=>document.getElementById(i);
function mapInfo(fill){fetch('/mapinfo').then(r=>r.json()).then(j=>{if(fill){$('mu').value=j.url;$('mc').value=j.credit;$('ms').value=j.style;$('mb').value=j.bright}
$('mi').textContent='range '+j.range+' km, zoom '+j.zoom+(j.missing?', '+j.missing+' tiles missing':'')})}
function mapApply(){fetch('/map?url='+encodeURIComponent($('mu').value)+'&credit='+encodeURIComponent($('mc').value)+'&style='+$('ms').value+'&bright='+$('mb').value).then(()=>setTimeout(mapInfo,1500))}
$('ma').onclick=mapApply;$('ms').onchange=mapApply;$('mb').onchange=mapApply;mapInfo(true);setInterval(mapInfo,3000);
function feedInfo(fill){fetch('/feedinfo').then(r=>r.json()).then(j=>{if(fill){$('fr').value=j.refresh;$('fm').value=j.max;$('fg').checked=j.ground}
$('fu').value=j.url;$('f').style.display=j.live?'':'none';$('dm').style.display=j.live?'none':'';$('fi').textContent=j.count<0?'waiting for the first reply':j.count+' aircraft, updated '+j.age+' s ago'+(j.error?' ('+j.error+')':'')})}
$('fa').onclick=()=>fetch('/feed?url='+encodeURIComponent($('fu').value)+'&refresh='+$('fr').value+'&max='+$('fm').value+'&ground='+($('fg').checked?1:0)).then(()=>feedInfo());
feedInfo(true);setInterval(feedInfo,2000);
async function loop(){for(;;){try{const b=await(await fetch('/frame')).blob();const u=URL.createObjectURL(b);
await new Promise(r=>{s.onload=s.onerror=r;s.src=u});URL.revokeObjectURL(u)}catch(e){await new Promise(r=>setTimeout(r,500))}
await new Promise(r=>setTimeout(r,50))}}loop();
</script>)HTML";

static void sendAll(int fd, const void *d, size_t n) {
  const char *p = (const char *)d;
  while (n) { ssize_t w = send(fd, p, n, 0); if (w <= 0) return; p += w; n -= (size_t)w; }
}
static void respond(int fd, const char *type, const void *body, size_t n) {
  char h[160];
  int hl = snprintf(h, sizeof h, "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n", type, n);
  sendAll(fd, h, (size_t)hl); sendAll(fd, body, n);
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
    char req[2048]; ssize_t n = recv(fd, req, sizeof req - 1, 0);
    if (n > 0) {
      req[n] = 0;
      std::lock_guard<std::mutex> lock(radar_mu);
      std::string path = "/";
      if (char *sp = strchr(req, ' ')) { char *e = strchr(sp + 1, ' '); if (e) path.assign(sp + 1, e); }
      if (path == "/frame") { render(nowMs()); auto b = bmp(); respond(fd, "image/bmp", b.data(), b.size()); }
      else if (path == "/press") { radar.shortPress(); respond(fd, "text/plain", "ok", 2); }
      else if (path == "/hold") { radar.longPress(); respond(fd, "text/plain", "ok", 2); }
      else if (path.rfind("/toggle/", 0) == 0) {
        bool *f = toggleByName(radar.set, path.substr(8).c_str());
        if (f) *f = !*f;
        respond(fd, "text/plain", "ok", 2);
      }
      else if (path.rfind("/map?", 0) == 0) {               // map source, credit, style and brightness from the page
        std::string u = queryValue(path, "url"), c = queryValue(path, "credit"), st = queryValue(path, "style"), b = queryValue(path, "bright");
        if (!u.empty()) tiles.url_template = u;
        if (path.find("credit=") != std::string::npos) map_credit = c;
        if (!st.empty()) radar.set.map_style = (MapStyle)(atoi(st.c_str()) % (int)MapStyle::Count);
        if (!b.empty()) { int v = atoi(b.c_str()); radar.set.map_brightness = (uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v); }
        respond(fd, "text/plain", "ok", 2);
      }
      else if (path.rfind("/feed?", 0) == 0) {              // feed source and refresh rate from the page
        std::string u = queryValue(path, "url"), rf = queryValue(path, "refresh"), g = queryValue(path, "ground");
        if (!u.empty()) live.feed_url = u;
        if (!rf.empty()) { int v = atoi(rf.c_str()); live.refresh_s = v < 2 ? 2 : v > 300 ? 300 : v; }
        if (!g.empty()) live.include_ground = g == "1";
        std::string mx = queryValue(path, "max");
        if (!mx.empty()) { int v = atoi(mx.c_str()); radar.set.max_aircraft = (uint8_t)(v < 1 ? 1 : v > MAX_AIRCRAFT ? MAX_AIRCRAFT : v); }
        respond(fd, "text/plain", "ok", 2);
      }
      else if (path == "/feedinfo") {
        char j[700];
        long age = live.last_ok ? (long)time(nullptr) - live.last_ok : -1;
        int n = snprintf(j, sizeof j, "{\"live\":%s,\"url\":\"%s\",\"refresh\":%d,\"max\":%d,\"ground\":%s,\"count\":%d,\"age\":%ld,\"errors\":%d,\"lookups\":%d,\"error\":\"%s\"}",
                         live_mode ? "true" : "false", jsonEscape(live.feed_url).c_str(), live.refresh_s, (int)radar.set.max_aircraft, live.include_ground ? "true" : "false",
                         live.last_count, age, live.errors, live.lookups, jsonEscape(live.last_error).c_str());
        respond(fd, "application/json", j, (size_t)(n < (int)sizeof j ? n : (int)sizeof j - 1));
      }
      else if (path == "/mapinfo") {
        char j[700];
        int n = snprintf(j, sizeof j, "{\"url\":\"%s\",\"credit\":\"%s\",\"style\":%d,\"bright\":%d,\"zoom\":%d,\"missing\":%d,\"range\":%d,\"fetched\":%d,\"failed\":%d}",
                         jsonEscape(tiles.url_template).c_str(), jsonEscape(map_credit).c_str(), (int)radar.set.map_style, (int)radar.set.map_brightness,
                         map_image.zoom, map_image.missing, (int)radar.range_km, tiles.fetched, tiles.failed);
        respond(fd, "application/json", j, (size_t)(n < (int)sizeof j ? n : (int)sizeof j - 1));
      }
      else if (path == "/settings") { std::string j = settingsJson(); respond(fd, "application/json", j.data(), j.size()); }
      else respond(fd, "text/html", PAGE, sizeof PAGE - 1);
    }
    close(fd);
  }
}

int main(int argc, char **argv) {
  const char *shot = nullptr; uint32_t t = 2600; int select = -1, port = 8360; float range = 0; bool want_map = false;
  tiles.url_template = "https://tile.openstreetmap.org/{z}/{x}/{y}.png";
  const char *ua = getenv("RADARMASH_UA");
  tiles.user_agent = live.user_agent = ua && *ua ? ua : "RadarMASH-simulator/0.1";
  bool have_home = false;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--shot" && i + 1 < argc) shot = argv[++i];
    else if (a == "--t" && i + 1 < argc) t = (uint32_t)strtoul(argv[++i], nullptr, 10);
    else if (a == "--select" && i + 1 < argc) select = atoi(argv[++i]);
    else if (a == "--range" && i + 1 < argc) range = (float)atof(argv[++i]);
    else if (a == "--port" && i + 1 < argc) port = atoi(argv[++i]);
    else if (a == "--map") want_map = true;
    else if (a == "--live") live_mode = true;
    else if (a == "--home" && i + 1 < argc) { have_home = sscanf(argv[++i], "%lf,%lf", &radar.home_lat, &radar.home_lon) == 2; }
    else if (a == "--no-map") want_map = false, shot = shot ? shot : nullptr, map_allowed = false, radar.set.map = false;
    else if (a == "--style" && i + 1 < argc) radar.set.map_style = (MapStyle)(atoi(argv[++i]) % (int)MapStyle::Count);
    else { fprintf(stderr, "usage: %s [--port n] | --shot out.bmp [--t ms] [--select n] [--range km] [--map] [--style 0-2]\n       --no-map with no --shot serves without a map\n", argv[0]); return 2; }
  }
  if (range > 0) radar.range_km = range;
  map_allowed = shot ? want_map : radar.set.map;        // a still frame stays offline unless asked for --map
  if (live_mode && (!have_home || shot)) { fprintf(stderr, "--live needs --home lat,lon and cannot be used with --shot\n"); return 2; }
  if (live_mode) live.start(&radar, &radar_mu, nowMs);
  if (!shot) return serve(port);
  demoTraffic(radar, t);
  radar.set.auto_cycle = false;          // a still frame shows exactly the aircraft asked for
  radar.selected = select;
  render(t);
  auto b = bmp();
  FILE *f = fopen(shot, "wb");
  if (!f) { perror(shot); return 1; }
  fwrite(b.data(), 1, b.size(), f); fclose(f);
  return 0;
}
