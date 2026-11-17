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
//  - Wi-Fi: it joins the network the device already knows. If it cannot, it puts up its own setup hotspot (and keeps
//    retrying the known network). Requests arriving over that open hotspot can only reach the Wi-Fi setup page, never
//    the settings or the updater.
//  - The map and the fast drawing path live in the external memory (PSRAM). If that is ever missing the radar still runs,
//    more slowly and without a map.
#include <Arduino.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <PNGdec.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <driver/spi_master.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_psram.h>
#include <esp_task_wdt.h>
#include <esp_mac.h>
#include <esp_wifi.h>
#include <mbedtls/sha256.h>

#include "admin_html.h"
#include "config.h"
#include "feed.h"
#include "panel_init.h"
#include "radar.h"
#include "wifi_html.h"
#if __has_include("secrets.h")
#include "secrets.h"                                 // optional: Wi-Fi details for a development build
#endif

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
static uint32_t last_activity_ms = 0;
static void touch();                                // marks deliberate use of the device (defined with the web handlers)
static uint32_t frame_ms = 0, frames = 0;
static int brightness = 1023;
static char boot_note[40] = "", note2[34] = "", note3[34] = "";   // up to three lines of message over the radar

// True only if the external memory chip really answered and is usable as heap (the Arduino flag alone is not proof).
static bool psramOk() { return esp_psram_is_initialized() && heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0; }

// ---- panel ----
// Drawing and sending run side by side on the two processor cores: the main loop draws a strip into one of two buffers
// and hands it over; the sender task works out what changed, turns it for the panel and clocks it out over SPI while the
// loop is already drawing the next strip.
// Word-aligned: the checksum reads the strips a word at a time, and the SPI driver's DMA wants an aligned buffer.
static uint16_t strips[2][SCREEN_W * BAND_ROWS] __attribute__((aligned(4)));   // upright strips of the picture
static uint16_t turned[SCREEN_W * BAND_ROWS] __attribute__((aligned(4)));      // a block as the panel wants it (sender task only)
struct StripJob { uint8_t buf; int16_t y0; bool last; };
static QueueHandle_t free_q, work_q;

// The panel is driven through the ESP-IDF SPI driver with DMA, which clocks a whole block out at the full 80 MHz. (The
// Arduino SPI class feeds the hardware 64 bytes at a time and manages a little over half that.)
static spi_device_handle_t panel = nullptr;
static esp_err_t panel_err = ESP_OK;

static void spiOut(const uint8_t *data, size_t n) {
  if (!panel || !n) return;
  spi_transaction_t t = {};
  t.length = n * 8;
  if (n <= 4) { t.flags = SPI_TRANS_USE_TXDATA; memcpy(t.tx_data, data, n); }
  else t.tx_buffer = data;                                  // must be DMA-capable memory: the static buffers are
  esp_err_t e = spi_device_polling_transmit(panel, &t);
  if (e != ESP_OK) panel_err = e;
}

static void panelCmd(uint8_t cmd, const uint8_t *data, size_t n) {
  digitalWrite(PIN_DC, LOW); spiOut(&cmd, 1);
  digitalWrite(PIN_DC, HIGH);
  if (n) spiOut(data, n);
}

static void panelInit() {
  pinMode(PIN_DC, OUTPUT); digitalWrite(PIN_DC, HIGH);
  pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, HIGH); delay(5);
  digitalWrite(PIN_RST, LOW); delay(10); digitalWrite(PIN_RST, HIGH); delay(120);
  if (!panel) {
    spi_bus_config_t bus = {};
    bus.mosi_io_num = PIN_MOSI; bus.miso_io_num = -1; bus.sclk_io_num = PIN_SCLK; bus.quadwp_io_num = -1; bus.quadhd_io_num = -1;
    bus.max_transfer_sz = SCREEN_W * BAND_ROWS * 2;
    panel_err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    spi_device_interface_config_t dev = {};
    dev.clock_speed_hz = SPI_HZ; dev.mode = 0; dev.spics_io_num = -1; dev.queue_size = 1; dev.flags = SPI_DEVICE_NO_DUMMY;
    if (panel_err == ESP_OK) panel_err = spi_bus_add_device(SPI2_HOST, &dev, &panel);
    if (panel_err == ESP_OK) panel_err = spi_device_acquire_bus(panel, portMAX_DELAY);   // the only device: keep the bus
  }
  const uint8_t *p = PANEL_INIT;
  for (uint16_t i = 0; i < PANEL_INIT_COUNT; i++) {
    panelCmd(p[0], p + 3, p[2]);
    if (p[1]) delay(p[1]);
    p += 3 + p[2];
  }
}

