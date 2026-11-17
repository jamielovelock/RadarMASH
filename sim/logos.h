// RadarMASH - open firmware for the GeekMagic Radar 360.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Simulator logo source: downloads airline logos with curl from the address pattern in the settings, keeps the files in
// ~/.cache/radarmash, and hands the radar the loaded ones. Fetching happens on the live-data thread, never while drawing.
#pragma once
#include "../core/logo.h"
#include <map>
#include <memory>
#include <mutex>
#include <string>

struct SimLogos : rm::LogoSource {
  std::string url_template;     // e.g. https://example.org/logos/{icao}.png; empty means no logos
  std::string user_agent;
  bool verify_certs = true;
  int fetched = 0, missing = 0;

  const rm::Logo *logo(const char *airline_icao) override;   // call with the radar lock held
  void fetchPending(std::mutex &mu);                         // call from the background thread, without the lock

private:
  struct Entry { std::unique_ptr<rm::Logo> logo; bool tried = false; std::string for_url; };
  std::map<std::string, Entry> entries_;
};
