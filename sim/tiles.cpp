// RadarMASH - open firmware for the GeekMagic Radar 360.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "tiles.h"
#include "third_party/stb_image.h"
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static const long KEEP_SECONDS = 30L * 24 * 3600;      // tiles on disk are reused for 30 days (servers ask for at least 7)
static const long RETRY_SECONDS = 60;

static void replaceAll(std::string &s, const std::string &from, const std::string &to) {
  for (size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + to.size())) s.replace(p, from.size(), to);
}

static void makeDirs(const std::string &path) {
  for (size_t p = 1; p <= path.size(); p++)
    if (p == path.size() || path[p] == '/') mkdir(path.substr(0, p).c_str(), 0755);
}

std::string SimTiles::cacheDir() const {
  unsigned long h = 1469598103934665603UL;             // one folder per source, so changing source never mixes tiles
  for (unsigned char ch : url_template) h = (h ^ ch) * 1099511628211UL;
  const char *home = getenv("HOME");
  char buf[32];
  snprintf(buf, sizeof buf, "%016lx", h);
  return std::string(home ? home : ".") + "/.cache/radarmash/tiles/" + buf;
}

// Run curl directly (no shell), so nothing in the URL can be read as a command.
static bool download(const std::string &url, const std::string &ua, const std::string &dest, bool verify) {
  std::string tmp = dest + ".part";
  const char *argv[] = {"curl", verify ? "-fsSL" : "-fsSLk", "--max-time", "15", "-A", ua.c_str(), "-o", tmp.c_str(), "--", url.c_str(), nullptr};
  pid_t pid;
  if (posix_spawnp(&pid, "curl", nullptr, nullptr, (char *const *)argv, environ) != 0) return false;
  int status = 0;
  waitpid(pid, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) { unlink(tmp.c_str()); return false; }
  return rename(tmp.c_str(), dest.c_str()) == 0;
}

const uint16_t *SimTiles::tile(int zoom, int x, int y) {
  if (url_template.compare(0, 8, "https://") != 0 && url_template.compare(0, 7, "http://") != 0) return nullptr;
  char name[64];
  snprintf(name, sizeof name, "/%d/%d/%d", zoom, x, y);
  std::string key = url_template + name;
  auto hit = decoded_.find(key);
  if (hit != decoded_.end()) return hit->second.data();

  long now = (long)time(nullptr);
  auto bad = failed_at_.find(key);
  if (bad != failed_at_.end() && now - bad->second < RETRY_SECONDS) return nullptr;

  char dir[32];
  snprintf(dir, sizeof dir, "/%d/%d", zoom, x);
  std::string folder = cacheDir() + dir, path = folder + "/" + std::to_string(y) + ".img";
  struct stat st;
  bool have = stat(path.c_str(), &st) == 0 && st.st_size > 0;
  if (!have || now - (long)st.st_mtime > KEEP_SECONDS) {
    std::string url = url_template;
    replaceAll(url, "{z}", std::to_string(zoom)); replaceAll(url, "{x}", std::to_string(x)); replaceAll(url, "{y}", std::to_string(y));
    replaceAll(url, "{s}", "a");
    makeDirs(folder);
    if (download(url, user_agent, path, verify_certs)) { fetched++; have = true; }
    else { failed++; if (!have) { failed_at_[key] = now; return nullptr; } }    // an old copy is better than nothing
  }

  int w = 0, h = 0, n = 0;
  unsigned char *rgbpx = stbi_load(path.c_str(), &w, &h, &n, 3);
  if (!rgbpx || w != rm::TILE_SIZE || h != rm::TILE_SIZE) {
    if (rgbpx) stbi_image_free(rgbpx);
    unlink(path.c_str());
    failed_at_[key] = now;
    return nullptr;
  }
  if (decoded_.size() >= 32) decoded_.clear();
  std::vector<uint16_t> &out = decoded_[key];
  out.resize((size_t)rm::TILE_SIZE * rm::TILE_SIZE);
  for (int i = 0; i < rm::TILE_SIZE * rm::TILE_SIZE; i++) out[i] = rm::rgb(rgbpx[i * 3], rgbpx[i * 3 + 1], rgbpx[i * 3 + 2]);
  stbi_image_free(rgbpx);
  return out.data();
}
