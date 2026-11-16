// RadarMASH - open firmware for the GeekMagic Radar 360.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Desktop simulator. Builds the same drawing code as the device and shows it in a web browser, with no dependencies
// beyond a C++ compiler:
//   radarmash-sim                       serve the live screen at http://localhost:8360
//   radarmash-sim --shot out.bmp [--t ms] [--select n] [--range km]     write one frame and exit
#include "../core/radar.h"
#include <arpa/inet.h>
#include <chrono>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

using namespace rm;

static uint16_t frame[SCREEN_W * SCREEN_H];
static Radar radar;

static uint32_t nowMs() {
  static const auto t0 = std::chrono::steady_clock::now();
  return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
}

static void render(uint32_t t) {
  demoTraffic(radar, t);
  radar.tick(t);
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

// Feature switches, by the names the web page uses. The device's settings page will use the same names.
static const struct { const char *name, *label; bool Settings::*field; } TOGGLES[] = {
  {"sweep", "Sweep", &Settings::sweep}, {"rings", "Range rings", &Settings::rings}, {"labels", "Callsigns", &Settings::labels},
  {"altitude", "Altitude", &Settings::altitude}, {"trails", "Trails", &Settings::trails},
  {"shapes", "Aircraft shapes", &Settings::shapes}, {"auto_cycle", "Auto-cycle", &Settings::auto_cycle},
  {"tag", "Tag box", &Settings::tag}, {"tag_route", "Tag: route", &Settings::tag_route}, {"tag_type", "Tag: type", &Settings::tag_type},
  {"tag_level", "Tag: level", &Settings::tag_level}, {"tag_speed", "Tag: speed", &Settings::tag_speed},
  {"card", "Large card", &Settings::card},
};
static bool *settingByName(const std::string &n) {
  for (auto &t : TOGGLES) if (n == t.name) return &(radar.set.*t.field);
  return nullptr;
}
static std::string settingsJson() {
  std::string j = "[";
  for (auto &t : TOGGLES) {
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
button{font:inherit;margin:14px 6px;padding:8px 16px} label{margin:0 8px;white-space:nowrap;display:inline-block;line-height:1.9} #t{margin:0 0 12px} small{display:block;opacity:.6}
</style>
<div class=case><img id=s width=360 height=360 alt="radar screen"></div>
<div><button id=sp>Short press</button><button id=lp>Long press</button><button id=px>Actual pixels</button></div>
<div id=t></div>
<small>Short press: next aircraft. Long press: change range. Auto-cycle moves on every 5 s. Demo traffic runs 20x faster than life.</small>
<script>
const s=document.getElementById('s');let big=true;
document.getElementById('sp').onclick=()=>fetch('/press');
document.getElementById('lp').onclick=()=>fetch('/hold');
document.getElementById('px').onclick=()=>{big=!big;s.style.width=s.style.height=(big?540:360)+'px'};
fetch('/settings').then(r=>r.json()).then(a=>{const t=document.getElementById('t');for(const o of a){const l=document.createElement('label'),
c=document.createElement('input');c.type='checkbox';c.checked=o.on;c.onchange=()=>fetch('/toggle/'+o.name);l.append(c,' '+o.label);t.append(l)}});
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
      std::string path = "/";
      if (char *sp = strchr(req, ' ')) { char *e = strchr(sp + 1, ' '); if (e) path.assign(sp + 1, e); }
      if (path == "/frame") { render(nowMs()); auto b = bmp(); respond(fd, "image/bmp", b.data(), b.size()); }
      else if (path == "/press") { radar.shortPress(); respond(fd, "text/plain", "ok", 2); }
      else if (path == "/hold") { radar.longPress(); respond(fd, "text/plain", "ok", 2); }
      else if (path.rfind("/toggle/", 0) == 0) {
        bool *f = settingByName(path.substr(8));
        if (f) *f = !*f;
        respond(fd, "text/plain", "ok", 2);
      }
      else if (path == "/settings") { std::string j = settingsJson(); respond(fd, "application/json", j.data(), j.size()); }
      else respond(fd, "text/html", PAGE, sizeof PAGE - 1);
    }
    close(fd);
  }
}

int main(int argc, char **argv) {
  const char *shot = nullptr; uint32_t t = 2600; int select = -1, port = 8360; float range = 0;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--shot" && i + 1 < argc) shot = argv[++i];
    else if (a == "--t" && i + 1 < argc) t = (uint32_t)strtoul(argv[++i], nullptr, 10);
    else if (a == "--select" && i + 1 < argc) select = atoi(argv[++i]);
    else if (a == "--range" && i + 1 < argc) range = (float)atof(argv[++i]);
    else if (a == "--port" && i + 1 < argc) port = atoi(argv[++i]);
    else { fprintf(stderr, "usage: %s [--port n] | --shot out.bmp [--t ms] [--select n] [--range km]\n", argv[0]); return 2; }
  }
  if (range > 0) radar.range_km = range;
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
