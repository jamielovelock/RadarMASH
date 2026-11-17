#!/bin/sh
# Build the firmware and produce radarmash-radar21en.bin, the image the original firmware's updater will accept
# (it checks that the image's project name is "radar21en"). The version comes from FW_VERSION in src/main.cpp.
#   PIO=/path/to/pio ./build.sh
set -e
cd "$(dirname "$0")"
VER=$(sed -n 's/^#define FW_VERSION "\(.*\)"$/\1/p' src/main.cpp)
[ -n "$VER" ] || { echo "could not read FW_VERSION from src/main.cpp" >&2; exit 1; }
# A build that carries Wi-Fi details must never be published. include/secrets.h is optional and for development only.
[ -f include/secrets.h ] && echo "note: include/secrets.h is present; this image will carry its Wi-Fi details. Do not share it." >&2
python3 ../tools/embed.py ../web/admin.html include/admin_html.h ADMIN_HTML
python3 ../tools/embed.py ../web/wifi.html include/wifi_html.h WIFI_HTML
${PIO:-pio} run -e radar360
python3 ../tools/patch_desc.py .pio/build/radar360/firmware.bin radarmash-radar21en.bin radar21en "$VER"
python3 ../tools/check_image.py radarmash-radar21en.bin "$VER"
