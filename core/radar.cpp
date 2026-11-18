// RadarMASH - open firmware for the GeekMagic Radar 360.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "radar.h"
#include "alert.h"
#include "pages.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace rm {

static const float PI_F = 3.14159265f;
static const float TRAIL_DEG = 100.0f;
static const uint32_t CYCLE_MS = 5000;

static const Color COL_RING = rgb(0, 120, 60), COL_TEXT_DIM = rgb(0, 150, 80), COL_SWEEP = rgb(140, 255, 190);
static const Color COL_BLIP = rgb(255, 190, 60), COL_MIL = rgb(255, 110, 90), COL_LABEL = rgb(230, 230, 230), COL_SEL = rgb(255, 240, 80);
static const Color COL_CARD = rgb(0, 14, 8), COL_ROUTE = rgb(120, 220, 255), COL_EMERG = rgb(255, 50, 40);

// atan2 accurate to about 0.01 rad; plenty for a sweep trail and much cheaper than the library call per pixel
static inline float fastAtan2(float y, float x) {
  float ax = fabsf(x), ay = fabsf(y);
  float mx = ax > ay ? ax : ay, mn = ax > ay ? ay : ax;
  if (mx < 1e-9f) return 0;
  float a = mn / mx, s = a * a;
  float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
  if (ay > ax) r = 1.57079637f - r;
  if (x < 0) r = PI_F - r;
  return y < 0 ? -r : r;
}

const Toggle TOGGLES[] = {
  {"sweep", "Sweep", &Settings::sweep}, {"rings", "Range rings", &Settings::rings}, {"labels", "Callsigns", &Settings::labels},
  {"altitude", "Altitude", &Settings::altitude}, {"trails", "Trails", &Settings::trails}, {"fade", "Fade after sweep", &Settings::fade},
  {"shapes", "Aircraft shapes", &Settings::shapes}, {"auto_cycle", "Auto-cycle", &Settings::auto_cycle},
  {"tag", "Tag box", &Settings::tag}, {"tag_route", "Tag: route", &Settings::tag_route}, {"tag_type", "Tag: type", &Settings::tag_type}, {"type_names", "Type in words", &Settings::type_names},
  {"tag_level", "Tag: level", &Settings::tag_level}, {"tag_speed", "Tag: speed", &Settings::tag_speed},
  {"card", "Large card", &Settings::card}, {"map", "Map", &Settings::map}, {"declutter", "Avoid overlapping labels", &Settings::declutter},
  {"logos", "Airline logos", &Settings::logos}, {"clock", "Clock", &Settings::clock}, {"alerts", "Emergency alerts", &Settings::alerts},
  {"visitors", "Unusual visitor notice", &Settings::visitors}, {"visitor_heli", "Notice helicopters too", &Settings::visitor_heli},
  {"auto_range", "Range follows nearest", &Settings::auto_range}, {"page_weather", "Weather page", &Settings::page_weather},
  {"page_board", "Airport board page", &Settings::page_board}, {"page_log", "Overhead log page", &Settings::page_log},
};
const int TOGGLE_COUNT = (int)(sizeof TOGGLES / sizeof TOGGLES[0]);

bool *toggleByName(Settings &s, const char *name) {
  for (int i = 0; i < TOGGLE_COUNT; i++)
    if (strcmp(TOGGLES[i].name, name) == 0) return &(s.*TOGGLES[i].field);
  return nullptr;
}

// Next aircraft in view after `from`. With wrap, goes round for ever; without, ends on -1 (nothing highlighted).
static int nextInView(const Radar &r, int from, bool wrap) {
  float x, y;
  for (int i = from + 1; i < r.count; i++)
    if (r.project(r.ac[i], x, y)) return i;
  if (wrap)
    for (int i = 0; i <= from && i < r.count; i++)
      if (r.project(r.ac[i], x, y)) return i;
  return -1;
}

void Radar::update(Aircraft *fresh, int n, uint32_t t) {
  if (n > MAX_AIRCRAFT) n = MAX_AIRCRAFT;
  char sel_hex[sizeof ac[0].hex] = "";
  if (selected >= 0 && selected < count) memcpy(sel_hex, ac[selected].hex, sizeof sel_hex);
  const double kx = 111.32 * cos(home_lat * 3.14159265358979 / 180.0);
  for (int i = 0; i < n; i++) {
    Aircraft &f = fresh[i];
    f.live = true; f.fix_lat = f.lat; f.fix_lon = f.lon; f.fix_ms = t;
    f.trail_len = 0;
    const Aircraft *old = nullptr;
    for (int k = 0; k < count && f.hex[0]; k++)
      if (strcmp(ac[k].hex, f.hex) == 0) { old = &ac[k]; break; }
    if (!old) continue;
    f.label_slot = old->label_slot;                                  // its label stays where it was
    if (!f.airline_icao[0]) memcpy(f.airline_icao, old->airline_icao, sizeof f.airline_icao);
    if (!f.airline[0]) memcpy(f.airline, old->airline, sizeof f.airline);
    if (!f.origin[0]) { memcpy(f.origin, old->origin, sizeof f.origin); memcpy(f.dest, old->dest, sizeof f.dest); }
    // the previous report becomes the newest trail point, unless the aircraft has barely moved
    double e = (f.lon - old->fix_lon) * kx, nn = (f.lat - old->fix_lat) * 110.57;
    bool moved = old->live && e * e + nn * nn > 0.15 * 0.15;
    int keep = old->trail_len;
    if (moved) {
      f.trail_lat[0] = (float)old->fix_lat; f.trail_lon[0] = (float)old->fix_lon;
      if (keep > TRAIL_POINTS - 1) keep = TRAIL_POINTS - 1;
    }
    for (int k = 0; k < keep; k++) { f.trail_lat[k + (moved ? 1 : 0)] = old->trail_lat[k]; f.trail_lon[k + (moved ? 1 : 0)] = old->trail_lon[k]; }
    f.trail_len = keep + (moved ? 1 : 0);
  }
  for (int i = 0; i < n; i++) ac[i] = fresh[i];
  count = n;
  selected = -1;
  for (int i = 0; i < n && sel_hex[0]; i++)
    if (strcmp(ac[i].hex, sel_hex) == 0) { selected = i; break; }

  // The range follows the nearest aircraft: out when it is beyond the edge, in when it would sit well inside the next
  // range down. Not more often than every fifteen seconds, so the map is not for ever being redrawn.
  if (set.auto_range && !pin_hex[0] && n > 0 && (!range_auto_ms_ || t - range_auto_ms_ > 15000)) {
    double nearest = 1e12;
    for (int i = 0; i < n; i++) {
      if (ac[i].on_ground) continue;
      double e = (ac[i].lon - home_lon) * kx, nn = (ac[i].lat - home_lat) * 110.57, d = sqrt(e * e + nn * nn);
      if (d < nearest) nearest = d;
    }
    float want = range_km;
    if (nearest < 1e11) {
      const int steps = (int)(sizeof RANGES_KM / sizeof RANGES_KM[0]);
      if (nearest > range_km * 0.95f || range_km > AUTO_RANGE_MAX_KM) {
        want = AUTO_RANGE_MAX_KM;
        for (int i = steps - 1; i >= 0; i--) if (RANGES_KM[i] <= AUTO_RANGE_MAX_KM && nearest <= RANGES_KM[i] * 0.85f) want = RANGES_KM[i];
      } else {
        for (int i = steps - 1; i >= 0; i--) if (RANGES_KM[i] < range_km && nearest <= RANGES_KM[i] * 0.7f) want = RANGES_KM[i];
      }
    }
    if (want != range_km) { range_km = want; range_auto_ms_ = t | 1; }
  }
}

