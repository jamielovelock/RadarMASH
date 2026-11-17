#!/usr/bin/env python3
# RadarMASH - SPDX-License-Identifier: GPL-3.0-or-later
#
# Checks a built firmware image before anyone is asked to flash it. The radar's case is sealed, so an image that is the
# wrong kind, too big, or damaged must be caught here.     usage: check_image.py image.bin [expected-version]
import hashlib, struct, sys

SLOT_BYTES = 5 * 1024 * 1024          # each firmware slot on the Radar 360
PROJECT = "radar21en"                 # what the original firmware's updater insists on
ESP32_S3 = 9


def fail(msg):
    print("image check FAILED:", msg)
    sys.exit(1)


def main():
    path = sys.argv[1]
    want_version = sys.argv[2] if len(sys.argv) > 2 else None
    b = open(path, "rb").read()
    if len(b) < 0x120 or b[0] != 0xE9:
        fail("not an ESP app image")
    if len(b) > SLOT_BYTES:
        fail(f"{len(b)} bytes does not fit the {SLOT_BYTES}-byte slot")
    if struct.unpack_from("<H", b, 12)[0] != ESP32_S3:
        fail("not built for the ESP32-S3")
    D = 0x20                          # the app description follows the header and the first segment header
    if struct.unpack_from("<I", b, D)[0] != 0xABCD5432:
        fail("no app description: this looks like a bootloader or a merged image, which must never be uploaded")
    text = lambda off, n: b[off:off + n].split(b"\0")[0].decode("ascii", "replace")
    version, project = text(D + 16, 32), text(D + 48, 32)
    if project != PROJECT:
        fail(f"project name is {project!r}, not {PROJECT!r}; the original firmware would refuse it")
    if want_version and version != want_version:
        fail(f"version is {version!r}, expected {want_version!r}")

    off, ck = 24, 0xEF                # walk the segments to find the checksum, as the bootloader does
    for _ in range(b[1]):
        if off + 8 > len(b):
            fail("segment table runs off the end")
        _, length = struct.unpack_from("<II", b, off)
        off += 8
        if off + length > len(b):
            fail("a segment runs off the end")
        for x in b[off:off + length]:
            ck ^= x
        off += length
    end = (off + 16) & ~15
    if end > len(b) or b[end - 1] != ck:
        fail("checksum does not match")
    if b[23] == 1:
        if len(b) < end + 32 or hashlib.sha256(b[:end]).digest() != b[end:end + 32]:
            fail("SHA-256 does not match")
    else:
        fail("image has no SHA-256 appended")
    print(f"image ok: {project} {version}, {len(b)} bytes ({len(b) * 100 // SLOT_BYTES}% of the slot), sha256 {hashlib.sha256(b).hexdigest()}")


if __name__ == "__main__":
    main()
