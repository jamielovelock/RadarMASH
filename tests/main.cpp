// RadarMASH - open firmware for the GeekMagic Radar 360.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "test.h"
#include <string.h>

// Runs every TEST, or only those whose name contains the first argument.
int main(int argc, char **argv) {
  const char *filter = argc > 1 ? argv[1] : "";
  int ran = 0, failed_cases = 0;
  for (t::Case *c = t::head(); c; c = c->next) {
    if (!strstr(c->name, filter)) continue;
    int before = t::failures();
    c->fn();
    ran++;
    if (t::failures() != before) { failed_cases++; printf("FAILED  %s\n", c->name); }
  }
  printf("%d tests, %d checks, %d failed\n", ran, t::checks(), failed_cases);
  return failed_cases || !ran ? 1 : 0;
}