float Radar::feedRangeKm() const {
  return set.auto_range && !pin_hex[0] && range_km < AUTO_RANGE_MAX_KM ? AUTO_RANGE_MAX_KM : range_km;
}

void Radar::tick(uint32_t t) {
  now_ms = t;
  // Between updates, carry each live aircraft on along its track so it glides instead of jumping. Capped, so an
  // aircraft whose reports stop does not sail away for ever.
  const double kx = 111.32 * cos(home_lat * 3.14159265358979 / 180.0);
  for (int i = 0; i < count; i++) {
    Aircraft &a = ac[i];
    if (!a.live) continue;
    uint32_t age = t - a.fix_ms;
    if (age > 30000) age = 30000;
    double km = a.on_ground || a.speed_kt < 30 ? 0 : a.speed_kt * 1.852 * age / 3600000.0;
    double tr = a.track_deg * PI_F / 180.0;
    a.lat = a.fix_lat + cos(tr) * km / 110.57;
    a.lon = a.fix_lon + sin(tr) * km / kx;
  }
  const uint32_t period = (uint32_t)(set.sweep_seconds < 1 ? 1 : set.sweep_seconds) * 1000u;
  sweep_deg = (float)(t % period) * 360.0f / (float)period;
  float x, y;
  if (pin_hex[0]) {                                                  // held on one aircraft: no cycling
    selected = -1;
    for (int i = 0; i < count; i++)
      if (strcmp(ac[i].hex, pin_hex) == 0 && project(ac[i], x, y)) { selected = i; break; }
    return;
  }
  bool gone = selected >= 0 && (selected >= count || !project(ac[selected], x, y));   // flew out of view
  if (set.auto_cycle && (selected < 0 || gone || t - last_cycle_ms >= CYCLE_MS)) {
    selected = nextInView(*this, selected, true);
    last_cycle_ms = t;
  } else if (gone) selected = -1;
}

void Radar::toScreen(double lat, double lon, float &x, float &y) const {
  // flat-earth approximation around home; fine at radar ranges
  if (home_lat != kx_for_lat_) { kx_ = (float)(111.32 * cos(home_lat * 3.14159265358979 / 180.0)); kx_for_lat_ = home_lat; }
  float east_km = (float)(lon - home_lon) * kx_;                  // subtract at full precision, then the rest in fast float
  float north_km = (float)(lat - home_lat) * 110.57f;
  float k = RADAR_RADIUS / range_km;
  x = CENTRE + east_km * k;
  y = CENTRE - north_km * k;
}

static inline bool inScope(float x, float y, float margin) {
  float dx = x - CENTRE, dy = y - CENTRE, r = RADAR_RADIUS - margin;
  return dx * dx + dy * dy <= r * r;
}

bool Radar::project(const Aircraft &a, float &x, float &y) const {
  toScreen(a.lat, a.lon, x, y);
  return inScope(x, y, 4);
}

void Radar::shortPress() {
  selected = nextInView(*this, selected, set.auto_cycle);
  last_cycle_ms = now_ms;                       // a press restarts the dwell on the new aircraft
}

void Radar::longPress() {
  const int n = (int)(sizeof RANGES_KM / sizeof RANGES_KM[0]);
  int next = 0;
  for (int i = 0; i < n; i++)
    if (RANGES_KM[i] > range_km + 0.5f) { next = i; break; }      // first range bigger than now, else back to the smallest
  range_km = RANGES_KM[next];
  selected = -1;
}

// Background: rings, axes and the sweep trail, computed per pixel so the rings and the trail are smooth.
// This runs for every pixel of every frame, and the device has no fast square root or division, so it only does the
// expensive sums where they are needed: the square root near a ring, the angle on the half of the scope behind the sweep.
static void drawScope(Canvas &c, const Settings &set, float sweep_deg, const MapImage *map) {
  const int bright = set.map_brightness > 100 ? 100 : set.map_brightness;
  const float outer2 = (RADAR_RADIUS + 1.0f) * (RADAR_RADIUS + 1.0f);
  float ring_r[3], ring_lo2[3], ring_hi2[3];
  int rings = 0;
  for (int k = set.rings ? 1 : 3; k <= 3; k++) {                   // with rings off, the outer edge is still drawn
    float rr = RADAR_RADIUS * k / 3.0f;
    ring_r[rings] = rr; ring_lo2[rings] = (rr - 1.0f) * (rr - 1.0f); ring_hi2[rings] = (rr + 1.0f) * (rr + 1.0f);
    rings++;
  }
  const float sweep_rad = sweep_deg * PI_F / 180.0f, sx = sinf(sweep_rad), sy = -cosf(sweep_rad);   // unit vector along the sweep
  const float to_deg = 180.0f / PI_F;

  for (int y = c.y0; y < c.y0 + c.rows; y++) {
    uint16_t *row = c.row(y);
    const uint16_t *map_row = map ? map->px + y * SCREEN_W : nullptr;
    float dy = y - CENTRE;
    for (int x = 0; x < SCREEN_W; x++) {
      float dx = x - CENTRE;
      float r2 = dx * dx + dy * dy;
      if (r2 > outer2) { row[x] = 0; continue; }
      float g = 10.0f;                                             // faint scope glow
      for (int k = 0; k < rings; k++)
        if (r2 > ring_lo2[k] && r2 < ring_hi2[k]) g += (1.0f - fabsf(sqrtf(r2) - ring_r[k])) * 110.0f;
      if (set.rings && (fabsf(dx) < 0.6f || fabsf(dy) < 0.6f)) g += 35.0f;
      if (set.sweep && r2 > 4.0f) {
        float cross = sy * dx - sx * dy;                           // positive on the side the sweep has just left
        if (cross > 0) {
          float behind = fastAtan2(cross, dx * sx + dy * sy) * to_deg;   // degrees since the sweep passed this pixel
          if (behind < TRAIL_DEG) { float t = 1.0f - behind / TRAIL_DEG; g += t * t * 120.0f; }
        }
      }
      if (g > 255.0f) g = 255.0f;
      int r8 = (int)(g * 0.10f), g8 = (int)g, b8 = (int)(g * 0.45f);
      if (map_row) {                                               // scope glow is added on top of the dimmed map
        uint16_t m = map_row[x];
        r8 += ((m >> 11) << 3) * bright / 100; g8 += (((m >> 5) & 63) << 2) * bright / 100; b8 += ((m & 31) << 3) * bright / 100;
      }
      row[x] = rgb(r8 > 255 ? 255 : r8, g8 > 255 ? 255 : g8, b8 > 255 ? 255 : b8);
    }
  }
}

