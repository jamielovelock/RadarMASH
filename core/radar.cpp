// RadarMASH - open firmware for the GeekMagic Radar 360.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "radar.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace rm {

static const float PI_F = 3.14159265f;
static const float SWEEP_PERIOD_MS = 4000.0f;
static const float TRAIL_DEG = 100.0f;
static const uint32_t CYCLE_MS = 5000;

static const Color COL_RING = rgb(0, 120, 60), COL_TEXT_DIM = rgb(0, 150, 80), COL_SWEEP = rgb(140, 255, 190);
static const Color COL_BLIP = rgb(255, 190, 60), COL_MIL = rgb(255, 110, 90), COL_LABEL = rgb(230, 230, 230), COL_SEL = rgb(255, 240, 80);
static const Color COL_CARD = rgb(0, 14, 8), COL_ROUTE = rgb(120, 220, 255);

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

void Radar::tick(uint32_t t) {
  now_ms = t;
  sweep_deg = fmodf((float)t, SWEEP_PERIOD_MS) / SWEEP_PERIOD_MS * 360.0f;
  float x, y;
  bool gone = selected >= 0 && (selected >= count || !project(ac[selected], x, y));   // flew out of view
  if (set.auto_cycle && (selected < 0 || gone || t - last_cycle_ms >= CYCLE_MS)) {
    selected = nextInView(*this, selected, true);
    last_cycle_ms = t;
  } else if (gone) selected = -1;
}

