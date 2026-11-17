#!/bin/sh
# Build the firmware and produce radarmash-radar21en.bin, the image the stock firmware's updater will accept
# (it checks that the image's project name is "radar21en").
#   PIO=/path/to/pio ./build.sh [version]
set -e
cd "$(dirname "$0")"
VER=${1:-0.1.0}
python3 ../tools/embed.py ../web/admin.html include/admin_html.h ADMIN_HTML
python3 ../tools/embed.py ../web/wifi.html include/wifi_html.h WIFI_HTML
${PIO:-pio} run -e radar360
python3 ../tools/patch_desc.py .pio/build/radar360/firmware.bin radarmash-radar21en.bin radar21en "$VER"
