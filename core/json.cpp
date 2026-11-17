// RadarMASH - open firmware for the GeekMagic Radar 360.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "json.h"
#include <stdlib.h>
#include <string.h>

namespace rm {
namespace json {

static const char *ws(const char *p, const char *end) {
  while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
  return p;
}

// End of the string starting at the opening quote p, or nullptr if it never closes.
static const char *skipString(const char *p, const char *end) {
  for (p++; p < end; p++) {
    if (*p == '\\') { p++; continue; }
    if (*p == '"') return p + 1;
  }
  return nullptr;
}

// End of the value starting at p, or nullptr. Loops rather than recursing, so deep nesting cannot exhaust the stack.
static const char *skipValue(const char *p, const char *end) {
  p = ws(p, end);
  if (p >= end) return nullptr;
  if (*p == '"') return skipString(p, end);
  if (*p == '{' || *p == '[') {
    int depth = 0;
    while (p < end) {
      char ch = *p;
      if (ch == '"') { p = skipString(p, end); if (!p) return nullptr; continue; }
      if (ch == '{' || ch == '[') depth++;
      else if (ch == '}' || ch == ']') { if (--depth == 0) return p + 1; }
      p++;
    }
    return nullptr;
  }
  const char *s = p;
  while (p < end && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t') p++;
  return p > s ? p : nullptr;
}

static Span valueAt(const char *p, const char *end) {
  Span v;
  p = ws(p, end);
  const char *e = skipValue(p, end);
  if (e) { v.p = p; v.end = e; }
  return v;
}

Span parse(const char *text, size_t len) { return text ? valueAt(text, text + len) : Span(); }
bool isObject(Span v) { return v.ok() && *v.p == '{'; }
bool isArray(Span v) { return v.ok() && *v.p == '['; }

bool member(Span obj, const char *key, Span &out) {
  if (!isObject(obj)) return false;
  size_t klen = strlen(key);
  const char *p = obj.p + 1, *end = obj.end;
  for (;;) {
    p = ws(p, end);
    if (p >= end || *p != '"') return false;                  // includes the closing brace
    const char *kend = skipString(p, end);
    if (!kend) return false;
    bool match = (size_t)(kend - p - 2) == klen && memcmp(p + 1, key, klen) == 0;
    p = ws(kend, end);
    if (p >= end || *p != ':') return false;
    Span v = valueAt(p + 1, end);
    if (!v.ok()) return false;
    if (match) { out = v; return true; }
    p = ws(v.end, end);
    if (p < end && *p == ',') p++;
  }
}

bool element(Span arr, const char *&iter, Span &out) {
  if (!isArray(arr)) return false;
  const char *p = iter ? iter : arr.p + 1;
  p = ws(p, arr.end);
  if (p >= arr.end || *p == ']') return false;
  Span v = valueAt(p, arr.end);
  if (!v.ok()) return false;
  out = v;
  p = ws(v.end, arr.end);
  if (p < arr.end && *p == ',') p++;
  iter = p;
  return true;
}

bool number(Span v, double &out) {
  if (!v.ok() || !(*v.p == '-' || (*v.p >= '0' && *v.p <= '9'))) return false;
  char buf[40];
  size_t n = (size_t)(v.end - v.p);
  if (n >= sizeof buf) return false;
  memcpy(buf, v.p, n); buf[n] = 0;
  char *stop = nullptr;
  double d = strtod(buf, &stop);
  if (stop != buf + n) return false;
  out = d;
  return true;
}

bool boolean(Span v, bool &out) {
  if (!v.ok()) return false;
  size_t n = (size_t)(v.end - v.p);
  if (n == 4 && memcmp(v.p, "true", 4) == 0) { out = true; return true; }
  if (n == 5 && memcmp(v.p, "false", 5) == 0) { out = false; return true; }
  return false;
}

// Western European accented letters, U+00C0..U+00FF, as their nearest plain letter. The screen font has no accents.
static const char PLAIN[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";

static char plainFor(unsigned cp) { return cp < 0x80 ? (char)cp : (cp >= 0xC0 && cp <= 0xFF) ? PLAIN[cp - 0xC0] : '?'; }

bool string(Span v, char *out, size_t n) {
  if (!v.ok() || *v.p != '"' || n == 0) return false;
  const char *p = v.p + 1, *end = v.end - 1;                  // inside the quotes
  size_t o = 0;
  while (p < end) {
    unsigned cp;
    unsigned char ch = (unsigned char)*p++;
    if (ch == '\\' && p < end) {
      char e = *p++;
      if (e == 'u') {
        cp = 0;
        for (int i = 0; i < 4 && p < end; i++, p++) {
          char h = *p;
          cp = cp * 16 + (unsigned)(h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : 0);
        }
      } else cp = e == 'n' || e == 'r' || e == 't' || e == 'b' || e == 'f' ? ' ' : (unsigned char)e;
    } else if (ch < 0x80) cp = ch;
    else {                                                    // UTF-8: gather the continuation bytes
      int extra = ch >= 0xF0 ? 3 : ch >= 0xE0 ? 2 : ch >= 0xC0 ? 1 : 0;
      cp = extra == 3 ? ch & 0x07 : extra == 2 ? ch & 0x0F : extra == 1 ? ch & 0x1F : '?';
      for (; extra > 0 && p < end && ((unsigned char)*p & 0xC0) == 0x80; extra--, p++) cp = (cp << 6) | ((unsigned char)*p & 0x3F);
    }
    char plain = plainFor(cp);
    if (plain < 32) plain = ' ';
    if (o + 1 < n) out[o++] = plain;
  }
  out[o] = 0;
  return true;
}

}  // namespace json
}  // namespace rm