void Radar::toScreen(double lat, double lon, float &x, float &y) const {
  // flat-earth approximation around home; fine at radar ranges
  float east_km = (float)((lon - home_lon) * 111.32 * cos(home_lat * 3.14159265358979 / 180.0));
  float north_km = (float)((lat - home_lat) * 110.57);
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
static void drawScope(Canvas &c, const Settings &set, float sweep_deg, const MapImage *map) {
  const int bright = set.map_brightness > 100 ? 100 : set.map_brightness;
  for (int y = c.y0; y < c.y0 + c.rows; y++) {
    uint16_t *row = c.row(y);
    float dy = y - CENTRE;
    for (int x = 0; x < SCREEN_W; x++) {
      float dx = x - CENTRE;
      float r = sqrtf(dx * dx + dy * dy);
      if (r > RADAR_RADIUS + 1.0f) { row[x] = 0; continue; }
      float g = 10.0f;                                             // faint scope glow
      if (set.rings) {
        for (int k = 1; k <= 3; k++) {
          float d = fabsf(r - RADAR_RADIUS * k / 3.0f);
          if (d < 1.0f) g += (1.0f - d) * 110.0f;
        }
        if (fabsf(dx) < 0.6f || fabsf(dy) < 0.6f) g += 35.0f;
      } else {
        float d = fabsf(r - RADAR_RADIUS);                         // always keep the outer edge
        if (d < 1.0f) g += (1.0f - d) * 110.0f;
      }
      if (set.sweep && r > 2.0f) {
        float bearing = fastAtan2(dx, -dy) * 180.0f / PI_F;        // 0 = up, clockwise
        float behind = sweep_deg - bearing;
        if (behind < 0) behind += 360.0f;
        if (behind >= 360.0f) behind -= 360.0f;
        if (behind < TRAIL_DEG) { float t = 1.0f - behind / TRAIL_DEG; g += t * t * 120.0f; }
      }
      int r8 = (int)(g * 0.10f), g8 = (int)g, b8 = (int)(g * 0.45f);
      if (map) {                                                   // scope glow is added on top of the dimmed map
        uint16_t m = map->px[y * SCREEN_W + x];
        r8 += ((m >> 11) << 3) * bright / 100; g8 += (((m >> 5) & 63) << 2) * bright / 100; b8 += ((m & 31) << 3) * bright / 100;
      }
      row[x] = rgb(r8 > 255 ? 255 : r8, g8 > 255 ? 255 : g8, b8 > 255 ? 255 : b8);
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

static void drawTris(Canvas &c, const Tri *t, int n, float x, float y, float track_deg, float size, Color col) {
  float a = track_deg * PI_F / 180.0f, co = cosf(a) * size, si = sinf(a) * size;
  for (int i = 0; i < n; i++)
    c.triangle(x + t[i].ax * co - t[i].ay * si, y + t[i].ax * si + t[i].ay * co,
               x + t[i].bx * co - t[i].by * si, y + t[i].bx * si + t[i].by * co,
               x + t[i].cx * co - t[i].cy * si, y + t[i].cx * si + t[i].cy * co, col);
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

// Small box beside the highlighted aircraft: callsign, then whichever extra lines the settings ask for.
static void drawTag(Canvas &c, const Settings &set, const Aircraft &a, float x, float y) {
  char route[16] = "", data[32] = "";
  if (set.tag_route && a.origin[0] && a.dest[0]) snprintf(route, sizeof route, "%s>%s", a.origin, a.dest);
  int n = 0;
  if (set.tag_type && a.type[0]) n += snprintf(data + n, sizeof data - n, "%s ", a.type);
  if (set.tag_level) n += snprintf(data + n, sizeof data - n, "FL%03d ", a.alt_ft / 100);
  if (set.tag_speed) n += snprintf(data + n, sizeof data - n, "%dkt ", (int)a.speed_kt);
  if (n) data[n - 1] = 0;                                            // drop the trailing space

  int w = Canvas::textWidth(a.callsign, 2), h = 16;
  if (route[0]) { int rw = Canvas::textWidth(route, 2); if (rw > w) w = rw; h += 19; }
  if (data[0]) { int dw = Canvas::textWidth(data, 1); if (dw > w) w = dw; h += 11; }
  w += 10; h += 8;

  // try right, left, below, above; take the first place where the whole box is inside the scope
  const int gap = 20;
  const int cand[4][2] = {{(int)x + gap, (int)y - h / 2}, {(int)x - gap - w, (int)y - h / 2},
                          {(int)x - w / 2, (int)y + gap}, {(int)x - w / 2, (int)y - gap - h}};
  int bx = cand[0][0], by = cand[0][1];
  for (int k = 0; k < 4; k++) {
    int px = cand[k][0], py = cand[k][1];
    if (inScope(px, py, 3) && inScope(px + w, py, 3) && inScope(px, py + h, 3) && inScope(px + w, py + h, 3)) { bx = px; by = py; break; }
  }
  c.blendRect(bx, by, w, h, COL_CARD, 225);
  c.blendRect(bx, by, w, 1, COL_SEL, 255); c.blendRect(bx, by + h - 1, w, 1, COL_SEL, 255);
  c.blendRect(bx, by, 1, h, COL_SEL, 255); c.blendRect(bx + w - 1, by, 1, h, COL_SEL, 255);
  int ty = by + 5;
  c.text(bx + 5, ty, a.callsign, 2, COL_SEL); ty += 19;
  if (route[0]) { c.text(bx + 5, ty, route, 2, COL_ROUTE); ty += 19; }
  if (data[0]) c.text(bx + 5, ty, data, 1, COL_LABEL);
}

static void drawAircraft(Canvas &c, const Radar &r, const Aircraft &a, float x, float y, bool sel) {
  const Settings &set = r.set;
  Color col = sel ? COL_SEL : a.shape == Shape::Military ? COL_MIL : COL_BLIP;
  Color label = COL_LABEL, level = COL_TEXT_DIM;
  if (set.fade && set.sweep && !sel) {                               // the highlighted aircraft always stays bright
    float f = sweepFade(x, y, r.sweep_deg);
    col = dim(col, f); label = dim(label, f); level = dim(level, f);
  }
  if (set.trails) drawTrail(c, r, a, x, y, col);
  drawIcon(c, set.shapes ? a.shape : Shape::Unknown, x, y, a.track_deg, col);
  if (sel) c.ring(x, y, 17, COL_SEL);
  if (sel && set.tag) { drawTag(c, set, a, x, y); return; }
  if (!set.labels && !sel) return;
  // label to the right, or to the left when that would run off the scope
  int w = Canvas::textWidth(a.callsign, 2);
  int lx = (x + 16 + w < CENTRE + RADAR_RADIUS - 8) ? (int)x + 16 : (int)x - 16 - w;
  c.text(lx, (int)y - 16, a.callsign, 2, sel ? COL_SEL : label);
  if (set.altitude) {
    char fl[12];                                                     // room for any int, however odd the data
    snprintf(fl, sizeof fl, "%03d", a.alt_ft / 100);
    c.text(lx, (int)y + 2, fl, 1, level);
  }
}

// The card takes the half of the screen the selected aircraft is not in, so it never hides its own aircraft.
static void drawCard(Canvas &c, const Aircraft &a, bool at_top) {
  const int top = at_top ? 0 : 208, bottom = at_top ? 152 : SCREEN_H;
  const int y = at_top ? 34 : top + 9;                               // first text line
  c.blendRect(0, top, SCREEN_W, bottom - top, COL_CARD, 225);
  c.blendRect(30, at_top ? bottom - 1 : top, SCREEN_W - 60, 1, COL_RING, 255);
  char line[48];
  c.textCentred(180, y, a.airline[0] ? a.airline : "Private / unknown", 2, COL_SEL);
  if (a.origin[0] && a.dest[0]) snprintf(line, sizeof line, "%s > %s", a.origin, a.dest);
  else snprintf(line, sizeof line, "no route");
  c.textCentred(180, y + 23, line, 3, COL_ROUTE);
  snprintf(line, sizeof line, "%s  %s", a.callsign, a.type);
  c.textCentred(180, y + 55, line, 2, COL_LABEL);
  snprintf(line, sizeof line, "FL%03d %dkt %03d\xF8", a.alt_ft / 100, (int)a.speed_kt, (int)a.track_deg);
  c.textCentred(180, y + 78, line, 2, COL_LABEL);
}

void Radar::draw(Canvas &c) const {
  const MapImage *underlay = set.map && map && map->ready ? map : nullptr;
  drawScope(c, set, sweep_deg, underlay);
  if (underlay && map_credit[0]) c.textCentred(180, 337, map_credit, 1, rgb(150, 150, 150));

  if (set.sweep) {
    float t = sweep_deg * PI_F / 180.0f;
    c.line(CENTRE, CENTRE, CENTRE + sinf(t) * RADAR_RADIUS, CENTRE - cosf(t) * RADAR_RADIUS, COL_SWEEP);
  }
  char label[20];
  snprintf(label, sizeof label, "%d km", (int)range_km);
  c.text((int)CENTRE + 5, (int)(CENTRE - RADAR_RADIUS) + 6, label, 1, COL_TEXT_DIM);
  c.text((int)CENTRE - 11, (int)(CENTRE - RADAR_RADIUS) + 6, "N", 1, COL_TEXT_DIM);

  float x, y;
  for (int i = 0; i < count; i++)
    if (i != selected && project(ac[i], x, y)) drawAircraft(c, *this, ac[i], x, y, false);
  c.disc(CENTRE, CENTRE, 2.5f, COL_SWEEP);
  if (selected >= 0 && selected < count && project(ac[selected], x, y)) {
    if (set.card) drawCard(c, ac[selected], y > CENTRE);
    drawAircraft(c, *this, ac[selected], x, y, true);
  }
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
  r.home_lat = 51.0; r.home_lon = -1.0;                          // nowhere in particular
  const float loop_km = 110.0f;                                  // each flight repeats after this distance
  const float trail_step_km = 1.2f;
  const double km_per_deg_lon = 111.32 * cos(r.home_lat * 3.14159265358979 / 180.0);
  r.count = (int)(sizeof DEMO / sizeof DEMO[0]);
  for (int i = 0; i < r.count; i++) {
    const DemoFlight &d = DEMO[i];
    Aircraft &a = r.ac[i];
    snprintf(a.callsign, sizeof a.callsign, "%s", d.callsign);
    snprintf(a.airline, sizeof a.airline, "%s", d.airline);
    snprintf(a.type, sizeof a.type, "%s", d.type);
    snprintf(a.origin, sizeof a.origin, "%s", d.origin);
    snprintf(a.dest, sizeof a.dest, "%s", d.dest);
    a.shape = d.shape;
    float km = fmodf(d.speed_kt * 1.852f * (now_ms / 3600000.0f) * 20.0f, loop_km);   // 20x faster than life
    float e, n, trk;
    demoAt(d, km, e, n, trk);
    a.lat = r.home_lat + n / 110.57; a.lon = r.home_lon + e / km_per_deg_lon;
    a.alt_ft = d.alt_ft; a.track_deg = trk; a.speed_kt = d.speed_kt;
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
