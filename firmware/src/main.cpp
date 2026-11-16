// RadarMASH - open firmware for the GeekMagic Radar 360.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Device firmware (development stage). Draws the same radar as the simulator, from demo traffic or a live feed.
//
// Staying recoverable matters more than anything else here: the case is sealed and Wi-Fi is the only way in.
//  - The bootloader has rollback on. This image never confirms itself at boot. Until it is confirmed, any reset returns
//    to the firmware in the other slot, and it re-points the boot selector there at start-up to make sure.
//  - Unconfirmed and left alone for 30 minutes, it restarts (back to the other slot).
//  - /ota writes only the other slot. Before erasing it, this image confirms itself: by then it has joined Wi-Fi and is
//    receiving the upload, and a half-written slot must never be the only thing left. /ota/dry receives without writing.
//  - No map yet: that needs the PSRAM, which has not been verified with this build.
#include <Arduino.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_psram.h>
#include <mbedtls/sha256.h>

#include "feed.h"
#include "panel_init.h"
#include "radar.h"
#include "secrets.h"

#define FW_VERSION "0.1.0-dev"
#define USER_AGENT "RadarMASH/" FW_VERSION

extern "C" bool verifyRollbackLater() { return true; }   // stop the Arduino core confirming the image for us

using namespace rm;

// ---- board ----
static const int PIN_MOSI = 11, PIN_SCLK = 12, PIN_DC = 13, PIN_RST = 8, PIN_BL = 5, PIN_BTN = 38, PIN_BUZZ = 9;
static const uint32_t SPI_HZ = 80000000;
static const int BAND_ROWS = 40;

static WebServer server(80);
static Radar radar;
static SemaphoreHandle_t radar_mu;                 // the feed task and the web handlers both touch the radar
struct Lock { Lock() { xSemaphoreTake(radar_mu, portMAX_DELAY); } ~Lock() { xSemaphoreGive(radar_mu); } };

static const esp_partition_t *running_p = nullptr, *other_p = nullptr;
static esp_err_t arm_err = -1;
static uint32_t last_activity_ms = 0, wifi_lost_ms = 0;
static uint32_t frame_ms = 0, frames = 0;
static int brightness = 1023;
static char boot_note[40] = "";

// True only if the external memory chip really answered and is usable as heap (the Arduino flag alone is not proof).
static bool psramOk() { return esp_psram_is_initialized() && heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0; }

// ---- panel ----
static uint16_t band[SCREEN_W * BAND_ROWS];        // one strip of the picture, upright
static uint16_t turned[SCREEN_W * BAND_ROWS];      // the same strip as the panel wants it

static void panelCmd(uint8_t cmd, const uint8_t *data, size_t n) {
  digitalWrite(PIN_DC, LOW); SPI.write(cmd);
  digitalWrite(PIN_DC, HIGH);
  if (n) SPI.writeBytes(data, n);
}

static void panelInit() {
  pinMode(PIN_DC, OUTPUT); digitalWrite(PIN_DC, HIGH);
  pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, HIGH); delay(5);
  digitalWrite(PIN_RST, LOW); delay(10); digitalWrite(PIN_RST, HIGH); delay(120);
  SPI.begin(PIN_SCLK, -1, PIN_MOSI, -1);
  const uint8_t *p = PANEL_INIT;
  for (uint16_t i = 0; i < PANEL_INIT_COUNT; i++) {
    SPI.beginTransaction(SPISettings(SPI_HZ, MSBFIRST, SPI_MODE0));
    panelCmd(p[0], p + 3, p[2]);
    SPI.endTransaction();
    if (p[1]) delay(p[1]);
    p += 3 + p[2];
  }
}

