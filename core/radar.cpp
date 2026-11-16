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

static const Color COL_RING = rgb(0, 120, 60), COL_TEXT_DIM = rgb(0, 150, 80);
static const Color COL_BLIP = rgb(255, 190, 60), COL_LABEL = rgb(230, 230, 230), COL_SEL = rgb(255, 240, 80);
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

void Radar::tick(uint32_t now_ms) { sweep_deg = fmodf((float)now_ms, SWEEP_PERIOD_MS) / SWEEP_PERIOD_MS * 360.0f; }

bool Radar::project(const Aircraft &a, float &x, float &y) const {
  // flat-earth approximation around home; fine at radar ranges
  float east_km = (float)((a.lon - home_lon) * 111.32 * cos(home_lat * 3.14159265358979 / 180.0));
  float north_km = (float)((a.lat - home_lat) * 110.57);
  float k = RADAR_RADIUS / range_km;
  x = CENTRE + east_km * k;
  y = CENTRE - north_km * k;
  float dx = x - CENTRE, dy = y - CENTRE;
  return dx * dx + dy * dy <= (RADAR_RADIUS - 4) * (RADAR_RADIUS - 4);
}

void Radar::shortPress() {
  float x, y;
  for (int i = selected + 1; i < count; i++)
    if (project(ac[i], x, y)) { selected = i; return; }
  selected = -1;
}

void Radar::longPress() { range_km = range_km < 30 ? 40 : range_km < 60 ? 80 : 20; selected = -1; }

// Background: rings, axes and the sweep trail, computed per pixel so the rings and the trail are smooth.
static void drawScope(Canvas &c, float sweep_deg) {
  for (int y = c.y0; y < c.y0 + c.rows; y++) {
    uint16_t *row = c.row(y);
    float dy = y - CENTRE;
    for (int x = 0; x < SCREEN_W; x++) {
      float dx = x - CENTRE;
      float r = sqrtf(dx * dx + dy * dy);
      if (r > RADAR_RADIUS + 1.0f) { row[x] = 0; continue; }
      float g = 10.0f;                                             // faint scope glow
      for (int k = 1; k <= 3; k++) {
        float d = fabsf(r - RADAR_RADIUS * k / 3.0f);
        if (d < 1.0f) g += (1.0f - d) * 110.0f;
      }
      if (fabsf(dx) < 0.6f || fabsf(dy) < 0.6f) g += 35.0f;
      float bearing = fastAtan2(dx, -dy) * 180.0f / PI_F;          // 0 = up, clockwise
      float behind = sweep_deg - bearing;
      if (behind < 0) behind += 360.0f;
      if (behind >= 360.0f) behind -= 360.0f;
      if (behind < TRAIL_DEG && r > 2.0f) { float t = 1.0f - behind / TRAIL_DEG; g += t * t * 120.0f; }
      if (g > 255.0f) g = 255.0f;
      row[x] = rgb((int)(g * 0.10f), (int)g, (int)(g * 0.45f));
    }
  }
}

static void drawAircraft(Canvas &c, const Aircraft &a, float x, float y, bool sel) {
  float t = a.track_deg * PI_F / 180.0f, sx = sinf(t), cy = -cosf(t);       // unit vector along track
  float px = -cy, py = sx;                                                   // perpendicular
  Color col = sel ? COL_SEL : COL_BLIP;
  c.triangle(x + sx * 8, y + cy * 8, x - sx * 5 + px * 5, y - cy * 5 + py * 5, x - sx * 5 - px * 5, y - cy * 5 - py * 5, col);
  if (sel) c.ring(x, y, 13, COL_SEL);
  // label to the right, or to the left when that would run off the scope
  int w = Canvas::textWidth(a.callsign, 2);
  int lx = (x + 12 + w < CENTRE + RADAR_RADIUS - 8) ? (int)x + 12 : (int)x - 12 - w;
  c.text(lx, (int)y - 16, a.callsign, 2, sel ? COL_SEL : COL_LABEL);
  char fl[8];
  snprintf(fl, sizeof fl, "%03d", a.alt_ft / 100);
  c.text(lx, (int)y + 2, fl, 1, COL_TEXT_DIM);
}