// ---- cached scope ----
static uint32_t scopeKey(const Radar &r, const MapImage *underlay) {
  uint32_t k = 0x9E3779B9u;
  k = k * 31 + (r.set.rings ? 1 : 2);
  k = k * 31 + (underlay ? 1000 + r.set.map_brightness : 7);
  k = k * 31 + (underlay ? underlay->version : 0);
  k = k * 31 + (uint32_t)(uintptr_t)underlay;
  return k ? k : 1;
}

void Radar::prepare() {
  if (!cache) return;
  if (!cache->bearings_done) {
    for (int y = 0; y < SCREEN_H; y++)
      for (int x = 0; x < SCREEN_W; x++) {
        float b = fastAtan2(x - CENTRE, -(y - CENTRE)) * (32768.0f / PI_F);       // -32768..32768
        cache->bearing[y * SCREEN_W + x] = (uint16_t)(int)(b < 0 ? b + 65536.0f : b);
      }
    cache->bearings_done = true;
  }
  const MapImage *underlay = set.map && map && map->ready ? map : nullptr;
  uint32_t k = scopeKey(*this, underlay);
  if (cache->valid && cache->key == k) return;
  Settings still = set;
  still.sweep = false;
  Canvas whole(cache->bg, 0, SCREEN_H);
  drawScope(whole, still, 0, underlay);
  cache->key = k; cache->valid = true;
}

// Brightness the sweep trail adds, by how far behind the sweep a pixel is: 1024 steps round the circle.
static const uint8_t *trailTable() {
  static uint8_t table[1024];
  static bool done = false;
  if (!done) {
    for (int i = 0; i < 1024; i++) {
      float behind = i * 360.0f / 1024.0f;
      float t = behind < TRAIL_DEG ? 1.0f - behind / TRAIL_DEG : 0.0f;
      table[i] = (uint8_t)(t * t * 120.0f);
    }
    done = true;
  }
  return table;
}

// The fast way to draw the background: copy the kept picture and add the sweep trail with whole-number sums only.
static void drawScopeCached(Canvas &c, const ScopeCache &cache, bool sweep, float sweep_deg) {
  const uint8_t *trail = trailTable();
  const uint16_t sweep16 = (uint16_t)(int)(sweep_deg * (65536.0f / 360.0f));
  const float outer2 = (RADAR_RADIUS + 1.0f) * (RADAR_RADIUS + 1.0f);
  // unit vectors along the sweep (b) and along the far end of its trail (a)
  const float b_rad = sweep_deg * PI_F / 180.0f, a_rad = (sweep_deg - TRAIL_DEG) * PI_F / 180.0f;
  const float b_x = sinf(b_rad), b_y = -cosf(b_rad), a_x = sinf(a_rad), a_y = -cosf(a_rad);
  for (int y = c.y0; y < c.y0 + c.rows; y++) {
    uint16_t *row = c.row(y);
    memcpy(row, cache.bg + y * SCREEN_W, SCREEN_W * sizeof(uint16_t));
    if (!sweep) continue;
    float dy = y - CENTRE, half2 = outer2 - dy * dy;
    if (half2 <= 0) continue;
    float half = sqrtf(half2);                                     // only the part of this row inside the glass...
    float lo = -half, hi = half;                                   // ...as offsets from the centre column
    // ...and inside the trail's wedge. The wedge is where two straight edges both hold, and along one row each edge
    // is a single cut-off in x. So most of the row is skipped without looking at it.
    bool none = false;
    for (int e = 0; e < 2; e++) {
      float k = e ? -a_y : b_y, m = e ? -a_x * dy : b_x * dy;      // the edge is k * dx >= m
      if (k > 1e-4f) { float v = m / k; if (v > lo) lo = v; }
      else if (k < -1e-4f) { float v = m / k; if (v < hi) hi = v; }
      else if (m > 0) none = true;
    }
    if (none || lo > hi) continue;
    int x0 = (int)(CENTRE + lo) - 1, x1 = (int)(CENTRE + hi) + 2;   // a pixel of slack; the table below has the last word
    if (x0 < 0) x0 = 0;
    if (x1 > SCREEN_W - 1) x1 = SCREEN_W - 1;
    const uint16_t *bearing = cache.bearing + y * SCREEN_W;
    for (int x = x0; x <= x1; x++) {
      uint16_t p = row[x];
      if (!p) continue;                                            // outside the glass (inside is never pure black)
      int v = trail[(uint16_t)(sweep16 - bearing[x]) >> 6];
      if (!v) continue;
      int r = (p >> 11) + v / 80, g = ((p >> 5) & 63) + v / 4, b = (p & 31) + v * 9 / 160;   // the scope's green tint
      row[x] = (uint16_t)(((r > 31 ? 31 : r) << 11) | ((g > 63 ? 63 : g) << 5) | (b > 31 ? 31 : b));
    }
  }
}

// ---- silhouettes ----
// Each is a list of triangles in a unit square: nose at (0,-1), x to the right, y towards the tail.
struct Tri { float ax, ay, bx, by, cx, cy; };