// The panel is mounted a quarter turn round: upright (x, y) is panel column 359 - y, panel row x. So an upright strip of
// rows becomes a block of panel columns, sent row by row, most-significant byte first.
static void pushBand(int y0) {
  int n = 0;
  for (int x = 0; x < SCREEN_W; x++)
    for (int y = BAND_ROWS - 1; y >= 0; y--) turned[n++] = __builtin_bswap16(band[y * SCREEN_W + x]);
  int c0 = SCREEN_H - BAND_ROWS - y0, c1 = SCREEN_H - 1 - y0;
  uint8_t cols[4] = {(uint8_t)(c0 >> 8), (uint8_t)c0, (uint8_t)(c1 >> 8), (uint8_t)c1};
  uint8_t rows[4] = {0, 0, (uint8_t)((SCREEN_W - 1) >> 8), (uint8_t)(SCREEN_W - 1)};
  panelCmd(0x2A, cols, 4); panelCmd(0x2B, rows, 4); panelCmd(0x2C, nullptr, 0);
  SPI.writeBytes((const uint8_t *)turned, sizeof turned);
}

static void setBacklight(int v) {                  // 0..1023. The pin is active low.
  brightness = constrain(v, 0, 1023);
  ledcWrite(PIN_BL, 1023 - brightness);
}

static void drawFrame(bool demo) {
  uint32_t t0 = millis();
  Lock lock;
  if (demo) demoTraffic(radar, t0);
  radar.tick(t0);
  SPI.beginTransaction(SPISettings(SPI_HZ, MSBFIRST, SPI_MODE0));
  for (int y0 = 0; y0 < SCREEN_H; y0 += BAND_ROWS) {
    Canvas c(band, y0, BAND_ROWS);
    radar.draw(c);
    if (boot_note[0]) c.textCentred(180, 300, boot_note, 2, rgb(255, 240, 80));
    pushBand(y0);
  }
  SPI.endTransaction();
  frame_ms = millis() - t0; frames++;
}

// ---- which firmware boots ----
static int runningState() { esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED; esp_ota_get_state_partition(running_p, &st); return (int)st; }
static bool confirmed() {
  const esp_partition_t *b = esp_ota_get_boot_partition();
  return b && b->address == running_p->address && runningState() == ESP_OTA_IMG_VALID;
}
static bool confirmSelf() {
  if (esp_ota_set_boot_partition(running_p) != ESP_OK) return false;
  esp_ota_mark_app_valid_cancel_rollback();
  return confirmed();
}
static bool otherHasApp() { esp_app_desc_t d; return other_p && esp_ota_get_partition_description(other_p, &d) == ESP_OK; }
static void goHome() {
  if (otherHasApp()) esp_ota_set_boot_partition(other_p);
  delay(200); ESP.restart();
}

// ---- live data (its own task, so a slow reply never freezes the sweep) ----
static bool live_mode = false;
static int refresh_s = 5;
static int feed_count = -1, feed_errors = 0, feed_failing = 0, lookups = 0;
static uint32_t feed_ok_ms = 0;
static bool use_fi = true;                          // which of the two free feeds; swapped when one stops answering
static Aircraft fresh[MAX_AIRCRAFT];

struct RouteEntry { char callsign[9]; RouteInfo info; bool known; uint32_t when_ms; };
static RouteEntry route_cache[48];
static int route_next = 0;
static RouteEntry *findRoute(const char *cs) {
  for (RouteEntry &e : route_cache) if (e.callsign[0] && strcmp(e.callsign, cs) == 0) return &e;
  return nullptr;
}

static bool httpsGet(const String &url, String &body) {
  WiFiClientSecure client;
  client.setInsecure();                             // development build: certificates are not checked yet
  HTTPClient http;
  http.setUserAgent(USER_AGENT);
  http.setConnectTimeout(8000); http.setTimeout(8000);
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  bool ok = code == 200 || code == 404;             // 404 is the route service's "unknown callsign"
  if (ok) body = http.getString();
  http.end();
  return ok && body.length() > 0;
}