// The panel is mounted a quarter turn round: upright (x, y) is panel column 359 - y, panel row x. So an upright block of
// rows and columns becomes a block of panel columns and rows, sent row by row, most-significant byte first.
static void pushBlock(const uint16_t *band, int y0, int x0, int x1) {   // columns x0..x1 of the strip starting at row y0
  int n = 0;
  for (int x = x0; x <= x1; x++)
    for (int y = BAND_ROWS - 1; y >= 0; y--) turned[n++] = __builtin_bswap16(band[y * SCREEN_W + x]);
  int c0 = SCREEN_H - BAND_ROWS - y0, c1 = SCREEN_H - 1 - y0;
  uint8_t cols[4] = {(uint8_t)(c0 >> 8), (uint8_t)c0, (uint8_t)(c1 >> 8), (uint8_t)c1};
  uint8_t rows[4] = {(uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1};
  panelCmd(0x2A, cols, 4); panelCmd(0x2B, rows, 4); panelCmd(0x2C, nullptr, 0);
  spiOut((const uint8_t *)turned, (size_t)n * 2);
}

// Only the parts of the screen that changed are sent: most of it is the same from one frame to the next, and sending
// all 259 KB every time is what would limit the frame rate. The screen is divided into cells and a checksum of what each
// one last showed is remembered; a cell is sent when its checksum changes. (Comparing against a full copy of the last
// frame also works, but reading that copy back from PSRAM cost more than the sending it saved.)
static const int CELL = 20;                               // cells are CELL wide and one strip tall
static uint32_t cell_sum[SCREEN_H / BAND_ROWS][SCREEN_W / CELL];
static bool send_all = true;
static uint32_t perf_draw_us = 0, perf_wait_us = 0, perf_diff_us = 0, perf_send_us = 0, perf_sent_px = 0, perf_frames = 0;

static void pushBand(const uint16_t *band, int y0) {
  uint32_t t = micros();
  uint32_t *sums = cell_sum[y0 / BAND_ROWS];
  bool dirty[SCREEN_W / CELL];
  for (int cx = 0; cx < SCREEN_W / CELL; cx++) {
    uint32_t h = 2166136261u;
    for (int y = 0; y < BAND_ROWS; y++) {
      const uint32_t *w = (const uint32_t *)(band + y * SCREEN_W + cx * CELL);   // CELL pixels are CELL / 2 words
      for (int i = 0; i < CELL / 2; i++) h = (h ^ w[i]) * 0x9E3779B1u;
    }
    dirty[cx] = send_all || h != sums[cx];
    sums[cx] = h;
  }
  perf_diff_us += micros() - t;
  t = micros();
  for (int cx = 0; cx < SCREEN_W / CELL;) {                // send each run of changed cells as one block
    if (!dirty[cx]) { cx++; continue; }
    int end = cx;
    while (end + 1 < SCREEN_W / CELL && dirty[end + 1]) end++;
    pushBlock(band, y0, cx * CELL, (end + 1) * CELL - 1);
    perf_sent_px += (end - cx + 1) * CELL * BAND_ROWS;
    cx = end + 1;
  }
  perf_send_us += micros() - t;
}

static void senderTask(void *) {
  StripJob job;
  for (;;) {
    xQueueReceive(work_q, &job, portMAX_DELAY);
    pushBand(strips[job.buf], job.y0);
    xQueueSend(free_q, &job.buf, portMAX_DELAY);
    if (job.last) {
      send_all = false;                                    // the whole screen has now been sent at least once
      // The loop feeds this task as fast as it can take it, so without a pause it would never stop running, the idle
      // task on this core would starve, and the watchdog would reset the device. Give way briefly once a frame.
      vTaskDelay(pdMS_TO_TICKS(2));
    }
  }
}

static void setBacklight(int v) {                  // 0..1023. The pin is active low.
  brightness = constrain(v, 0, 1023);
  ledcWrite(PIN_BL, 1023 - brightness);
}

static void drawFrame(bool demo) {
  static uint32_t last_start = 0;
  uint32_t t0 = millis();
  frame_ms = t0 - last_start; last_start = t0;             // time from one frame to the next, sending included
  Lock lock;
  if (demo) demoTraffic(radar, t0);
  radar.tick(t0);
  radar.prepare();
  for (int y0 = 0; y0 < SCREEN_H; y0 += BAND_ROWS) {
    uint8_t buf;
    uint32_t w = micros();
    xQueueReceive(free_q, &buf, portMAX_DELAY);            // waits here only if the sender is behind
    perf_wait_us += micros() - w;
    uint32_t t = micros();
    Canvas c(strips[buf], y0, BAND_ROWS);
    radar.draw(c);
    if (boot_note[0]) {
      int lines = 1 + (note2[0] ? 1 : 0) + (note3[0] ? 1 : 0), top = 300 - (lines - 1) * 22;
      c.blendRect(0, top - 8, SCREEN_W, lines * 22 + 12, rgb(0, 14, 8), 225);
      c.textCentred(180, top, boot_note, 2, rgb(255, 240, 80));
      if (note2[0]) c.textCentred(180, top + 22, note2, 2, rgb(230, 230, 230));
      if (note3[0]) c.textCentred(180, top + 44, note3, 2, rgb(230, 230, 230));
    }
    perf_draw_us += micros() - t;
    StripJob job{buf, (int16_t)y0, y0 + BAND_ROWS >= SCREEN_H};
    xQueueSend(work_q, &job, portMAX_DELAY);
  }
  frames++; perf_frames++;
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
static String feed_url = FEED_ADSB_FI;              // swapped between the two built-in feeds when one stops answering
static bool include_ground = false;
static Aircraft *fresh = nullptr;                   // the feed task's working list (in PSRAM when there is some)

struct RouteEntry { char callsign[9]; RouteInfo info; bool known; uint32_t when_ms; };
static RouteEntry route_cache[48];
static int route_next = 0;
static RouteEntry *findRoute(const char *cs) {
  for (RouteEntry &e : route_cache) if (e.callsign[0] && strcmp(e.callsign, cs) == 0) return &e;
  return nullptr;
}

// Secure connections are checked against the built-in list of certificate authorities (the same list browsers trust),
// so a feed or map server is who it says it is. Plain http:// addresses, for a server on the home network, are allowed.
static bool verify_certs = true;
static WiFiClient plain_client;
static void secure(WiFiClientSecure &c) { if (verify_certs) c.useBuiltinCACertBundle(); else c.setInsecure(); }

static bool httpsGet(const String &url, String &body) {
  WiFiClientSecure client;
  secure(client);
  HTTPClient http;
  http.setUserAgent(USER_AGENT);
  http.setConnectTimeout(8000); http.setTimeout(8000);
  if (!(url.startsWith("https://") ? http.begin(client, url) : http.begin(plain_client, url))) return false;
  int code = http.GET();
  bool ok = code == 200 || code == 404;             // 404 is the route service's "unknown callsign"
  if (ok) body = http.getString();
  http.end();
  return ok && body.length() > 0;
}

static void pollFeed() {
  double lat, lon; int nm, limit;
  { Lock lock; lat = radar.home_lat; lon = radar.home_lon; nm = feedRadiusNm(radar.range_km); limit = constrain((int)radar.set.max_aircraft, 1, MAX_AIRCRAFT); }
  String url, url_before;
  { Lock lock; url = url_before = feed_url; }
  url.replace("{lat}", String(lat, 4)); url.replace("{lon}", String(lon, 4)); url.replace("{nm}", String(nm));
  String body;
  int n = httpsGet(url, body) ? parseAircraft(body.c_str(), body.length(), lat, lon, include_ground, fresh, limit) : -1;
  body = String();
  Lock lock;
  if (n < 0) {
    feed_errors++;
    if (++feed_failing >= 2 && feed_url == url_before) {
      if (feed_url == FEED_ADSB_FI) { feed_url = FEED_ADSB_LOL; feed_failing = 0; }
      else if (feed_url == FEED_ADSB_LOL) { feed_url = FEED_ADSB_FI; feed_failing = 0; }
    }
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

struct AirportEntry { char icao[5], iata[5]; double lat, lon; };
static AirportEntry airport_cache[24];
static int airport_next = 0, second_opinions = 0, routes_rejected = 0;

static bool airportInfo(const char *icao, AirportEntry &out) {
  for (AirportEntry &e : airport_cache) if (e.icao[0] && strcmp(e.icao, icao) == 0) { out = e; return true; }
  String url = AIRPORT_HEXDB, body;
  url.replace("{icao}", icao);
  AirportEntry e = {};
  if (!httpsGet(url, body) || !parseHexdbAirport(body.c_str(), body.length(), e.iata, e.lat, e.lon)) return false;
  snprintf(e.icao, sizeof e.icao, "%s", icao);
  airport_cache[airport_next] = e; airport_next = (airport_next + 1) % (int)(sizeof airport_cache / sizeof airport_cache[0]);
  out = e;
  return true;
}

// One callsign per cycle. The first service's answer is used if it fits where the aircraft is; if it has none, or one
// that does not fit, a second service is asked and its answer held to the same test. A route that fails is not shown:
// no route is better than a wrong one.
static void lookupOneRoute() {
  char cs[9] = "";
  double lat = 0, lon = 0; float track = 0; int climb = 0;
  {
    Lock lock;
    for (int i = 0; i < radar.count; i++) {
      float x, y;
      const Aircraft &a = radar.ac[i];
      if (!lookupableCallsign(a.callsign) || !radar.project(a, x, y)) continue;
      RouteEntry *e = findRoute(a.callsign);
      if (e && (e->known || millis() - e->when_ms < 30UL * 60UL * 1000UL)) continue;
      memcpy(cs, a.callsign, sizeof cs); lat = a.lat; lon = a.lon; track = a.track_deg; climb = a.climb_fpm;
      break;
    }
  }
  if (!cs[0]) return;
  String url = ROUTE_ADSBDB, body;
  url.replace("{callsign}", cs);
  RouteInfo info;
  RouteResult res = httpsGet(url, body) ? parseRoute(body.c_str(), body.length(), info) : RouteResult::Bad;
  body = String();
  if (res == RouteResult::Bad) { Lock lock; lookups++; return; }           // network trouble: ask again next cycle

  bool rejected = false, asked_second = false, retry = false;
  if (res != RouteResult::Found || routeFit(info, lat, lon, track, climb) == RouteFit::DoesNotFit) {
    rejected = res == RouteResult::Found;
    asked_second = true;
    info.origin[0] = info.dest[0] = 0; info.have_pos = false;              // keep the airline name, drop the route
    String url2 = ROUTE_HEXDB, body2;
    url2.replace("{callsign}", cs);
    char o[5], d[5];
    if (!httpsGet(url2, body2)) retry = true;
    else if (parseHexdbRoute(body2.c_str(), body2.length(), o, d)) {
      AirportEntry ao, ad;
      if (airportInfo(o, ao) && airportInfo(d, ad)) {
        RouteInfo second = info;
        snprintf(second.origin, sizeof second.origin, "%s", ao.iata); snprintf(second.dest, sizeof second.dest, "%s", ad.iata);
        second.o_lat = ao.lat; second.o_lon = ao.lon; second.d_lat = ad.lat; second.d_lon = ad.lon; second.have_pos = true;
        if (routeFit(second, lat, lon, track, climb) == RouteFit::Fits) info = second;
      }
    }
  }
  Lock lock;
  lookups++; second_opinions += asked_second; routes_rejected += rejected;
  if (retry) return;
  RouteEntry *e = findRoute(cs);
  if (!e) { e = &route_cache[route_next]; route_next = (route_next + 1) % (int)(sizeof route_cache / sizeof route_cache[0]); }
  memcpy(e->callsign, cs, sizeof e->callsign); e->info = info; e->known = info.airline[0] || info.origin[0]; e->when_ms = millis();
  if (!e->known) return;
  for (int i = 0; i < radar.count; i++) {
    Aircraft &a = radar.ac[i];
    if (strcmp(a.callsign, cs) != 0) continue;
    memcpy(a.airline, info.airline, sizeof a.airline); memcpy(a.origin, info.origin, sizeof a.origin); memcpy(a.dest, info.dest, sizeof a.dest);
  }
}

// ---- map underlay (needs PSRAM) ----
// Tiles are fetched over HTTPS as PNG, decoded one at a time, and composed into a picture the radar draws over. Raw
// tiles are kept in memory while the device is up, so changing range and coming back does not download them again.
static String map_url = "https://tile.openstreetmap.org/{z}/{x}/{y}.png";
static char map_credit[28] = "(c) OpenStreetMap";
static MapImage *map_front = nullptr, *map_back = nullptr;   // front is on screen; back is being built
static uint16_t *tile_px = nullptr;                          // one decoded tile
static PNG *png = nullptr;
static int map_tiles_fetched = 0, map_tiles_failed = 0, map_decode_failed = 0, map_missing = -1;
static uint32_t map_built_ms = 0;
static String map_built_for;

struct RawTile { int z, x, y; uint8_t *data; int len; };
static RawTile raw_tiles[64];
static int raw_next = 0;

static int pngLine(PNGDRAW *d) {
  if (d->y >= 0 && d->y < TILE_SIZE) png->getLineAsRGB565(d, tile_px + d->y * TILE_SIZE, PNG_RGB565_LITTLE_ENDIAN, 0xffffffff);
  return 1;
}

static bool fetchBytes(const String &url, uint8_t *&data, int &len) {
  WiFiClientSecure client;
  secure(client);
  HTTPClient http;
  http.setUserAgent(USER_AGENT);
  http.setConnectTimeout(8000); http.setTimeout(8000);
  if (!(url.startsWith("https://") ? http.begin(client, url) : http.begin(plain_client, url))) return false;
  bool ok = false;
  if (http.GET() == 200) {
    len = http.getSize();
    if (len > 0 && len <= 300000 && (data = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM)) != nullptr) {
      // Read until it is all here. A tile arrives in bursts, so a single read with the stream's short timeout gives up early.
      Stream &in = http.getStream();
      int got = 0;
      uint32_t deadline = millis() + 15000;
      while (got < len && millis() < deadline) {
        int avail = in.available();
        if (avail > 0) got += (int)in.readBytes(data + got, min(avail, len - got));
        else if (!http.connected()) break;
        else delay(2);
      }
      ok = got == len;
      if (!ok) { heap_caps_free(data); data = nullptr; }
    }
  }
  http.end();
  return ok;
}

struct DeviceTiles : TileSource {
  const uint16_t *tile(int z, int x, int y) override {
    RawTile *t = nullptr;
    for (RawTile &r : raw_tiles) if (r.data && r.z == z && r.x == x && r.y == y) { t = &r; break; }
    if (!t) {
      String url = map_url;
      url.replace("{z}", String(z)); url.replace("{x}", String(x)); url.replace("{y}", String(y)); url.replace("{s}", "a");
      uint8_t *data = nullptr; int len = 0;
      if (!fetchBytes(url, data, len)) { map_tiles_failed++; return nullptr; }
      map_tiles_fetched++;
      t = &raw_tiles[raw_next]; raw_next = (raw_next + 1) % (int)(sizeof raw_tiles / sizeof raw_tiles[0]);
      if (t->data) heap_caps_free(t->data);
      *t = RawTile{z, x, y, data, len};
    }
    bool ok = png->openRAM(t->data, t->len, pngLine) == PNG_SUCCESS;
    if (ok) {
      ok = png->getWidth() == TILE_SIZE && png->getHeight() == TILE_SIZE && png->decode(nullptr, 0) == PNG_SUCCESS;
      png->close();
    }
    if (!ok) { map_decode_failed++; heap_caps_free(t->data); t->data = nullptr; }   // do not keep a tile we cannot read
    return ok ? tile_px : nullptr;
  }
};
static DeviceTiles device_tiles;

static bool mapSetup() {
  if (map_front) return true;
  if (!psramOk()) return false;
  void *a = heap_caps_malloc(sizeof(MapImage), MALLOC_CAP_SPIRAM), *b = heap_caps_malloc(sizeof(MapImage), MALLOC_CAP_SPIRAM);
  void *p = heap_caps_malloc(sizeof(PNG), MALLOC_CAP_SPIRAM);
  tile_px = (uint16_t *)heap_caps_malloc(TILE_SIZE * TILE_SIZE * 2, MALLOC_CAP_SPIRAM);
  if (!a || !b || !p || !tile_px) return false;
  map_front = new (a) MapImage(); map_back = new (b) MapImage(); png = new (p) PNG();
  return true;
}

// Rebuild the map when home, range, style or source changes, and retry now and then while tiles are missing.
static void updateMap() {
  double lat, lon; float range; MapStyle style; bool want; int detail;
  { Lock lock; lat = radar.home_lat; lon = radar.home_lon; range = radar.range_km; style = radar.set.map_style; want = radar.set.map; detail = radar.set.map_detail; }
  if (!want || !mapSetup()) return;
  String key = String(lat, 4) + "," + String(lon, 4) + "," + String(range, 0) + "," + String((int)style) + "," + String(detail) + "," + map_url;
  if (key == map_built_for && (map_missing == 0 || millis() - map_built_ms < 60000)) return;   // be gentle on the tile server
  int missing = composeMap(*map_back, device_tiles, lat, lon, range, style, detail);
  Lock lock;
  MapImage *t = map_front; map_front = map_back; map_back = t;
  map_back->version = map_front->version;           // keep the two counters in step so every rebuild looks new
  radar.map = map_front;
  snprintf(radar.map_credit, sizeof radar.map_credit, "%s", map_credit);
  map_missing = missing; map_built_ms = millis(); map_built_for = key;
}

// ---- airline logos (needs PSRAM) ----
// None are built in. If the settings give an address pattern, each airline's logo is fetched the first time one of its
// aircraft is highlighted, shrunk to size and kept in memory until the device restarts.
static String logo_url;
static int logos_loaded = 0, logos_missing = 0;

struct DeviceLogos : LogoSource {
  enum State : uint8_t { EMPTY, WANTED, LOADED, MISSING };
  struct Entry { char icao[4]; State state; Logo *logo; };
  Entry entries[24] = {};
  int next = 0;

  const Logo *logo(const char *icao) override {            // called while drawing, with the radar lock held
    if (!logo_url.length() || !psramOk()) return nullptr;
    for (Entry &e : entries)
      if (e.state != EMPTY && strcmp(e.icao, icao) == 0) return e.state == LOADED ? e.logo : nullptr;
    Entry &e = entries[next]; next = (next + 1) % (int)(sizeof entries / sizeof entries[0]);   // reuse the oldest slot
    snprintf(e.icao, sizeof e.icao, "%s", icao);
    e.state = WANTED;                                      // its picture, if it had one, is reused when the new one loads
    return nullptr;
  }
  void forgetAll() { for (Entry &e : entries) e.state = EMPTY; }
};
static DeviceLogos device_logos;

struct LogoJob { uint8_t *rgba; int w, h; bool ok; };

// One decoded line of a PNG, whatever kind it is, as red, green, blue, alpha.
static int logoLine(PNGDRAW *d) {
  LogoJob *j = (LogoJob *)d->pUser;
  if (d->y < 0 || d->y >= j->h || d->iWidth > j->w) return 1;
  uint8_t *out = j->rgba + (size_t)d->y * j->w * 4;
  const uint8_t *s = d->pPixels;
  switch (d->iPixelType) {
    case PNG_PIXEL_INDEXED:                                // 1, 2, 4 or 8 bits a pixel, most significant first
      for (int x = 0; x < d->iWidth; x++) {
        int idx;
        if (d->iBpp == 8) idx = s[x];
        else { int per = 8 / d->iBpp, shift = 8 - d->iBpp * (x % per + 1); idx = (s[x / per] >> shift) & ((1 << d->iBpp) - 1); }
        const uint8_t *c = d->pPalette + idx * 3;
        out[0] = c[0]; out[1] = c[1]; out[2] = c[2]; out[3] = d->iHasAlpha ? d->pPalette[768 + idx] : 255;
        out += 4;
      }
      break;
    case PNG_PIXEL_TRUECOLOR_ALPHA:
      if (d->iBpp != 8) { j->ok = false; return 0; }
      memcpy(out, s, (size_t)d->iWidth * 4);
      break;
    case PNG_PIXEL_TRUECOLOR:
      if (d->iBpp != 8) { j->ok = false; return 0; }
      for (int x = 0; x < d->iWidth; x++, out += 4, s += 3) { out[0] = s[0]; out[1] = s[1]; out[2] = s[2]; out[3] = 255; }
      break;
    case PNG_PIXEL_GRAY_ALPHA:
      if (d->iBpp != 8) { j->ok = false; return 0; }
      for (int x = 0; x < d->iWidth; x++, out += 4, s += 2) { out[0] = out[1] = out[2] = s[0]; out[3] = s[1]; }
      break;
    case PNG_PIXEL_GRAYSCALE:
      if (d->iBpp != 8) { j->ok = false; return 0; }
      for (int x = 0; x < d->iWidth; x++, out += 4) { out[0] = out[1] = out[2] = s[x]; out[3] = 255; }
      break;
    default: j->ok = false; return 0;
  }
  return 1;
}

static void fetchOneLogo() {
  char icao[4] = "";
  String tmpl;
  { Lock lock;
    tmpl = logo_url;
    for (DeviceLogos::Entry &e : device_logos.entries) if (e.state == DeviceLogos::WANTED) { memcpy(icao, e.icao, sizeof icao); break; } }
  if (!icao[0] || !tmpl.length() || !mapSetup()) return;   // mapSetup() also makes the PNG decoder
  String url = tmpl;
  url.replace("{icao}", icao);
  uint8_t *data = nullptr; int len = 0;
  Logo *made = nullptr;
  if (fetchBytes(url, data, len)) {
    if (png->openRAM(data, len, logoLine) == PNG_SUCCESS) {
      LogoJob job{nullptr, png->getWidth(), png->getHeight(), true};
      if (job.w >= 1 && job.h >= 1 && job.w <= 320 && job.h <= 320 && (job.rgba = (uint8_t *)heap_caps_calloc((size_t)job.w * job.h, 4, MALLOC_CAP_SPIRAM)) != nullptr) {
        if (png->decode(&job, 0) == PNG_SUCCESS && job.ok) {
          made = (Logo *)heap_caps_malloc(sizeof(Logo), MALLOC_CAP_SPIRAM);
          if (made && !makeLogo(job.rgba, job.w, job.h, *made)) { heap_caps_free(made); made = nullptr; }
        }
        heap_caps_free(job.rgba);
      }
      png->close();
    }
    heap_caps_free(data);
  }
  Lock lock;
  for (DeviceLogos::Entry &e : device_logos.entries) {
    if (e.state != DeviceLogos::WANTED || strcmp(e.icao, icao) != 0) continue;
    if (e.logo) { heap_caps_free(e.logo); e.logo = nullptr; }
    e.logo = made; made = nullptr;
    e.state = e.logo ? DeviceLogos::LOADED : DeviceLogos::MISSING;   // an airline with no logo there is not asked for again
    if (e.logo) logos_loaded++; else logos_missing++;
  }
  if (made) heap_caps_free(made);
}

static void feedTask(void *) {
  fresh = (Aircraft *)heap_caps_malloc(sizeof(Aircraft) * MAX_AIRCRAFT, psramOk() ? MALLOC_CAP_SPIRAM : MALLOC_CAP_DEFAULT);
  for (int i = 0; fresh && i < MAX_AIRCRAFT; i++) new (&fresh[i]) Aircraft();
  for (;;) {
    uint32_t began = millis();
    if (WiFi.status() == WL_CONNECTED) { updateMap(); fetchOneLogo(); }
    if (fresh && live_mode && WiFi.status() == WL_CONNECTED) { pollFeed(); lookupOneRoute(); }
    uint32_t every = (uint32_t)constrain(refresh_s, 2, 300) * 1000UL, took = millis() - began;
    vTaskDelay(pdMS_TO_TICKS(took < every ? every - took : 200));
  }
}

// ---- Wi-Fi ----
static DNSServer dns;
static bool portal = false, mdns_up = false;
static char ap_name[24] = "RadarMASH-setup";
static uint32_t wifi_ok_since = 0, wifi_lost_since = 0, wifi_retry_ms = 0, portal_hold_until = 0;
static int scan_wait = 0;
static struct {
  enum State { IDLE, JOINING, JOINED, FAILED } state = IDLE;
  char ssid[33] = "", kept[33] = "";
  uint32_t started = 0;
  wifi_config_t old, wanted;                        // what to go back to if the new details do not work, and the new details
  bool had_old = false;
} join_req;

// New Wi-Fi details are tried from memory only. They are written to the device's store only once they have worked, so
// a wrong password or a mistyped name can never replace details that do work. (An earlier version stored first and
// tried to put the old ones back on failure; the put-back was refused while the radio was busy, and the radar was left
// knowing only the bad details.)
static void tryWifi(const wifi_config_t &c, bool remember) {
  esp_wifi_disconnect();
  esp_wifi_set_storage(remember ? WIFI_STORAGE_FLASH : WIFI_STORAGE_RAM);
  wifi_config_t copy = c;
  esp_wifi_set_config(WIFI_IF_STA, &copy);
  esp_wifi_connect();
}

static bool savedWifi(char *ssid_out) {             // does the device already know a network?
  wifi_config_t wc = {};
  if (esp_wifi_get_config(WIFI_IF_STA, &wc) != ESP_OK || !wc.sta.ssid[0]) return false;
  if (ssid_out) { memcpy(ssid_out, wc.sta.ssid, 32); ssid_out[32] = 0; }
  return true;
}

static void startPortal() {
  if (portal) return;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(ap_name);                             // open, so it can be joined from any phone; see viaSetupAp()
  delay(100);
  dns.start(53, "*", WiFi.softAPIP());              // every name resolves here, which makes phones pop the page up
  portal = true;
  snprintf(boot_note, sizeof boot_note, "Wi-Fi setup");
  snprintf(note2, sizeof note2, "join %s", ap_name);
  snprintf(note3, sizeof note3, "then open 192.168.4.1");
}
static void stopPortal() {
  if (!portal) return;
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  portal = false;
  boot_note[0] = note2[0] = note3[0] = 0;
}
// True for a request that came in over the setup hotspot rather than the home network.
static bool viaSetupAp() { return portal && server.client().localIP() == WiFi.softAPIP(); }

static void wifiTick(uint32_t now) {
  bool up = WiFi.status() == WL_CONNECTED;
  if (portal) dns.processNextRequest();
  if (up) {
    if (!wifi_ok_since) wifi_ok_since = now | 1;
    wifi_lost_since = 0;
    if (!mdns_up) {
      mdns_up = MDNS.begin("radarmash");                                   // reachable as radarmash.local
      configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");             // certificates have dates, so the clock must be right
    }
  } else {
    if (!wifi_lost_since) wifi_lost_since = now | 1;
    wifi_ok_since = 0;
  }
  if (join_req.state == join_req.JOINING) {
    if (up) {
      join_req.state = join_req.JOINED;
      esp_wifi_set_storage(WIFI_STORAGE_FLASH);                            // it works: now remember it
      esp_wifi_set_config(WIFI_IF_STA, &join_req.wanted);
      memset(&join_req.wanted, 0, sizeof join_req.wanted);
    } else if (now - join_req.started > 20000) {
      join_req.state = join_req.FAILED;
      memset(&join_req.wanted, 0, sizeof join_req.wanted);
      if (join_req.had_old) tryWifi(join_req.old, true);                   // back to what worked (still in the store, untouched)
      else { esp_wifi_disconnect(); esp_wifi_set_storage(WIFI_STORAGE_FLASH); }
      wifi_retry_ms = now;
    }
    return;
  }
  if (portal && up && now - wifi_ok_since > 45000 && (int32_t)(now - portal_hold_until) > 0) stopPortal();   // joined; the page has had time to say so
  if (!portal && !up && now - wifi_lost_since > 60000) startPortal();      // the network has gone: offer setup, keep trying
  if (portal && !up && WiFi.softAPgetStationNum() == 0 && now - wifi_retry_ms > 30000) {
    wifi_retry_ms = now;                                                   // nobody is using the hotspot: try the known network
    wifi_config_t known = {};
    esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    if (join_req.had_old) tryWifi(join_req.old, true);
    else if (esp_wifi_get_config(WIFI_IF_STA, &known) == ESP_OK && known.sta.ssid[0]) esp_wifi_connect();
  }
}

// Put the setup hotspot up for five minutes while still on the home network, to try the setup page from a phone.
static void hWifiPortal() {
  touch();
  portal_hold_until = millis() + 5UL * 60UL * 1000UL;
  startPortal();
  server.send(200, "application/json", String("{\"ap\":\"") + ap_name + "\",\"minutes\":5}");
}
static void hWifiPage() { server.send_P(200, "text/html", WIFI_HTML); }
static void hWifiScan() {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_FAILED || (server.hasArg("fresh") && n != WIFI_SCAN_RUNNING)) {
    // The radio will not scan while it is busy trying to join a network, and when the known network is wrong or gone it
    // is always trying. Stop that first; the retry in wifiTick() starts it again later.
    if (WiFi.status() != WL_CONNECTED && join_req.state != join_req.JOINING) { esp_wifi_disconnect(); wifi_retry_ms = millis(); }
    WiFi.scanDelete();
    n = WiFi.scanNetworks(true) == WIFI_SCAN_FAILED ? WIFI_SCAN_FAILED : WIFI_SCAN_RUNNING;
  }
  String j = String("{\"scanning\":") + (n == WIFI_SCAN_RUNNING || n == WIFI_SCAN_FAILED ? "true" : "false") + ",\"networks\":[";   // failed: the page asks again
  int listed = 0;
  for (int i = 0; i < n && listed < 20; i++) {
    String name = WiFi.SSID(i);
    if (!name.length()) continue;                                          // hidden networks have no name to show
    bool stronger = false;
    for (int k = 0; k < n; k++)                                            // one entry per name: the strongest
      if (k != i && WiFi.SSID(k) == name && (WiFi.RSSI(k) > WiFi.RSSI(i) || (WiFi.RSSI(k) == WiFi.RSSI(i) && k < i))) stronger = true;
    if (stronger) continue;
    char esc[80];
    jsonEscape(name.c_str(), esc, sizeof esc);
    j += String(listed++ ? "," : "") + "{\"ssid\":\"" + esc + "\",\"rssi\":" + String(WiFi.RSSI(i)) + ",\"secure\":" + (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "false" : "true") + "}";
  }
  server.send(200, "application/json", j + "]}");
}
static void hWifiJoin() {
  touch();
  String body = server.arg("plain");
  char ssid[33], pass[65];
  if (!parseWifiRequest(body.c_str(), body.length(), ssid, pass)) { server.send(400, "application/json", "{\"error\":\"bad request\"}"); return; }
  join_req.had_old = esp_wifi_get_config(WIFI_IF_STA, &join_req.old) == ESP_OK && join_req.old.sta.ssid[0] != 0;
  join_req.kept[0] = 0;
  if (join_req.had_old) { memcpy(join_req.kept, join_req.old.sta.ssid, 32); join_req.kept[32] = 0; }
  memcpy(join_req.ssid, ssid, sizeof join_req.ssid);
  join_req.started = millis(); join_req.state = join_req.JOINING;
  memset(&join_req.wanted, 0, sizeof join_req.wanted);
  memcpy(join_req.wanted.sta.ssid, ssid, strlen(ssid));
  memcpy(join_req.wanted.sta.password, pass, strlen(pass));
  memset(pass, 0, sizeof pass);
  server.send(200, "application/json", "{\"ok\":true}");
  delay(100);                                                              // let the reply out before the radio moves
  tryWifi(join_req.wanted, false);                                         // from memory only, until it has worked
}
static void hWifiStatus() {
  static const char *names[] = {"idle", "joining", "joined", "failed"};
  char a[80], b[80];
  jsonEscape(join_req.ssid, a, sizeof a); jsonEscape(join_req.kept, b, sizeof b);
  server.send(200, "application/json", String("{\"state\":\"") + names[join_req.state] + "\",\"ssid\":\"" + a + "\",\"ip\":\"" + WiFi.localIP().toString() +
                                         "\",\"kept\":\"" + (join_req.state == join_req.FAILED ? b : "") + "\"}");
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
    if (viaSetupAp()) otaFail("updates are not accepted over the setup hotspot");   // it is open: anyone nearby could be on it
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
    esp_task_wdt_reset();                             // a long upload is one long call from the main loop: it is alive
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

// ---- settings ----
// One Config holds everything the settings page can change. It is saved as JSON in the device's key-value store (NVS),
// under RadarMASH's own name, a few seconds after the last change so a dragged slider is one write, not fifty.
static Config cfg;
static Preferences prefs;
static bool cfg_dirty = false;
static uint32_t cfg_dirty_ms = 0;
static uint32_t fps = 0;

static void applyConfig() {                         // caller holds the lock (or nothing else is running yet)
  bool was_live = live_mode, moved = radar.home_lat != cfg.home_lat || radar.home_lon != cfg.home_lon;
  radar.set = cfg.set;
  radar.home_lat = cfg.home_lat; radar.home_lon = cfg.home_lon;
  radar.range_km = cfg.range_km;
  refresh_s = cfg.refresh_s; include_ground = cfg.include_ground; verify_certs = cfg.verify_certs;
  feed_url = cfg.feed_url;                          // (also undoes any automatic swap to the other feed)
  map_url = cfg.map_url;
  if (logo_url != cfg.logo_url) { logo_url = cfg.logo_url; device_logos.forgetAll(); }   // a different source: start again
  snprintf(map_credit, sizeof map_credit, "%s", cfg.map_credit);
  snprintf(radar.map_credit, sizeof radar.map_credit, "%s", cfg.map_credit);
  live_mode = cfg.wantsLive();
  if (was_live != live_mode || moved) { radar.count = 0; radar.selected = -1; feed_count = -1; feed_ok_ms = 0; }   // a different sky
  brightness = cfg.brightness * 1023 / 100;
}
static void markDirty() { cfg_dirty = true; cfg_dirty_ms = millis(); }

static size_t currentConfigJson(char *buf, size_t n) {   // caller holds the lock
  cfg.set = radar.set; cfg.range_km = radar.range_km;    // the button changes these behind the page's back
  return configToJson(cfg, buf, n);
}
static void saveConfigIfDue() {
  if (!cfg_dirty || millis() - cfg_dirty_ms < 3000) return;
  char buf[CONFIG_JSON_MAX];
  size_t n;
  { Lock lock; n = currentConfigJson(buf, sizeof buf); }
  if (n) prefs.putString("config", buf);
  cfg_dirty = false;
}
static void loadConfig() {
  prefs.begin("radarmash", false);
  String saved = prefs.getString("config", "");
  if (saved.length()) configFromJson(saved.c_str(), saved.length(), cfg);
  applyConfig();
}

// ---- web ----
static void touch() { last_activity_ms = millis(); }
static void ok() { server.send(200, "text/plain", "ok"); }

// Timings averaged since the last call: where a frame's time goes, and how much of the screen is being sent.
static void hPerf() {
  uint32_t n = perf_frames ? perf_frames : 1;
  String j = String("{\"frames\":") + String(perf_frames) + ",\"draw_us\":" + String(perf_draw_us / n) + ",\"wait_us\":" + String(perf_wait_us / n) + ",\"diff_us\":" + String(perf_diff_us / n) +
             ",\"send_us\":" + String(perf_send_us / n) + ",\"sent_percent\":" + String(perf_sent_px / n * 100 / (SCREEN_W * SCREEN_H)) + ",\"frame_ms\":" + String(frame_ms) + "}";
  perf_draw_us = perf_wait_us = perf_diff_us = perf_send_us = perf_sent_px = perf_frames = 0;
  server.send(200, "application/json", j);
}

// Reading the status does not count as someone using the device: a page left open polling it must not hold off the
// return to the other firmware. The first few fields are what the settings page shows; the rest is for diagnosis.
static void hStatus() {
  const esp_partition_t *b = esp_ota_get_boot_partition();
  char net[33] = "", net_esc[80];
  bool wifi_saved = savedWifi(net);
  jsonEscape(net, net_esc, sizeof net_esc);
  Lock lock;
  String j = "{\"device\":true,\"version\":\"" FW_VERSION "\"";
  j += ",\"confirmed\":" + String(confirmed() ? "true" : "false");
  j += ",\"live\":" + String(live_mode ? "true" : "false") + ",\"aircraft\":" + String(radar.count);
  j += ",\"feed_age_s\":" + String(feed_ok_ms ? (long)((millis() - feed_ok_ms) / 1000) : -1L) + ",\"feed_error\":\"\"";
  j += ",\"map_missing\":" + String(map_missing < 0 ? 0 : map_missing) + ",\"range_km\":" + String((int)radar.range_km) + ",\"fps\":" + String(fps);
  j += ",\"running\":\"" + String(running_p->label) + "\",\"running_state\":" + String(runningState());
  j += ",\"boot\":\"" + String(b ? b->label : "?") + "\",\"other\":\"" + String(other_p ? other_p->label : "none") + "\",\"arm_err\":" + String((int)arm_err);
  j += ",\"frame_ms\":" + String(frame_ms) + ",\"frames\":" + String(frames) + ",\"brightness\":" + String(brightness);
  j += ",\"feed_count\":" + String(feed_count) + ",\"feed_errors\":" + String(feed_errors) + ",\"lookups\":" + String(lookups) + ",\"second_opinions\":" + String(second_opinions) + ",\"routes_rejected\":" + String(routes_rejected);
  j += ",\"home_set\":" + String(cfg.home_set ? "true" : "false") + ",\"wifi_saved\":" + String(wifi_saved ? "true" : "false");
  j += ",\"wifi\":\"" + String(WiFi.status() == WL_CONNECTED ? net_esc : "") + "\",\"portal\":" + String(portal ? "true" : "false") + ",\"ap\":\"" + String(ap_name) + "\"";
  j += ",\"map\":" + String(radar.map ? "true" : "false") + ",\"map_zoom\":" + String(map_front ? map_front->zoom : -1);
  j += ",\"map_tiles\":" + String(map_tiles_fetched) + ",\"map_tile_failures\":" + String(map_tiles_failed) + ",\"map_decode_failures\":" + String(map_decode_failed);
  j += ",\"time_set\":" + String(time(nullptr) > 1700000000 ? "true" : "false") + ",\"verify_certs\":" + String(verify_certs ? "true" : "false");
  j += ",\"logos\":" + String(logos_loaded) + ",\"logos_missing\":" + String(logos_missing);
  j += ",\"panel_err\":" + String((int)panel_err) + ",\"cache\":" + String(radar.cache ? "true" : "false");
  j += ",\"psram\":" + String(psramOk() ? "true" : "false") + ",\"psram_free\":" + String((unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  j += ",\"reset_reason\":" + String((int)esp_reset_reason()) + ",\"uptime_ms\":" + String(millis());
  j += ",\"heap\":" + String(ESP.getFreeHeap()) + ",\"heap_min\":" + String(ESP.getMinFreeHeap()) + ",\"rssi\":" + String(WiFi.RSSI()) + "}";
  server.send(200, "application/json", j);
}

static void hSettingsGet() {
  char buf[CONFIG_JSON_MAX];
  { Lock lock; currentConfigJson(buf, sizeof buf); }
  server.send(200, "application/json", buf);
}
static void hSettingsPost() {
  touch();
  String body = server.arg("plain");
  char buf[CONFIG_JSON_MAX];
  bool good;
  {
    Lock lock;
    cfg.set = radar.set; cfg.range_km = radar.range_km;
    good = configFromJson(body.c_str(), body.length(), cfg);
    if (good) { applyConfig(); currentConfigJson(buf, sizeof buf); }
  }
  if (!good) { server.send(400, "application/json", "{\"error\":\"not JSON\"}"); return; }
  setBacklight(brightness);
  markDirty();
  server.send(200, "application/json", buf);
}
static void hFonts() { char b[600]; fontsJson(b, sizeof b); server.send(200, "application/json", b); }
static void hPress() { touch(); { Lock lock; radar.shortPress(); } ok(); }
static void hHold() { touch(); { Lock lock; radar.longPress(); } markDirty(); ok(); }

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
static void hKeep() {                                // "keep this firmware": the same as /stay, for the settings page
  touch();
  bool c = confirmSelf();
  server.send(c ? 200 : 500, "application/json", c ? "{\"confirmed\":true}" : "{\"confirmed\":false}");
}
// Self-test of certificate checking: connect to servers that deliberately present bad certificates (badssl.com exists
// for this) and to one good one. With checking on, only the good one should connect.
static void hTlsTest() {
  touch();
  static const char *const sites[] = {"https://expired.badssl.com/", "https://self-signed.badssl.com/", "https://wrong.host.badssl.com/",
                                      "https://untrusted-root.badssl.com/", "https://tile.openstreetmap.org/0/0/0.png"};
  String j = String("{\"checking\":") + (verify_certs ? "true" : "false") + ",\"time_set\":" + (time(nullptr) > 1700000000 ? "true" : "false") + ",\"connected\":{";
  for (size_t i = 0; i < sizeof sites / sizeof sites[0]; i++) {
    WiFiClientSecure client;
    secure(client);
    HTTPClient http;
    http.setUserAgent(USER_AGENT);
    http.setConnectTimeout(8000); http.setTimeout(8000);
    int code = http.begin(client, sites[i]) ? http.GET() : -100;
    http.end();
    esp_task_wdt_reset();                             // several slow connections in one call from the main loop
    j += String(i ? "," : "") + "\"" + String(sites[i] + 8) + "\":" + (code > 0 ? "true" : "false");
  }
  server.send(200, "application/json", j + "}}");
}
// "Start this firmware once more, still on trial." Points the boot selector at this image without confirming it, so
// the next start, including one from a power cycle, runs it again as a trial; a reset after that goes back to the other
// firmware as usual. This is how a cold power-on is tested before deciding to keep the firmware.
static void hTrialAgain() {
  touch();
  esp_err_t e = esp_ota_set_boot_partition(running_p);
  const esp_partition_t *b = esp_ota_get_boot_partition();
  server.send(e == ESP_OK ? 200 : 500, "application/json", String("{\"ok\":") + (e == ESP_OK ? "true" : "false") + ",\"next_boot\":\"" + (b ? b->label : "?") +
                                                               "\",\"state\":" + String(runningState()) + ",\"confirmed\":" + (confirmed() ? "true" : "false") + "}");
}
static void hStay() { touch(); bool c = confirmSelf(); server.send(c ? 200 : 500, "text/plain", c ? "confirmed: this image now survives resets" : "confirm failed"); }
static void hStock() {
  touch();
  esp_err_t e = otherHasApp() ? esp_ota_set_boot_partition(other_p) : ESP_FAIL;
  server.send(e == ESP_OK ? 200 : 500, "text/plain", String("next boot: ") + (e == ESP_OK ? other_p->label : "unchanged (other slot has no app)"));
}
static void hReboot() {
  server.send(200, "text/plain", "rebooting");
  cfg_dirty_ms = 0; saveConfigIfDue();                // do not lose a change made a moment ago
  delay(300); ESP.restart();
}
static void hUi() { server.send_P(200, "text/html", ADMIN_HTML); }

void setup() {
  running_p = esp_ota_get_running_partition();
  other_p = esp_ota_get_next_update_partition(NULL);
  if (other_p && other_p->address == running_p->address) other_p = nullptr;
  if (!confirmed() && otherHasApp()) arm_err = esp_ota_set_boot_partition(other_p);   // any reset from here goes back to the other slot

  radar_mu = xSemaphoreCreateMutex();
  radar.logos = &device_logos;
  loadConfig();                                                // saved settings, before anything is drawn
  if (psramOk()) {                                             // with the external memory, keep the still background ready
    void *mem = heap_caps_malloc(sizeof(ScopeCache), MALLOC_CAP_SPIRAM);
    if (mem) radar.cache = new (mem) ScopeCache();
  }
  pinMode(PIN_BUZZ, OUTPUT); digitalWrite(PIN_BUZZ, LOW);                              // keep the buzzer quiet, as stock does
  pinMode(PIN_BTN, INPUT_PULLUP);

  WiFi.mode(WIFI_STA);
  WiFi.setHostname("radarmash");
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);                         // (WiFi.macAddress() reads zeros this early)
  snprintf(ap_name, sizeof ap_name, "RadarMASH-%02X%02X", mac[4], mac[5]);
  bool have_wifi = savedWifi(nullptr);
  if (have_wifi) WiFi.begin();                                 // the network the device already knows
#ifdef WIFI_SSID
  else { WiFi.begin(WIFI_SSID, WIFI_PASS); have_wifi = true; }   // development build with details compiled in
#endif

  panelInit();
  free_q = xQueueCreate(2, sizeof(uint8_t)); work_q = xQueueCreate(2, sizeof(StripJob));
  for (uint8_t i = 0; i < 2; i++) xQueueSend(free_q, &i, 0);
  xTaskCreatePinnedToCore(senderTask, "send", 4096, nullptr, 1, nullptr, 0);   // same priority as the feed task, so they share the core
  snprintf(boot_note, sizeof boot_note, have_wifi ? "joining Wi-Fi" : "starting");
  drawFrame(!live_mode);
  ledcAttach(PIN_BL, 20000, 10); setBacklight(brightness);   // the light is off until driven: nothing else turns it on after a cold start

  uint32_t t0 = millis();
  while (have_wifi && WiFi.status() != WL_CONNECTED && millis() - t0 < 25000) drawFrame(!live_mode);
  boot_note[0] = 0;
  // No network: put up the setup hotspot rather than give up. An unconfirmed image still returns to the other firmware
  // on any reset, or after 30 minutes if nobody uses the hotspot.
  if (WiFi.status() != WL_CONNECTED) {
#ifdef WIFI_SSID
    // Development build: the stored details did not work, so try the compiled-in ones (kept only if they do work).
    memset(&join_req.wanted, 0, sizeof join_req.wanted);
    strlcpy((char *)join_req.wanted.sta.ssid, WIFI_SSID, sizeof join_req.wanted.sta.ssid);
    strlcpy((char *)join_req.wanted.sta.password, WIFI_PASS, sizeof join_req.wanted.sta.password);
    strlcpy(join_req.ssid, WIFI_SSID, sizeof join_req.ssid);
    join_req.had_old = false; join_req.started = millis(); join_req.state = join_req.JOINING;
    tryWifi(join_req.wanted, false);
    uint32_t t1 = millis();
    snprintf(boot_note, sizeof boot_note, "joining Wi-Fi");
    while (WiFi.status() != WL_CONNECTED && millis() - t1 < 20000) drawFrame(!live_mode);
    boot_note[0] = 0;
#endif
    if (WiFi.status() != WL_CONNECTED) startPortal();
  }

  // Anything arriving over the open setup hotspot is sent to the Wi-Fi page; only that page's own calls get through.
  server.addMiddleware([](WebServer &s, Middleware::Callback next) -> bool {
    if (!viaSetupAp() || s.uri() == "/wifi" || s.uri().startsWith("/api/wifi")) return next();
    s.sendHeader("Location", "http://192.168.4.1/wifi");
    s.send(302, "text/plain", "");
    return true;
  });
  server.onNotFound([]() {
    if (viaSetupAp()) { server.sendHeader("Location", "http://192.168.4.1/wifi"); server.send(302, "text/plain", ""); }
    else server.send(404, "text/plain", "not found");
  });
  server.on("/wifi", hWifiPage);
  server.on("/api/wifi/scan", hWifiScan);
  server.on("/api/wifi/status", hWifiStatus);
  server.on("/api/wifi", HTTP_POST, hWifiJoin);
  server.on("/api/wifi/portal", HTTP_POST, hWifiPortal);
  server.on("/api/keep", HTTP_POST, hKeep);
  server.on("/api/trial-again", HTTP_POST, hTrialAgain);
  server.on("/", hUi);
  server.on("/api/status", hStatus);
  server.on("/status", hStatus);                       // older name, kept for scripts
  server.on("/api/settings", HTTP_GET, hSettingsGet);
  server.on("/api/settings", HTTP_POST, hSettingsPost);
  server.on("/api/fonts", hFonts);
  server.on("/api/press", HTTP_POST, hPress);
  server.on("/api/hold", HTTP_POST, hHold);
  server.on("/api/reboot", HTTP_POST, hReboot);
  server.on("/reboot", hReboot);
  server.on("/perf", hPerf);
  server.on("/psramtest", hPsramTest);
  server.on("/tlstest", hTlsTest);
  server.on("/stay", hStay);
  server.on("/stock", hStock);
  server.on("/ota", HTTP_POST, hOtaDone, hOtaRaw);
  server.on("/ota/dry", HTTP_POST, hOtaDone, hOtaDryRaw);
  server.begin();
  xTaskCreatePinnedToCore(feedTask, "feed", 12288, nullptr, 1, nullptr, 0);
  last_activity_ms = millis();
  // What "healthy" means here is that frames keep being drawn, so the watchdog watches the main loop. By default it
  // watches the idle task on the first core instead, but that core is shared by Wi-Fi, the feed, the map and the screen
  // sender, and when several are busy at once (a scan during a map download, say) idle can go unserved for seconds
  // without anything being wrong. That reset the device twice.
  disableCore0WDT();
  enableLoopWDT();
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
  if (btn_down && !long_done && now - btn_down_ms > 700) { long_done = true; { Lock lock; radar.longPress(); } markDirty(); }
  if (btn_down && now - btn_down_ms > 8000) { snprintf(boot_note, sizeof boot_note, "back to other firmware"); drawFrame(!live_mode); delay(150); goHome(); }

  drawFrame(!live_mode);

  static uint32_t checked_ms = 0, frames_then = 0;
  static bool is_confirmed = false;
  if (now - checked_ms > 1000) {                                                      // once a second
    fps = (frames - frames_then) * 1000 / (now - checked_ms); frames_then = frames;
    checked_ms = now; is_confirmed = confirmed();                                     // reads flash, so not every frame
    saveConfigIfDue();
  }
  if (!is_confirmed && now - last_activity_ms > 30UL * 60UL * 1000UL) ESP.restart();   // unconfirmed and left alone: go back
  wifiTick(now);
}