static const Tri SHAPE_NARROW[] = {                     // single-aisle airliner: slim body, swept wings
  {0, -1.0f, -0.13f, -0.7f, 0.13f, -0.7f},
  {-0.13f, -0.7f, 0.13f, -0.7f, 0.13f, 0.85f}, {-0.13f, -0.7f, 0.13f, 0.85f, -0.13f, 0.85f},
  {0, -0.35f, -1.0f, 0.3f, 0, 0.12f}, {0, -0.35f, 1.0f, 0.3f, 0, 0.12f},
  {0, 0.55f, -0.42f, 0.98f, 0, 0.9f}, {0, 0.55f, 0.42f, 0.98f, 0, 0.9f},
};
static const Tri SHAPE_WIDE[] = {                       // twin-aisle / heavy: fat body, broad wings, engines
  {0, -1.0f, -0.2f, -0.68f, 0.2f, -0.68f},
  {-0.2f, -0.68f, 0.2f, -0.68f, 0.2f, 0.85f}, {-0.2f, -0.68f, 0.2f, 0.85f, -0.2f, 0.85f},
  {0, -0.45f, -1.0f, 0.38f, 0, 0.2f}, {0, -0.45f, 1.0f, 0.38f, 0, 0.2f},
  {0, 0.5f, -0.5f, 1.0f, 0, 0.9f}, {0, 0.5f, 0.5f, 1.0f, 0, 0.9f},
  {-0.5f, -0.28f, -0.36f, -0.28f, -0.43f, 0.08f}, {0.5f, -0.28f, 0.36f, -0.28f, 0.43f, 0.08f},
};
static const Tri SHAPE_LIGHT[] = {                      // light aircraft: straight high wing, small tail
  {-0.1f, -0.9f, 0.1f, -0.9f, 0.1f, 0.9f}, {-0.1f, -0.9f, 0.1f, 0.9f, -0.1f, 0.9f},
  {-1.0f, -0.42f, 1.0f, -0.42f, 1.0f, -0.12f}, {-1.0f, -0.42f, 1.0f, -0.12f, -1.0f, -0.12f},
  {-0.4f, 0.68f, 0.4f, 0.68f, 0.4f, 0.9f}, {-0.4f, 0.68f, 0.4f, 0.9f, -0.4f, 0.9f},
};
static const Tri SHAPE_MIL[] = {                        // military: delta with a notched tail
  {0, -1.0f, -0.85f, 0.85f, 0, 0.4f}, {0, -1.0f, 0.85f, 0.85f, 0, 0.4f},
};
static const Tri SHAPE_ARROW[] = {                      // plain arrowhead, when the kind is unknown or shapes are off
  {0, -1.0f, -0.62f, 0.62f, 0.62f, 0.62f},
};

// Silhouettes are drawn once into small coverage masks and reused, because filling triangles with soft edges for every
// aircraft on every frame is slow on the device. A mask is kept per shape, size and heading (in 2 degree steps).
static const int ICON = 32;                               // masks are ICON x ICON, centred on the aircraft
struct IconMask { const Tri *tris; uint8_t size, heading; uint32_t used; uint8_t a[ICON * ICON]; };
static IconMask icon_masks[12];
static uint32_t icon_clock = 0;

static void fillMask(IconMask &m, const Tri *t, int n, float size, float track_deg) {
  memset(m.a, 0, sizeof m.a);
  const float ang = track_deg * PI_F / 180.0f, co = cosf(ang) * size, si = sinf(ang) * size, mid = ICON / 2;
  static const float off[2] = {0.25f, 0.75f};
  for (int i = 0; i < n; i++) {
    float ax = mid + t[i].ax * co - t[i].ay * si, ay = mid + t[i].ax * si + t[i].ay * co;
    float bx = mid + t[i].bx * co - t[i].by * si, by = mid + t[i].bx * si + t[i].by * co;
    float cx = mid + t[i].cx * co - t[i].cy * si, cy = mid + t[i].cx * si + t[i].cy * co;
    float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if (fabsf(area) < 1e-6f) continue;
    float s = area < 0 ? -1.0f : 1.0f;
    int x0 = (int)floorf(fminf(ax, fminf(bx, cx))), x1 = (int)ceilf(fmaxf(ax, fmaxf(bx, cx)));
    int y0 = (int)floorf(fminf(ay, fminf(by, cy))), y1 = (int)ceilf(fmaxf(ay, fmaxf(by, cy)));
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > ICON - 1) x1 = ICON - 1;
    if (y1 > ICON - 1) y1 = ICON - 1;
    for (int y = y0; y <= y1; y++)
      for (int x = x0; x <= x1; x++) {
        int hit = 0;                                      // 2x2 samples per pixel give the soft edge
        for (int j = 0; j < 2; j++)
          for (int k = 0; k < 2; k++) {
            float qx = x + off[k] - 0.5f, qy = y + off[j] - 0.5f;
            if (s * ((bx - ax) * (qy - ay) - (by - ay) * (qx - ax)) >= 0 && s * ((cx - bx) * (qy - by) - (cy - by) * (qx - bx)) >= 0 &&
                s * ((ax - cx) * (qy - cy) - (ay - cy) * (qx - cx)) >= 0) hit++;
          }
        int a = hit * 255 / 4;
        if (a > m.a[y * ICON + x]) m.a[y * ICON + x] = (uint8_t)a;   // triangles overlap: keep the most covered
      }
  }
}

static void drawTris(Canvas &c, const Tri *t, int n, float x, float y, float track_deg, float size, Color col) {
  int ix = (int)lroundf(x) - ICON / 2, iy = (int)lroundf(y) - ICON / 2;
  if (iy >= c.y0 + c.rows || iy + ICON <= c.y0) return;   // not in this strip
  uint8_t heading = (uint8_t)(((int)lroundf(track_deg / 2.0f) % 180 + 180) % 180);
  IconMask *m = nullptr, *oldest = &icon_masks[0];
  for (IconMask &e : icon_masks) {
    if (e.tris == t && e.size == (uint8_t)size && e.heading == heading) { m = &e; break; }
    if (e.used < oldest->used) oldest = &e;
  }
  if (!m) {
    m = oldest;
    m->tris = t; m->size = (uint8_t)size; m->heading = heading;
    fillMask(*m, t, n, size, heading * 2.0f);
  }
  m->used = ++icon_clock;
  for (int my = 0; my < ICON; my++) {
    int py = iy + my;
    if (!c.hasRow(py)) continue;
    const uint8_t *a = m->a + my * ICON;
    for (int mx = 0; mx < ICON; mx++)
      if (a[mx]) c.blend(ix + mx, py, col, a[mx]);
  }
}
#define TRIS(s) s, (int)(sizeof(s) / sizeof(s[0]))

