// RadarMASH - open firmware for the GeekMagic Radar 360.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A very small test harness, so the tests need nothing but a C++ compiler.
//   TEST(name) { CHECK(condition); CHECK_NEAR(a, b, tolerance); }
#pragma once
#include <math.h>
#include <stdio.h>

namespace t {
struct Case { const char *name; void (*fn)(); Case *next; };
inline Case *&head() { static Case *h = nullptr; return h; }
inline int &failures() { static int f = 0; return f; }
inline int &checks() { static int c = 0; return c; }
struct Reg { Case c; Reg(const char *n, void (*f)()) : c{n, f, head()} { head() = &c; } };
}  // namespace t

#define TEST(name) static void name(); static t::Reg reg_##name(#name, name); static void name()
#define CHECK(cond) do { t::checks()++; if (!(cond)) { t::failures()++; printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_NEAR(a, b, eps) do { t::checks()++; double va_ = (a), vb_ = (b); if (!(fabs(va_ - vb_) <= (eps))) { t::failures()++; \
  printf("    FAIL %s:%d: %s = %g, expected %g +/- %g\n", __FILE__, __LINE__, #a, va_, vb_, (double)(eps)); } } while (0)
