// RadarMASH - open firmware for the GeekMagic Radar 360.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A very small JSON reader: no allocation, no copying, safe on truncated or hostile input. It walks the text in place.
// Enough to pick fields out of the aircraft feeds; not a general-purpose library.
#pragma once
#include <stddef.h>

namespace rm {
namespace json {

// A value's extent within the text. ok() is false when the text was malformed or cut short.
struct Span {
  const char *p = nullptr, *end = nullptr;
  bool ok() const { return p != nullptr && end != nullptr; }
};

Span parse(const char *text, size_t len);                    // the top-level value
bool member(Span object, const char *key, Span &out);        // object["key"]
bool element(Span array, const char *&iter, Span &out);      // next array element; start with iter = nullptr
bool number(Span v, double &out);
bool string(Span v, char *out, size_t out_size);             // unescaped, cut to fit, accents reduced to plain letters
bool isObject(Span v);
bool isArray(Span v);

}  // namespace json
}  // namespace rm