static void drawIcon(Canvas &c, Shape shape, float x, float y, float track_deg, Color col) {
  switch (shape) {
    case Shape::Narrow: drawTris(c, TRIS(SHAPE_NARROW), x, y, track_deg, 10, col); break;
    case Shape::Wide: drawTris(c, TRIS(SHAPE_WIDE), x, y, track_deg, 13, col); break;
    case Shape::Light: drawTris(c, TRIS(SHAPE_LIGHT), x, y, track_deg, 7, col); break;
    case Shape::Military: drawTris(c, TRIS(SHAPE_MIL), x, y, track_deg, 10, col); break;
    case Shape::Heli: {                                 // body, tail boom and a rotor cross
      float a = track_deg * PI_F / 180.0f, sx = sinf(a), sy = -cosf(a);
      c.line(x, y, x - sx * 10, y - sy * 10, col);
      c.disc(x + sx * 1.5f, y + sy * 1.5f, 3.6f, col);
      c.line(x - 8, y - 8, x + 8, y + 8, col); c.line(x - 8, y + 8, x + 8, y - 8, col);
      break;
    }
    default: drawTris(c, TRIS(SHAPE_ARROW), x, y, track_deg, 8, col); break;
  }
}

static void drawTrail(Canvas &c, const Radar &r, const Aircraft &a, float x, float y, Color col) {
  float px = x, py = y;
  for (int i = 0; i < a.trail_len; i++) {
    float tx, ty;
    r.toScreen(a.trail_lat[i], a.trail_lon[i], tx, ty);
    if (!inScope(tx, ty, 2)) break;
    c.line(px, py, tx, ty, col, 200 - i * 190 / TRAIL_POINTS);   // fades with age
    px = tx; py = ty;
  }
}

// Scale a colour's brightness, 0..1.
static inline Color dim(Color c, float f) {
  return (Color)(((int)((c >> 11) * f) << 11) | ((int)(((c >> 5) & 63) * f) << 5) | (int)((c & 31) * f));
}

// How bright an aircraft is: full as the sweep crosses it, falling away until the sweep comes round again.
static float sweepFade(float x, float y, float sweep_deg) {
  float bearing = fastAtan2(x - CENTRE, -(y - CENTRE)) * 180.0f / PI_F;
  float behind = fmodf(sweep_deg - bearing + 720.0f, 360.0f);
  return 1.0f - 0.7f * behind / 360.0f;
}

// ---- labels and the tag box ----
// What the tag box says about an aircraft: its route line and its small data line, either of which may be empty.
static void tagText(const Settings &set, const Aircraft &a, char *route, size_t rn, char *data, size_t dn) {
  route[0] = 0; data[0] = 0;
  if (set.tag_route && a.origin[0] && a.dest[0]) snprintf(route, rn, "%s>%s", a.origin, a.dest);
  int n = 0;
  if (isEmergencySquawk(a.squawk)) n += snprintf(data + n, dn - n, "%s ", a.squawk);
  if (set.tag_type && a.type[0]) n += snprintf(data + n, dn - n, "%s ", typeShown(set, a.type));
  if (set.tag_level) n += snprintf(data + n, dn - n, "FL%03d ", a.alt_ft / 100);
  if (set.tag_speed) n += snprintf(data + n, dn - n, "%dkt ", (int)a.speed_kt);
  if (n) data[n - 1] = 0;                                            // drop the trailing space
}

struct Box { int x0, y0, x1, y1; };                                  // x1 and y1 are one past the last pixel
// Where the clock and the alert notice sit: both centred, the clock low and the notice high.
static const int CLOCK_Y = 312, CLOCK_X0 = 146, ALERT_Y = 30, ALERT_X0 = 82, ALERT_H = 38;
static inline int overlap(const Box &a, const Box &b) {
  int w = (a.x1 < b.x1 ? a.x1 : b.x1) - (a.x0 > b.x0 ? a.x0 : b.x0), h = (a.y1 < b.y1 ? a.y1 : b.y1) - (a.y0 > b.y0 ? a.y0 : b.y0);
  return w > 0 && h > 0 ? w * h : 0;
}
static inline bool boxInScope(const Box &b, float margin) {
  return inScope(b.x0, b.y0, margin) && inScope(b.x1, b.y0, margin) && inScope(b.x0, b.y1, margin) && inScope(b.x1, b.y1, margin);
}

