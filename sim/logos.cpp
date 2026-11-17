// RadarMASH - open firmware for the GeekMagic Radar 360.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "logos.h"
#include "third_party/stb_image.h"
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char **environ;

const rm::Logo *SimLogos::logo(const char *icao) {
  if (url_template.empty()) return nullptr;
  Entry &e = entries_[icao];
  if (e.for_url != url_template) { e = Entry(); e.for_url = url_template; }     // the source changed: start again
  return e.logo.get();
}

static bool download(const std::string &url, const std::string &ua, const std::string &dest, bool verify) {
  std::string tmp = dest + ".part";
  const char *argv[] = {"curl", verify ? "-fsSL" : "-fsSLk", "--max-time", "12", "-A", ua.c_str(), "-o", tmp.c_str(), "--", url.c_str(), nullptr};
  pid_t pid;
  if (posix_spawnp(&pid, "curl", nullptr, nullptr, (char *const *)argv, environ) != 0) return false;
  int status = 0;
  waitpid(pid, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) { unlink(tmp.c_str()); return false; }
  return rename(tmp.c_str(), dest.c_str()) == 0;
}

void SimLogos::fetchPending(std::mutex &mu) {
  std::vector<std::string> todo;
  std::string tmpl, ua;
  bool verify;
  {
    std::lock_guard<std::mutex> lock(mu);
    tmpl = url_template; ua = user_agent; verify = verify_certs;
    for (auto &kv : entries_)
      if (!kv.second.tried && kv.second.for_url == tmpl && todo.size() < 3) todo.push_back(kv.first);
  }
  if (tmpl.compare(0, 8, "https://") != 0 && tmpl.compare(0, 7, "http://") != 0) return;
  for (const std::string &icao : todo) {
    unsigned long h = 1469598103934665603UL;
    for (unsigned char ch : tmpl) h = (h ^ ch) * 1099511628211UL;
    const char *home = getenv("HOME");
    char sub[64];
    snprintf(sub, sizeof sub, "/.cache/radarmash/logos/%016lx", h);
    std::string dir = std::string(home ? home : ".") + sub, path = dir + "/" + icao + ".img";
    for (size_t p = 1; p <= dir.size(); p++) if (p == dir.size() || dir[p] == '/') mkdir(dir.substr(0, p).c_str(), 0755);
    struct stat st;
    bool have = stat(path.c_str(), &st) == 0 && st.st_size > 0;
    if (!have) {
      std::string url = tmpl;
      for (size_t p = url.find("{icao}"); p != std::string::npos; p = url.find("{icao}", p + icao.size())) url.replace(p, 6, icao);
      have = download(url, ua, path, verify);
    }
    std::unique_ptr<rm::Logo> made;
    if (have) {
      int w = 0, hh = 0, n = 0;
      unsigned char *px = stbi_load(path.c_str(), &w, &hh, &n, 4);
      if (px) { made.reset(new rm::Logo()); if (!rm::makeLogo(px, w, hh, *made)) made.reset(); stbi_image_free(px); }
      else unlink(path.c_str());
    }
    std::lock_guard<std::mutex> lock(mu);
    Entry &e = entries_[icao];
    if (e.for_url != tmpl) continue;
    e.tried = true;                              // an airline with no logo at this source is not asked for again
    if (made) { e.logo = std::move(made); fetched++; } else missing++;
  }
}