static void pollFeed() {
  double lat, lon; int nm, limit;
  { Lock lock; lat = radar.home_lat; lon = radar.home_lon; nm = feedRadiusNm(radar.range_km); limit = constrain((int)radar.set.max_aircraft, 1, MAX_AIRCRAFT); }
  String url = use_fi ? FEED_ADSB_FI : FEED_ADSB_LOL;
  url.replace("{lat}", String(lat, 4)); url.replace("{lon}", String(lon, 4)); url.replace("{nm}", String(nm));
  String body;
  int n = httpsGet(url, body) ? parseAircraft(body.c_str(), body.length(), lat, lon, false, fresh, limit) : -1;
  body = String();
  Lock lock;
  if (n < 0) {
    feed_errors++;
    if (++feed_failing >= 2) { use_fi = !use_fi; feed_failing = 0; }
    return;
  }
  for (int i = 0; i < n; i++) {
    RouteEntry *e = findRoute(fresh[i].callsign);
    if (!e || !e->known) continue;
    memcpy(fresh[i].airline, e->info.airline, sizeof fresh[i].airline);
    memcpy(fresh[i].origin, e->info.origin, sizeof fresh[i].origin);
    memcpy(fresh[i].dest, e->info.dest, sizeof fresh[i].dest);
  }
  radar.update(fresh, n, millis());
  feed_count = n; feed_ok_ms = millis(); feed_failing = 0;
}

static void lookupOneRoute() {
  char cs[9] = "";
  {
    Lock lock;
    for (int i = 0; i < radar.count; i++) {
      float x, y;
      const char *c = radar.ac[i].callsign;
      if (!lookupableCallsign(c) || !radar.project(radar.ac[i], x, y)) continue;
      RouteEntry *e = findRoute(c);
      if (e && (e->known || millis() - e->when_ms < 30UL * 60UL * 1000UL)) continue;
      memcpy(cs, c, sizeof cs);
      break;
    }
  }
  if (!cs[0]) return;
  String url = ROUTE_ADSBDB, body;
  url.replace("{callsign}", cs);
  RouteInfo info;
  RouteResult res = httpsGet(url, body) ? parseRoute(body.c_str(), body.length(), info) : RouteResult::Bad;
  Lock lock;
  lookups++;
  if (res == RouteResult::Bad) return;
  RouteEntry *e = findRoute(cs);
  if (!e) { e = &route_cache[route_next]; route_next = (route_next + 1) % (int)(sizeof route_cache / sizeof route_cache[0]); }
  memcpy(e->callsign, cs, sizeof e->callsign); e->info = info; e->known = res == RouteResult::Found; e->when_ms = millis();
  if (!e->known) return;
  for (int i = 0; i < radar.count; i++) {
    Aircraft &a = radar.ac[i];
    if (strcmp(a.callsign, cs) != 0) continue;
    memcpy(a.airline, info.airline, sizeof a.airline); memcpy(a.origin, info.origin, sizeof a.origin); memcpy(a.dest, info.dest, sizeof a.dest);
  }
}

static void feedTask(void *) {
  for (;;) {
    uint32_t began = millis();
    if (live_mode && WiFi.status() == WL_CONNECTED) { pollFeed(); lookupOneRoute(); }
    uint32_t every = (uint32_t)constrain(refresh_s, 2, 300) * 1000UL, took = millis() - began;
    vTaskDelay(pdMS_TO_TICKS(took < every ? every - took : 200));
  }
}

// ---- firmware update over Wi-Fi: POST the raw image, as the stock firmware's updater takes it ----
static struct { bool active, dry, failed, begun, ok; esp_ota_handle_t h; size_t total; String msg; mbedtls_sha256_context sha; uint8_t digest[32]; } ota;

