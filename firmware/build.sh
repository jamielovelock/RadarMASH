#!/bin/sh
# Build the firmware and produce radarmash-radar21en.bin, the image the stock firmware's updater will accept
# (it checks that the image's project name is "radar21en").
#   PIO=/path/to/pio ./build.sh [version]
set -e
cd "$(dirname "$0")"
VER=${1:-0.1.0-dev}
[ -f include/secrets.h ] || { echo "Create include/secrets.h from include/secrets.example.h first." >&2; exit 1; }
${PIO:-pio} run -e radar360
python3 ../tools/patch_desc.py .pio/build/radar360/firmware.bin radarmash-radar21en.bin radar21en "$VER"
