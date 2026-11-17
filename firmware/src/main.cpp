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
//  - The map and the fast drawing path live in the external memory (PSRAM). If that is ever missing the radar still runs,
//    more slowly and without a map.
#include <Arduino.h>
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
#include <esp_wifi.h>
#include <mbedtls/sha256.h>

#include "admin_html.h"
#include "config.h"
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
    if (boot_note[0]) c.textCentred(180, 300, boot_note, 2, rgb(255, 240, 80));
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
static RawTile raw_tiles[40];
static int raw_next = 0;

static int pngLine(PNGDRAW *d) {
  if (d->y >= 0 && d->y < TILE_SIZE) png->getLineAsRGB565(d, tile_px + d->y * TILE_SIZE, PNG_RGB565_LITTLE_ENDIAN, 0xffffffff);
  return 1;
}

static bool fetchBytes(const String &url, uint8_t *&data, int &len) {
  WiFiClientSecure client;
  client.setInsecure();                             // development build: certificates are not checked yet
  HTTPClient http;
  http.setUserAgent(USER_AGENT);
  http.setConnectTimeout(8000); http.setTimeout(8000);
  if (!http.begin(client, url)) return false;
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
  double lat, lon; float range; MapStyle style; bool want;
  { Lock lock; lat = radar.home_lat; lon = radar.home_lon; range = radar.range_km; style = radar.set.map_style; want = radar.set.map; }
  if (!want || !mapSetup()) return;
  String key = String(lat, 4) + "," + String(lon, 4) + "," + String(range, 0) + "," + String((int)style) + "," + map_url;
  if (key == map_built_for && (map_missing == 0 || millis() - map_built_ms < 60000)) return;   // be gentle on the tile server
  int missing = composeMap(*map_back, device_tiles, lat, lon, range, style);
  Lock lock;
  MapImage *t = map_front; map_front = map_back; map_back = t;
  map_back->version = map_front->version;           // keep the two counters in step so every rebuild looks new
  radar.map = map_front;
  snprintf(radar.map_credit, sizeof radar.map_credit, "%s", map_credit);
  map_missing = missing; map_built_ms = millis(); map_built_for = key;
}

static void feedTask(void *) {
  fresh = (Aircraft *)heap_caps_malloc(sizeof(Aircraft) * MAX_AIRCRAFT, psramOk() ? MALLOC_CAP_SPIRAM : MALLOC_CAP_DEFAULT);
  for (int i = 0; fresh && i < MAX_AIRCRAFT; i++) new (&fresh[i]) Aircraft();
  for (;;) {
    uint32_t began = millis();
    if (WiFi.status() == WL_CONNECTED) updateMap();
    if (fresh && live_mode && WiFi.status() == WL_CONNECTED) { pollFeed(); lookupOneRoute(); }
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
  refresh_s = cfg.refresh_s; include_ground = cfg.include_ground;
  feed_url = cfg.feed_url;                          // (also undoes any automatic swap to the other feed)
  map_url = cfg.map_url;
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
  wifi_config_t wc = {};
  bool wifi_saved = esp_wifi_get_config(WIFI_IF_STA, &wc) == ESP_OK && wc.sta.ssid[0] != 0;
  Lock lock;
  String j = "{\"device\":true,\"version\":\"" FW_VERSION "\"";
  j += ",\"confirmed\":" + String(confirmed() ? "true" : "false");
  j += ",\"live\":" + String(live_mode ? "true" : "false") + ",\"aircraft\":" + String(radar.count);
  j += ",\"feed_age_s\":" + String(feed_ok_ms ? (long)((millis() - feed_ok_ms) / 1000) : -1L) + ",\"feed_error\":\"\"";
  j += ",\"map_missing\":" + String(map_missing < 0 ? 0 : map_missing) + ",\"range_km\":" + String((int)radar.range_km) + ",\"fps\":" + String(fps);
  j += ",\"running\":\"" + String(running_p->label) + "\",\"running_state\":" + String(runningState());
  j += ",\"boot\":\"" + String(b ? b->label : "?") + "\",\"other\":\"" + String(other_p ? other_p->label : "none") + "\",\"arm_err\":" + String((int)arm_err);
  j += ",\"frame_ms\":" + String(frame_ms) + ",\"frames\":" + String(frames) + ",\"brightness\":" + String(brightness);
  j += ",\"feed_count\":" + String(feed_count) + ",\"feed_errors\":" + String(feed_errors) + ",\"lookups\":" + String(lookups);
  j += ",\"home_set\":" + String(cfg.home_set ? "true" : "false") + ",\"wifi_saved\":" + String(wifi_saved ? "true" : "false");
  j += ",\"map\":" + String(radar.map ? "true" : "false") + ",\"map_zoom\":" + String(map_front ? map_front->zoom : -1);
  j += ",\"map_tiles\":" + String(map_tiles_fetched) + ",\"map_tile_failures\":" + String(map_tiles_failed) + ",\"map_decode_failures\":" + String(map_decode_failed);
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
  loadConfig();                                                // saved settings, before anything is drawn
  if (psramOk()) {                                             // with the external memory, keep the still background ready
    void *mem = heap_caps_malloc(sizeof(ScopeCache), MALLOC_CAP_SPIRAM);
    if (mem) radar.cache = new (mem) ScopeCache();
  }
  pinMode(PIN_BUZZ, OUTPUT); digitalWrite(PIN_BUZZ, LOW);                              // keep the buzzer quiet, as stock does
  pinMode(PIN_BTN, INPUT_PULLUP);

  WiFi.mode(WIFI_STA);
  WiFi.setHostname("radarmash");
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  panelInit();
  free_q = xQueueCreate(2, sizeof(uint8_t)); work_q = xQueueCreate(2, sizeof(StripJob));
  for (uint8_t i = 0; i < 2; i++) xQueueSend(free_q, &i, 0);
  xTaskCreatePinnedToCore(senderTask, "send", 4096, nullptr, 1, nullptr, 0);   // same priority as the feed task, so they share the core
  snprintf(boot_note, sizeof boot_note, "joining Wi-Fi");
  drawFrame(true);
  ledcAttach(PIN_BL, 20000, 10); setBacklight(brightness);   // the light is off until driven: nothing else turns it on after a cold start

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 45000) drawFrame(true);
  if (WiFi.status() != WL_CONNECTED) {
    snprintf(boot_note, sizeof boot_note, "Wi-Fi failed");
    drawFrame(true); drawFrame(true); delay(3000);
    ESP.restart();                                           // unconfirmed: back to the other slot. Confirmed: try again.
  }
  boot_note[0] = 0;

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
  server.on("/stay", hStay);
  server.on("/stock", hStock);
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
  if (WiFi.status() == WL_CONNECTED) wifi_lost_ms = 0;
  else { if (!wifi_lost_ms) wifi_lost_ms = now | 1; if (now - wifi_lost_ms > 10UL * 60UL * 1000UL) ESP.restart(); }
}
