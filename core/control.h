// RadarMASH - open firmware for the GeekMagic Radar 360.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The ways of telling the radar what to do: its one button, and one-line requests from elsewhere on the network (a
// Stream Deck key, a home-automation rule, curl). Shared by the device and the simulator so both behave the same.
#pragma once
#include "alert.h"
#include "pages.h"
#include <stddef.h>

namespace rm {

// ---- the button ----
// One press: next aircraft (or answer an emergency notice, or go back to the scope from another page).
// Two quick presses: next page. Hold: next range. Five quick presses: forget the control key.
enum class Press : uint8_t { None, Short, Double, Long, Reset };

struct Button {
  // Call often with the raw state of the button. Returns what the user has just finished doing, usually None.
  // A single press is reported about a third of a second late: that is how long it waits for a second one.
  Press feed(bool pressed, uint32_t now_ms);
  uint32_t heldMs(uint32_t now_ms) const { return down_ ? now_ms - down_ms_ : 0; }

private:
  bool raw_ = false, down_ = false, long_done_ = false;
  uint32_t change_ms_ = 0, down_ms_ = 0, up_ms_ = 0;
  uint8_t taps_ = 0;
};

void setPage(Radar &r, Page p, uint32_t now_ms);
// Do what a press means. Reset is left to the caller (it concerns the saved settings).
void doPress(Press p, Radar &r, Watch &w, uint32_t now_ms);
// Once a frame: moves to the next page every `cycle_s` seconds, if that is wanted (0: stay put).
void pageTick(Radar &r, const Watch &w, int cycle_s, uint32_t now_ms);

// ---- one-line requests ----
// GET /api/do?page=weather&brightness=40 ... Each name=value pair goes through here.
enum class Cmd : uint8_t {
  Done,       // carried out
  Settings,   // `json` now holds a settings change for the caller to apply and save, e.g. {"clock":false}
  Unknown,    // no such name
  Bad,        // the name is fine, the value is not
};
Cmd command(const char *name, const char *value, Radar &r, Watch &w, uint32_t now_ms, char *json, size_t json_size);

// The optional shared key. With none configured everything is allowed; otherwise the caller must give the same one.
bool keyOk(const char *configured, const char *given);

}  // namespace rm