static void otaFail(const String &m) { if (!ota.failed) { ota.failed = true; ota.msg = m; } }
static void otaRaw(bool dry) {                      // dry comes from the route: query arguments are not parsed yet at this point
  HTTPRaw &r = server.raw();
  last_activity_ms = millis();
  if (r.status == RAW_START) {
    if (ota.begun) esp_ota_abort(ota.h);
    ota.active = true; ota.dry = dry; ota.failed = false; ota.begun = false; ota.ok = false; ota.total = 0; ota.msg = "";
    mbedtls_sha256_init(&ota.sha); mbedtls_sha256_starts(&ota.sha, 0);
  } else if (r.status == RAW_WRITE) {
    if (ota.failed || !r.currentSize) return;
    if (ota.total == 0) {
      if (r.currentSize < 24 || r.buf[0] != 0xE9) return otaFail("not an ESP app image");
      if (r.buf[12] != 0x09 || r.buf[13] != 0x00) return otaFail("image is not for ESP32-S3");
      if (!other_p) return otaFail("no other slot");
      if (!ota.dry) {
        if (!confirmed() && !confirmSelf()) return otaFail("could not confirm running image; flash untouched");
        esp_err_t e = esp_ota_begin(other_p, OTA_WITH_SEQUENTIAL_WRITES, &ota.h);
        if (e != ESP_OK) return otaFail(String("esp_ota_begin ") + esp_err_to_name(e));
        ota.begun = true;
      }
    }
    if (ota.total + r.currentSize > other_p->size) return otaFail("image larger than slot");
    mbedtls_sha256_update(&ota.sha, r.buf, r.currentSize);
    if (ota.begun) { esp_err_t e = esp_ota_write(ota.h, r.buf, r.currentSize); if (e != ESP_OK) return otaFail(String("esp_ota_write ") + esp_err_to_name(e)); }
    ota.total += r.currentSize;
  } else if (r.status == RAW_END) {
    mbedtls_sha256_finish(&ota.sha, ota.digest); mbedtls_sha256_free(&ota.sha);
    if (ota.begun) {
      esp_err_t e = ota.failed ? (esp_ota_abort(ota.h), ESP_FAIL) : esp_ota_end(ota.h);     // esp_ota_end checks the image
      ota.begun = false;
      if (!ota.failed && e != ESP_OK) otaFail(String("image failed verification: ") + esp_err_to_name(e));
      if (!ota.failed) { e = esp_ota_set_boot_partition(other_p); if (e != ESP_OK) otaFail(String("set boot ") + esp_err_to_name(e)); }
    }
    if (!ota.failed && ota.total == 0) otaFail("empty upload");
    ota.ok = !ota.failed;
  } else {
    if (ota.begun) { esp_ota_abort(ota.h); ota.begun = false; }
    mbedtls_sha256_free(&ota.sha);
    otaFail("upload aborted");
  }
}
static void hOtaRaw() { otaRaw(false); }
static void hOtaDryRaw() { otaRaw(true); }
static void hOtaDone() {
  last_activity_ms = millis();
  if (!ota.active) { server.send(400, "application/json", "{\"ok\":false,\"msg\":\"no body; POST the raw image\"}"); return; }
  ota.active = false;
  char hex[65]; for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", ota.digest[i]);
  String j = String("{\"ok\":") + (ota.ok ? "true" : "false") + ",\"dry\":" + (ota.dry ? "true" : "false") + ",\"bytes\":" + String(ota.total);
  j += String(",\"sha256\":\"") + (ota.ok ? hex : "") + "\",\"slot\":\"" + (other_p ? other_p->label : "none") + "\",\"msg\":\"" + ota.msg + "\"";
  j += String(",\"rebooting\":") + (ota.ok && !ota.dry ? "true" : "false") + "}";
  server.send(ota.ok ? 200 : 500, "application/json", j);
  if (ota.ok && !ota.dry) { delay(500); ESP.restart(); }
}

// ---- web ----
static void touch() { last_activity_ms = millis(); }
static void ok() { server.send(200, "text/plain", "ok"); }