// Decide where the tag box and every label go. Done once per frame, however many strips the frame is drawn in.
//  - Every aircraft's icon is somewhere a label may not sit; so is the tag box, placed first.
//  - Each label then tries six places round its aircraft, starting with where it was last frame so labels stay put
//    while they can, and takes the first that is on the scope and clear. If none is clear it takes the least bad.
void Radar::layoutLabels() const {
  uint32_t key = 2166136261u;
  auto mixin = [&key](uint32_t v) { key = (key ^ v) * 16777619u; };
  mixin(now_ms); mixin((uint32_t)count); mixin((uint32_t)selected); mixin((uint32_t)(range_km * 16)); mixin(set.font);
  mixin((set.labels ? 1u : 0) | (set.altitude ? 2u : 0) | (set.tag ? 4u : 0) | (set.tag_route ? 8u : 0) | (set.tag_type ? 16u : 0) |
        (set.tag_level ? 32u : 0) | (set.tag_speed ? 64u : 0) | (set.declutter ? 128u : 0) | (set.logos ? 256u : 0) | (set.card ? 512u : 0) |
        (set.clock && clock_min >= 0 ? 1024u : 0) | (alert[0] ? 2048u : 0) | (set.type_names ? 4096u : 0));
  // the highlighted aircraft's logo may arrive at any moment; when it does the box grows to hold it
  const Logo *logo = set.logos && logos && (set.tag || set.card) && selected >= 0 && selected < count && ac[selected].airline_icao[0] ? logos->logo(ac[selected].airline_icao) : nullptr;
  mixin((uint32_t)(uintptr_t)logo);
  for (int i = 0; i < count; i++) { mixin((uint32_t)(int32_t)(ac[i].lat * 1e5)); mixin((uint32_t)(int32_t)(ac[i].lon * 1e5)); mixin((uint8_t)ac[i].callsign[0]); }
  if (key == layout_key_) return;                                    // same frame: already done
  layout_key_ = key;

  setFont(set.font);
  static Box taken[MAX_AIRCRAFT * 2 + 1];
  static float sx[MAX_AIRCRAFT], sy[MAX_AIRCRAFT];
  static bool seen[MAX_AIRCRAFT];
  int n_taken = 0;
  for (int i = 0; i < count; i++) {
    seen[i] = project(ac[i], sx[i], sy[i]);
    if (seen[i]) taken[n_taken++] = Box{(int)sx[i] - 12, (int)sy[i] - 12, (int)sx[i] + 13, (int)sy[i] + 13};
  }
  const int n_icons = n_taken;                                       // what follows is furniture, not aircraft
  if (set.clock && clock_min >= 0) taken[n_taken++] = Box{CLOCK_X0, CLOCK_Y, SCREEN_W - CLOCK_X0, CLOCK_Y + lineHeight(2) + 4};
  if (alert[0]) taken[n_taken++] = Box{ALERT_X0, ALERT_Y, SCREEN_W - ALERT_X0, ALERT_Y + ALERT_H};

  tag_w_ = 0;
  tag_logo_ = logo;
  if (selected >= 0 && selected < count && seen[selected] && set.tag) {
    const Aircraft &a = ac[selected];
    char route[16], data[48];
    tagText(set, a, route, sizeof route, data, sizeof data);
    const int line = lineHeight(2) + 3, small = lineHeight(1) + 3;
    int w = Canvas::textWidth(a.callsign, 2), h = line - 3;
    if (route[0]) { int rw = Canvas::textWidth(route, 2); if (rw > w) w = rw; h += line; }
    if (data[0]) { int dw = Canvas::textWidth(data, 1); if (dw > w) w = dw; h += small; }
    w += 10; h += 8;
    if (logo) { w += LOGO_SIZE + 6; if (h < LOGO_SIZE + 8) h = LOGO_SIZE + 8; }
    const int x = (int)sx[selected], y = (int)sy[selected], gap = 20;
    const int cand[4][2] = {{x + gap, y - h / 2}, {x - gap - w, y - h / 2}, {x - w / 2, y + gap}, {x - w / 2, y - gap - h}};
    int bx = cand[0][0], by = cand[0][1];
    const Box notice{ALERT_X0, ALERT_Y, SCREEN_W - ALERT_X0, ALERT_Y + ALERT_H};
    for (int k = 0; k < 4; k++) {
      Box b{cand[k][0], cand[k][1], cand[k][0] + w, cand[k][1] + h};
      if (boxInScope(b, 3) && !(alert[0] && overlap(b, notice))) { bx = cand[k][0]; by = cand[k][1]; break; }   // on the glass and clear of the notice
    }
    tag_x_ = (int16_t)bx; tag_y_ = (int16_t)by; tag_w_ = (int16_t)w; tag_h_ = (int16_t)h;
    taken[n_taken++] = Box{bx - 2, by - 2, bx + w + 2, by + h + 2};
  }

  const int h2 = lineHeight(2), h = h2 + (set.altitude ? 2 + lineHeight(1) : 0);
  for (int i = 0; i < count; i++) {
    const Aircraft &a = ac[i];
    if (!seen[i] || (i == selected && set.tag)) continue;
    const int x = (int)sx[i], y = (int)sy[i], w = Canvas::textWidth(a.callsign, 2);
    // right, left, the four diagonals, then centred below and above (the only places a long label fits at the very
    // top or bottom of the round screen)
    const int cand[8][2] = {{x + 16, y - h2}, {x - 16 - w, y - h2}, {x + 6, y - 15 - h}, {x + 6, y + 15}, {x - 6 - w, y - 15 - h}, {x - 6 - w, y + 15},
                            {x - w / 2, y + 15}, {x - w / 2, y - 15 - h}};
    int best = -1, best_cost = 0;
    for (int t = 0; t < 9; t++) {
      int k = t == 0 ? (a.label_slot < 8 ? a.label_slot : 0) : t - 1;   // first where it was, then each in turn
      if (t > 0 && k == a.label_slot) continue;
      Box b{cand[k][0] - 1, cand[k][1] - 1, cand[k][0] + w + 1, cand[k][1] + h + 1};
      int cost = boxInScope(b, 4) ? 0 : 1000000;                     // off the glass is worse than any overlap
      if (set.declutter)
        for (int q = 0; q < n_taken; q++) {
          if (q < n_icons && taken[q].x0 == (int)sx[i] - 12 && taken[q].y0 == (int)sy[i] - 12) continue;   // its own icon
          cost += overlap(b, taken[q]);
        }
      if (best < 0 || cost < best_cost) { best = k; best_cost = cost; }
      if (cost == 0) break;
      if (!set.declutter && t >= 2 && best_cost < 1000000) break;    // without tidying: right, else left, unless neither is on the glass
    }
    a.label_slot = (uint8_t)best; a.label_x = (int16_t)cand[best][0]; a.label_y = (int16_t)cand[best][1];
    if (set.labels) taken[n_taken++] = Box{a.label_x, a.label_y, a.label_x + w, a.label_y + h};
  }
}

// Small box beside the highlighted aircraft: callsign, then whichever extra lines the settings ask for.
static void drawLogo(Canvas &c, const Logo &l, int x, int y) {
  for (int ly = 0; ly < LOGO_SIZE; ly++) {
    if (!c.hasRow(y + ly)) continue;
    const uint16_t *px = l.px + ly * LOGO_SIZE;
    const uint8_t *a = l.alpha + ly * LOGO_SIZE;
    for (int lx = 0; lx < LOGO_SIZE; lx++)
      if (a[lx]) c.blend(x + lx, y + ly, px[lx], a[lx]);
  }
}

static void drawTag(Canvas &c, const Settings &set, const Aircraft &a, int bx, int by, int w, int h, const Logo *logo) {
  char route[16], data[48];
  tagText(set, a, route, sizeof route, data, sizeof data);
  const int line = lineHeight(2) + 3;
  c.blendRect(bx, by, w, h, COL_CARD, 225);
  c.blendRect(bx, by, w, 1, COL_SEL, 255); c.blendRect(bx, by + h - 1, w, 1, COL_SEL, 255);
  c.blendRect(bx, by, 1, h, COL_SEL, 255); c.blendRect(bx + w - 1, by, 1, h, COL_SEL, 255);
  int tx = bx + 5, ty = by + 5;
  if (logo) {
    drawLogo(c, *logo, bx + 4, by + (h - LOGO_SIZE) / 2);
    tx += LOGO_SIZE + 6;
    int lines = 1 + (route[0] ? 1 : 0), text_h = lines * line - 3 + (data[0] ? lineHeight(1) + 3 : 0);
    if (text_h < h - 8) ty = by + (h - text_h) / 2;                  // a short tag: centre the text beside the logo
  }
  c.text(tx, ty, a.callsign, 2, COL_SEL); ty += line;
  if (route[0]) { c.text(tx, ty, route, 2, COL_ROUTE); ty += line; }
  if (data[0]) c.text(tx, ty, data, 1, COL_LABEL);
}

