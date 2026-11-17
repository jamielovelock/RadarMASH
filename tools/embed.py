#!/usr/bin/env python3
# RadarMASH - SPDX-License-Identifier: GPL-3.0-or-later
# Turn a text file into a C++ header holding it as a string, so the simulator and the firmware can both serve the same
# settings page.   usage: embed.py web/admin.html out.h ADMIN_HTML
import sys
src, dst, name = sys.argv[1:4]
text = open(src, encoding="utf-8").read()
assert ')RMEMBED"' not in text
out = f"// Generated from {src} by tools/embed.py. Do not edit.\n#pragma once\nstatic const char {name}[] = R\"RMEMBED({text})RMEMBED\";\n"
try:
    if open(dst, encoding="utf-8").read() == out: sys.exit(0)      # unchanged: leave the timestamp alone
except OSError:
    pass
open(dst, "w", encoding="utf-8").write(out)