static void hStatus() {
  touch();
  const esp_partition_t *b = esp_ota_get_boot_partition();
  Lock lock;
  String j = "{\"fw\":\"radarmash\",\"version\":\"" FW_VERSION "\"";
  j += ",\"running\":\"" + String(running_p->label) + "\",\"running_state\":" + String(runningState());
  j += ",\"boot\":\"" + String(b ? b->label : "?") + "\",\"other\":\"" + String(other_p ? other_p->label : "none") + "\"";
  j += ",\"confirmed\":" + String(confirmed() ? "true" : "false") + ",\"arm_err\":" + String((int)arm_err);
  j += ",\"frame_ms\":" + String(frame_ms) + ",\"frames\":" + String(frames) + ",\"brightness\":" + String(brightness);
  j += ",\"live\":" + String(live_mode ? "true" : "false") + ",\"feed\":\"" + String(use_fi ? "adsb.fi" : "adsb.lol") + "\"";
  j += ",\"aircraft\":" + String(radar.count) + ",\"feed_count\":" + String(feed_count) + ",\"feed_errors\":" + String(feed_errors);
  j += ",\"feed_age_ms\":" + String(feed_ok_ms ? (long)(millis() - feed_ok_ms) : -1L) + ",\"lookups\":" + String(lookups);
  j += ",\"refresh_s\":" + String(refresh_s) + ",\"max_aircraft\":" + String((int)radar.set.max_aircraft) + ",\"range_km\":" + String((int)radar.range_km);
  j += ",\"selected\":" + String(radar.selected);
  j += ",\"reset_reason\":" + String((int)esp_reset_reason()) + ",\"uptime_ms\":" + String(millis());
  j += ",\"psram\":" + String(psramOk() ? "true" : "false") + ",\"psram_size\":" + String((unsigned)esp_psram_get_size());
  j += ",\"psram_heap\":" + String((unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM)) + ",\"psram_free\":" + String((unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  j += ",\"heap\":" + String(ESP.getFreeHeap()) + ",\"heap_min\":" + String(ESP.getMinFreeHeap()) + ",\"rssi\":" + String(WiFi.RSSI()) + "}";
  server.send(200, "application/json", j);
}
static void hSettings() {
  touch(); Lock lock;
  String j = "[";
  for (int i = 0; i < TOGGLE_COUNT; i++) {
    if (i) j += ",";
    j += String("{\"name\":\"") + TOGGLES[i].name + "\",\"label\":\"" + TOGGLES[i].label + "\",\"on\":" + (radar.set.*TOGGLES[i].field ? "true" : "false") + "}";
  }
  server.send(200, "application/json", j + "]");
}
static void hToggle() {
  touch(); Lock lock;
  bool *f = toggleByName(radar.set, server.arg("name").c_str());
  if (!f) { server.send(404, "text/plain", "no such switch"); return; }
  *f = server.hasArg("on") ? server.arg("on") == "1" : !*f;
  ok();
}
static void hPress() { touch(); { Lock lock; radar.shortPress(); } ok(); }
static void hHold() { touch(); { Lock lock; radar.longPress(); } ok(); }
static void hBl() { touch(); setBacklight(server.arg("v").toInt()); ok(); }
// Home position is held in memory only for now, so nothing about where the device lives is written to flash.
static void hHome() {
  touch();
  double lat = server.arg("lat").toDouble(), lon = server.arg("lon").toDouble();
  if (!server.hasArg("lat") || !server.hasArg("lon") || lat < -90 || lat > 90 || lon < -180 || lon > 180) { server.send(400, "text/plain", "lat=..&lon=.."); return; }
  Lock lock;
  radar.home_lat = lat; radar.home_lon = lon;
  radar.count = 0; radar.selected = -1; feed_count = -1;
  live_mode = true;
  ok();
}
static void hDemo() { touch(); { Lock lock; live_mode = false; radar.count = 0; radar.selected = -1; } ok(); }
static void hFeed() {
  touch(); Lock lock;
  if (server.hasArg("refresh")) refresh_s = constrain((int)server.arg("refresh").toInt(), 2, 300);
  if (server.hasArg("max")) radar.set.max_aircraft = (uint8_t)constrain((int)server.arg("max").toInt(), 1, MAX_AIRCRAFT);
  ok();
}
// Exercise the external memory chip: write a pattern across a block, read it back, and time both.
static void hPsramTest() {
  touch();
  if (!psramOk()) { server.send(200, "application/json", "{\"found\":false}"); return; }
  size_t bytes = (size_t)constrain((int)server.arg("mb").toInt(), 1, 6) * 1024 * 1024;
  uint32_t *p = (uint32_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
  if (!p) { server.send(200, "application/json", "{\"found\":true,\"alloc\":false}"); return; }
  size_t n = bytes / 4, bad = 0;
  uint32_t t0 = millis();
  for (size_t i = 0; i < n; i++) p[i] = (uint32_t)(i * 2654435761UL) ^ 0xA5A5A5A5UL;
  uint32_t t1 = millis();
  for (size_t i = 0; i < n; i++) bad += p[i] != ((uint32_t)(i * 2654435761UL) ^ 0xA5A5A5A5UL);
  uint32_t t2 = millis();
  heap_caps_free(p);
  server.send(200, "application/json", String("{\"found\":true,\"alloc\":true,\"bytes\":") + String(bytes) + ",\"bad_words\":" + String(bad) +
                                         ",\"write_ms\":" + String(t1 - t0) + ",\"read_ms\":" + String(t2 - t1) + "}");
}
static void hStay() { touch(); bool c = confirmSelf(); server.send(c ? 200 : 500, "text/plain", c ? "confirmed: this image now survives resets" : "confirm failed"); }
static void hStock() {
  touch();
  esp_err_t e = otherHasApp() ? esp_ota_set_boot_partition(other_p) : ESP_FAIL;
  server.send(e == ESP_OK ? 200 : 500, "text/plain", String("next boot: ") + (e == ESP_OK ? other_p->label : "unchanged (other slot has no app)"));
}
static void hReboot() { server.send(200, "text/plain", "rebooting"); delay(300); ESP.restart(); }

static const char UI[] PROGMEM = R"HTML(<!doctype html><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1"><title>RadarMASH</title>
<style>body{font:15px system-ui;max-width:36em;margin:1em auto;padding:0 1em}button{margin:.2em;padding:.5em .8em}label{display:inline-block;margin:.2em .6em .2em 0}pre{background:#eee;padding:.6em;overflow:auto}</style>
<h3>RadarMASH <small id=v></small></h3>
<p><button onclick="g('/press')">Short press</button><button onclick="g('/hold')">Long press</button>
 Backlight <button onclick="g('/bl?v=80')">dim</button><button onclick="g('/bl?v=400')">mid</button><button onclick="g('/bl?v=1023')">full</button></p>
<p id=t></p>
<p>Live aircraft: lat <input id=la size=8> lon <input id=lo size=8> <button onclick="g('/home?lat='+la.value+'&lon='+lo.value)">Go live</button><button onclick="g('/demo')">Demo</button><br>
 refresh <input id=rf size=3> s, show at most <input id=mx size=3> <button onclick="g('/feed?refresh='+rf.value+'&max='+mx.value)">Apply</button></p>
<p>Boot: <button onclick="g('/stock')">next boot = other firmware</button><button onclick="g('/reboot')">reboot</button></p>
<pre id=s></pre>
<script>const $=i=>document.getElementById(i);let filled=0;
function g(u){fetch(u).then(()=>setTimeout(s,300))}
function s(){fetch('/status').then(r=>r.json()).then(j=>{$('s').textContent=JSON.stringify(j,null,1);$('v').textContent=j.version;if(!filled){filled=1;$('rf').value=j.refresh_s;$('mx').value=j.max_aircraft}})}
fetch('/settings').then(r=>r.json()).then(a=>{for(const o of a){const l=document.createElement('label'),c=document.createElement('input');c.type='checkbox';c.checked=o.on;
c.onchange=()=>fetch('/toggle?name='+o.name+'&on='+(c.checked?1:0));l.append(c,' '+o.label);$('t').append(l)}});
s();setInterval(s,3000)</script>)HTML";
static void hUi() { touch(); server.send_P(200, "text/html", UI); }

void setup() {
  running_p = esp_ota_get_running_partition();
  other_p = esp_ota_get_next_update_partition(NULL);
  if (other_p && other_p->address == running_p->address) other_p = nullptr;
  if (!confirmed() && otherHasApp()) arm_err = esp_ota_set_boot_partition(other_p);   // any reset from here goes back to the other slot

  radar_mu = xSemaphoreCreateMutex();
  pinMode(PIN_BUZZ, OUTPUT); digitalWrite(PIN_BUZZ, LOW);                              // keep the buzzer quiet, as stock does
  pinMode(PIN_BTN, INPUT_PULLUP);

  WiFi.mode(WIFI_STA);
  WiFi.setHostname("radarmash");
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  panelInit();
  snprintf(boot_note, sizeof boot_note, "joining Wi-Fi");
  drawFrame(true);
  ledcAttach(PIN_BL, 20000, 10); setBacklight(brightness);   // the light is off until driven: nothing else turns it on after a cold start

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 45000) drawFrame(true);
  if (WiFi.status() != WL_CONNECTED) {
    snprintf(boot_note, sizeof boot_note, "Wi-Fi failed");
    drawFrame(true); delay(3000);
    ESP.restart();                                           // unconfirmed: back to the other slot. Confirmed: try again.
  }
  boot_note[0] = 0;

  server.on("/", hUi);
  server.on("/status", hStatus);
  server.on("/settings", hSettings);
  server.on("/toggle", hToggle);
  server.on("/press", hPress);
  server.on("/hold", hHold);
  server.on("/bl", hBl);
  server.on("/home", hHome);
  server.on("/demo", hDemo);
  server.on("/feed", hFeed);
  server.on("/psramtest", hPsramTest);
  server.on("/stay", hStay);
  server.on("/stock", hStock);
  server.on("/reboot", hReboot);
  server.on("/ota", HTTP_POST, hOtaDone, hOtaRaw);
  server.on("/ota/dry", HTTP_POST, hOtaDone, hOtaDryRaw);
  server.begin();
  xTaskCreatePinnedToCore(feedTask, "feed", 12288, nullptr, 1, nullptr, 0);
  last_activity_ms = millis();
}

void loop() {
  static uint32_t btn_change_ms = 0, btn_down_ms = 0;
  static bool btn_raw_last = false, btn_down = false, long_done = false;
  server.handleClient();
  uint32_t now = millis();

  bool raw = digitalRead(PIN_BTN) == LOW;                    // active low, 30 ms debounce
  if (raw != btn_raw_last) { btn_raw_last = raw; btn_change_ms = now; }
  if (raw != btn_down && now - btn_change_ms > 30) {
    btn_down = raw; last_activity_ms = now;
    if (btn_down) { btn_down_ms = now; long_done = false; }
    else if (!long_done) { Lock lock; radar.shortPress(); }
  }
  if (btn_down && !long_done && now - btn_down_ms > 700) { long_done = true; Lock lock; radar.longPress(); }
  if (btn_down && now - btn_down_ms > 8000) { snprintf(boot_note, sizeof boot_note, "back to other firmware"); drawFrame(!live_mode); goHome(); }

  drawFrame(!live_mode);

  if (!confirmed() && now - last_activity_ms > 30UL * 60UL * 1000UL) ESP.restart();   // unconfirmed and left alone: go back
  if (WiFi.status() == WL_CONNECTED) wifi_lost_ms = 0;
  else { if (!wifi_lost_ms) wifi_lost_ms = now | 1; if (now - wifi_lost_ms > 10UL * 60UL * 1000UL) ESP.restart(); }
}