static void drawAircraft(Canvas &c, const Radar &r, const Aircraft &a, float x, float y, bool sel, const int16_t *tag, const Logo *logo) {
  const Settings &set = r.set;
  const bool emerg = isEmergencySquawk(a.squawk);                    // in distress: red, never faded, with a pulsing ring
  Color col = emerg ? COL_EMERG : sel ? COL_SEL : a.shape == Shape::Military ? COL_MIL : COL_BLIP;
  Color label = COL_LABEL, level = COL_TEXT_DIM;
  if (set.fade && set.sweep && !sel && !emerg) {                               // the highlighted aircraft always stays bright
    float f = sweepFade(x, y, r.sweep_deg);
    col = dim(col, f); label = dim(label, f); level = dim(level, f);
  }
  if (set.trails) drawTrail(c, r, a, x, y, col);
  // The icon, label and tag box all sit within this many rows of the aircraft. Outside that, this strip has nothing
  // more of this aircraft, so skip the formatting and drawing (which adds up over nine strips and a full sky).
  if (y + 90 < c.y0 || y - 90 >= c.y0 + c.rows) return;
  drawIcon(c, set.shapes ? a.shape : Shape::Unknown, x, y, a.track_deg, col);
  if (sel) c.ring(x, y, 17, COL_SEL);
  if (emerg) c.ring(x, y, 20.0f + (float)(r.now_ms % 1000) * 0.008f, COL_EMERG);
  if (sel && set.tag && tag[2] > 0) { drawTag(c, set, a, tag[0], tag[1], tag[2], tag[3], logo); return; }
  if (!set.labels && !sel) return;
  const int h2 = lineHeight(2);
  if (a.label_slot >= 2) {                                           // moved away from its aircraft: a short tick ties them together
    float x1 = a.label_x + Canvas::textWidth(a.callsign, 2), y1 = a.label_y + h2 + (set.altitude ? 2 + lineHeight(1) : 0);
    float lx = x < a.label_x ? a.label_x : x > x1 ? x1 : x, ly = y < a.label_y ? a.label_y : y > y1 ? y1 : y;   // nearest point of the label
    c.line(x, y, lx, ly, label, 110);
  }
  c.text(a.label_x, a.label_y, a.callsign, 2, sel ? COL_SEL : label);
  if (set.altitude) {
    char fl[12];                                                     // room for any int, however odd the data
    snprintf(fl, sizeof fl, "%03d", a.alt_ft / 100);
    c.text(a.label_x, a.label_y + h2 + 2, emerg ? a.squawk : fl, 1, emerg ? COL_EMERG : level);
  }
}

// The card takes the half of the screen the selected aircraft is not in, so it never hides its own aircraft.
static void drawCard(Canvas &c, const Settings &set, const Aircraft &a, bool at_top, const Logo *logo) {
  const int top = at_top ? 0 : 208, bottom = at_top ? 152 : SCREEN_H;
  const int y = at_top ? 34 : top + 9;                               // first text line
  c.blendRect(0, top, SCREEN_W, bottom - top, COL_CARD, 225);
  c.blendRect(30, at_top ? bottom - 1 : top, SCREEN_W - 60, 1, COL_RING, 255);
  char line[48];
  c.textCentred(180, y, a.airline[0] ? a.airline : "Private / unknown", 2, COL_SEL);
  if (a.origin[0] && a.dest[0]) snprintf(line, sizeof line, "%s > %s", a.origin, a.dest);
  else snprintf(line, sizeof line, "no route");
  const int medium = lineHeight(2) + 7, large = lineHeight(3) + 8;   // 23 and 32 with the classic font
  if (logo) {                                                        // the logo sits beside the route, the pair centred
    const int w = Canvas::textWidth(line, 3), x0 = 180 - (LOGO_SIZE + 8 + w) / 2;
    drawLogo(c, *logo, x0, y + medium + lineHeight(3) / 2 - LOGO_SIZE / 2);
    c.text(x0 + LOGO_SIZE + 8, y + medium, line, 3, COL_ROUTE);
  } else c.textCentred(180, y + medium, line, 3, COL_ROUTE);
  snprintf(line, sizeof line, "%s  %s", a.callsign, typeShown(set, a.type));
  c.textCentred(180, y + medium + large, line, 2, COL_LABEL);
  snprintf(line, sizeof line, "FL%03d %dkt %03d\xF8", a.alt_ft / 100, (int)a.speed_kt, (int)a.track_deg);
  c.textCentred(180, y + medium + large + medium, line, 2, COL_LABEL);
}

static void drawClock(Canvas &c, const Radar &r) {
  if (!r.set.clock || r.clock_min < 0) return;
  char hm[32];                                                       // room for any int, however odd the clock
  snprintf(hm, sizeof hm, "%02d:%02d%s", r.clock_min / 60 % 24, r.clock_min % 60, r.clock_utc ? "Z" : "");
  const int w = Canvas::textWidth(hm, 2);
  c.blendRect(180 - w / 2 - 4, CLOCK_Y, w + 8, lineHeight(2) + 4, COL_CARD, 200);   // a backing, so the axis does not run through the digits
  c.textCentred(180, CLOCK_Y + 2, hm, 2, COL_SWEEP);
}

// The notice at the top, on top of everything: an emergency flashes red, a visitor is steady amber.
static void drawNotice(Canvas &c, const Radar &r) {
  if (!r.alert[0]) return;
  const bool visitor = r.alert_style == 1, lit = visitor || r.now_ms % 1000 < 500;
  const Color fill = visitor ? rgb(70, 45, 0) : lit ? rgb(150, 0, 0) : rgb(60, 0, 0), edge = visitor ? COL_BLIP : COL_EMERG;
  c.blendRect(ALERT_X0, ALERT_Y, SCREEN_W - 2 * ALERT_X0, ALERT_H, fill, 235);
  c.blendRect(ALERT_X0, ALERT_Y, SCREEN_W - 2 * ALERT_X0, 1, edge, 255); c.blendRect(ALERT_X0, ALERT_Y + ALERT_H - 1, SCREEN_W - 2 * ALERT_X0, 1, edge, 255);
  c.textCentred(180, ALERT_Y + 4, r.alert, 2, rgb(255, 255, 255));
  c.textCentred(180, ALERT_Y + 6 + lineHeight(2), r.alert2, 1, visitor ? rgb(255, 225, 170) : rgb(255, 200, 190));
}