static void drawCard(Canvas &c, const Aircraft &a) {
  c.blendRect(0, 236, SCREEN_W, 124, COL_CARD, 215);
  c.blendRect(40, 236, SCREEN_W - 80, 1, COL_RING, 255);
  c.textCentred(180, 246, a.callsign, 3, COL_SEL);
  char line[40];
  if (a.origin[0] && a.dest[0]) snprintf(line, sizeof line, "%s > %s", a.origin, a.dest);
  else snprintf(line, sizeof line, "route unknown");
  c.textCentred(180, 278, line, 2, COL_ROUTE);
  snprintf(line, sizeof line, "%s FL%03d %dkt", a.type, a.alt_ft / 100, (int)a.speed_kt);
  c.textCentred(180, 302, line, 2, COL_LABEL);
}

void Radar::draw(Canvas &c) const {
  drawScope(c, sweep_deg);

  float t = sweep_deg * PI_F / 180.0f;
  c.line(CENTRE, CENTRE, CENTRE + sinf(t) * RADAR_RADIUS, CENTRE - cosf(t) * RADAR_RADIUS, rgb(140, 255, 190));

  char label[12];
  snprintf(label, sizeof label, "%d km", (int)range_km);
  c.text((int)CENTRE + 5, (int)(CENTRE - RADAR_RADIUS) + 6, label, 1, COL_TEXT_DIM);
  c.text((int)CENTRE - 11, (int)(CENTRE - RADAR_RADIUS) + 6, "N", 1, COL_TEXT_DIM);

  float x, y;
  for (int i = 0; i < count; i++)
    if (i != selected && project(ac[i], x, y)) drawAircraft(c, ac[i], x, y, false);
  if (selected >= 0 && selected < count && project(ac[selected], x, y)) {
    drawAircraft(c, ac[selected], x, y, true);
    drawCard(c, ac[selected]);
  }
  c.disc(CENTRE, CENTRE, 2.5f, rgb(140, 255, 190));
}

// ---- demo traffic ----
struct DemoFlight { const char *callsign, *type, *origin, *dest; float start_e_km, start_n_km, track, speed_kt; int alt_ft; };
static const DemoFlight DEMO[] = {
  {"RDR4077", "B77W", "LHR", "JFK", 30, -18, 285, 470, 34000},
  {"HWK12",   "A320", "EDI", "LGW", -8, 34, 170, 410, 28000},
  {"KLG5",    "E190", "AMS", "BRS", 36, 10, 260, 380, 22000},
  {"MSH432",  "A359", "SIN", "LHR", -30, -26, 40, 300, 9000},
  {"HNC88",   "DH8D", "",    "",    12, -32, 350, 250, 14000},
  {"TPR7",    "C172", "",    "",    -14, 6, 95, 105, 2400},
};

void demoTraffic(Radar &r, uint32_t now_ms) {
  r.home_lat = 51.0; r.home_lon = -1.0;                          // nowhere in particular
  const float loop_km = 110.0f;                                  // each flight repeats after this distance
  r.count = (int)(sizeof DEMO / sizeof DEMO[0]);
  for (int i = 0; i < r.count; i++) {
    const DemoFlight &d = DEMO[i];
    Aircraft &a = r.ac[i];
    snprintf(a.callsign, sizeof a.callsign, "%s", d.callsign);
    snprintf(a.type, sizeof a.type, "%s", d.type);
    snprintf(a.origin, sizeof a.origin, "%s", d.origin);
    snprintf(a.dest, sizeof a.dest, "%s", d.dest);
    float km = fmodf(d.speed_kt * 1.852f * (now_ms / 3600000.0f) * 20.0f, loop_km);   // 20x faster than life
    float t = d.track * PI_F / 180.0f;
    float e = d.start_e_km + sinf(t) * km, n = d.start_n_km + cosf(t) * km;
    a.lat = r.home_lat + n / 110.57;
    a.lon = r.home_lon + e / (111.32 * cos(r.home_lat * 3.14159265358979 / 180.0));
    a.alt_ft = d.alt_ft; a.track_deg = d.track; a.speed_kt = d.speed_kt;
  }
  if (r.selected >= r.count) r.selected = -1;
}

}  // namespace rm