void Radar::draw(Canvas &c) const {
  setFont(set.font);
  if (page != 0 && pages) {                                          // one of the other pages: see pages.cpp
    drawPage(c, *this, *pages);
    drawClock(c, *this);
    drawNotice(c, *this);
    return;
  }
  const MapImage *underlay = set.map && map && map->ready ? map : nullptr;
  if (cache && cache->valid && cache->key == scopeKey(*this, underlay)) drawScopeCached(c, *cache, set.sweep, sweep_deg);
  else drawScope(c, set, sweep_deg, underlay);
  if (underlay && map_credit[0]) c.textCentred(180, 337, map_credit, 1, rgb(150, 150, 150));

  if (set.sweep) {
    float t = sweep_deg * PI_F / 180.0f;
    c.line(CENTRE, CENTRE, CENTRE + sinf(t) * RADAR_RADIUS, CENTRE - cosf(t) * RADAR_RADIUS, COL_SWEEP);
  }
  char label[20];
  snprintf(label, sizeof label, "%d km", (int)range_km);
  c.text((int)CENTRE + 5, (int)(CENTRE - RADAR_RADIUS) + 6, label, 1, COL_TEXT_DIM);
  c.text((int)CENTRE - 11, (int)(CENTRE - RADAR_RADIUS) + 6, "N", 1, COL_TEXT_DIM);

  layoutLabels();
  const int16_t tag[4] = {tag_x_, tag_y_, tag_w_, tag_h_};
  float x, y;
  for (int i = 0; i < count; i++)
    if (i != selected && project(ac[i], x, y)) drawAircraft(c, *this, ac[i], x, y, false, tag, nullptr);
  c.disc(CENTRE, CENTRE, 2.5f, COL_SWEEP);
  drawClock(c, *this);
  if (selected >= 0 && selected < count && project(ac[selected], x, y)) {
    if (set.card) drawCard(c, set, ac[selected], y > CENTRE, tag_logo_);
    drawAircraft(c, *this, ac[selected], x, y, true, tag, tag_logo_);
  }
  drawNotice(c, *this);
}

// ---- demo traffic (all names made up) ----
// Each flight flies straight, optionally makes one turn of a given radius, then flies straight again.
struct DemoFlight {
  const char *callsign, *airline, *type, *origin, *dest; Shape shape;
  float start_e_km, start_n_km, track, speed_kt; int alt_ft;
  float turn_at_km, turn_deg, turn_radius_km;        // turn_deg > 0 is a right turn; 0 means no turn
};
static const DemoFlight DEMO[] = {
  {"RDR4077", "Radar Air",          "B77W", "LHR", "JFK", Shape::Wide,     30, -18, 285, 470, 34000, 26, 85, 7},
  {"HWK12",   "Hawkeye Airways",    "A320", "EDI", "LGW", Shape::Narrow,   -8, 34, 170, 410, 28000, 30, -60, 9},
  {"KLG5",    "Klinger Regional",   "E190", "AMS", "BRS", Shape::Narrow,   36, 10, 260, 380, 22000, 0, 0, 0},
  {"MSH432",  "Mash Pacific",       "A359", "SIN", "LHR", Shape::Wide,     -30, -26, 40, 300, 9000, 0, 0, 0},
  {"POTTR1",  "",                   "A400", "",    "",    Shape::Military, 12, -32, 350, 280, 14000, 20, 180, 5},
  {"TPR7",    "",                   "C172", "",    "",    Shape::Light,    -14, 6, 95, 105, 2400, 0, 0, 0},
  {"CHOPR1",  "Swamp Air Medical",  "EC35", "",    "",    Shape::Heli,     -24, 20, 135, 120, 1200, 8, 1080, 3},
};

// Position (km east/north of home) and track after flying s km along the flight's path.
static void demoAt(const DemoFlight &d, float s, float &e, float &n, float &track) {
  float t = d.track * PI_F / 180.0f;
  float pre = (d.turn_deg == 0 || s < d.turn_at_km) ? s : d.turn_at_km;
  e = d.start_e_km + sinf(t) * pre; n = d.start_n_km + cosf(t) * pre; track = d.track;
  if (pre == s) return;
  float sign = d.turn_deg > 0 ? 1.0f : -1.0f, total = fabsf(d.turn_deg) * PI_F / 180.0f, R = d.turn_radius_km;
  float along = s - d.turn_at_km, phi = along / R;                // angle turned so far
  if (phi > total) phi = total;
  float ce = e + sign * R * cosf(t), cn = n - sign * R * sinf(t);  // centre of the turn, off the wing
  float t2 = t + sign * phi;
  e = ce - sign * R * cosf(t2); n = cn + sign * R * sinf(t2);
  float after = along - total * R;                                // distance flown since rolling out
  if (after > 0) { e += sinf(t2) * after; n += cosf(t2) * after; }
  track = fmodf(t2 * 180.0f / PI_F + 720.0f, 360.0f);
}

void demoTraffic(Radar &r, uint32_t now_ms) {
  const float loop_km = 110.0f;                                  // each flight repeats after this distance
  const float trail_step_km = 1.2f;
  const double km_per_deg_lon = 111.32 * cos(r.home_lat * 3.14159265358979 / 180.0);
  r.count = (int)(sizeof DEMO / sizeof DEMO[0]);
  for (int i = 0; i < r.count; i++) {
    const DemoFlight &d = DEMO[i];
    Aircraft &a = r.ac[i];
    snprintf(a.callsign, sizeof a.callsign, "%s", d.callsign);
    snprintf(a.airline, sizeof a.airline, "%s", d.airline);
    airlineFromCallsign(d.callsign, a.airline_icao);
    snprintf(a.type, sizeof a.type, "%s", d.type);
    snprintf(a.origin, sizeof a.origin, "%s", d.origin);
    snprintf(a.dest, sizeof a.dest, "%s", d.dest);
    a.shape = d.shape;
    float km = fmodf(d.speed_kt * 1.852f * (now_ms / 3600000.0f) * 20.0f, loop_km);   // 20x faster than life
    float e, n, trk;
    demoAt(d, km, e, n, trk);
    a.lat = r.home_lat + n / 110.57; a.lon = r.home_lon + e / km_per_deg_lon;
    a.alt_ft = d.alt_ft; a.track_deg = trk; a.speed_kt = d.speed_kt;
    a.live = false; a.on_ground = false;
    snprintf(a.hex, sizeof a.hex, "d%04x", (unsigned)i & 0xffffu);   // made-up address, unique per demo flight
    a.trail_len = 0;
    for (int k = 1; k <= TRAIL_POINTS; k++) {
      float back = km - k * trail_step_km;
      if (back < 0) break;                                        // no trail from before the flight (re)appeared
      demoAt(d, back, e, n, trk);
      a.trail_lat[a.trail_len] = (float)(r.home_lat + n / 110.57);
      a.trail_lon[a.trail_len] = (float)(r.home_lon + e / km_per_deg_lon);
      a.trail_len++;
    }
  }
  if (r.selected >= r.count) r.selected = -1;
}

}  // namespace rm
