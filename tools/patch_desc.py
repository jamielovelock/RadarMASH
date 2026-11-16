#!/usr/bin/env python3
# RadarMASH - SPDX-License-Identifier: GPL-3.0-or-later
# usage: patch_desc.py in.bin out.bin project_name [version]
# Rewrite project_name/version in an ESP32 app image's esp_app_desc_t and fix checksum + SHA256.
import sys,struct,hashlib
src,dst,proj,ver=sys.argv[1],sys.argv[2],sys.argv[3],(sys.argv[4] if len(sys.argv)>4 else None)
b=bytearray(open(src,"rb").read())
assert b[0]==0xE9; nseg=b[1]; hash_appended=b[23]==1
D=0x20; assert struct.unpack_from("<I",b,D)[0]==0xABCD5432,"no app desc"
def put(off,s): b[off:off+32]=s.encode()[:31].ljust(32,b"\0")
if ver: put(D+16,ver)
put(D+48,proj)
off=24; ck=0xEF
for _ in range(nseg):
    addr,ln=struct.unpack_from("<II",b,off); off+=8
    for x in b[off:off+ln]: ck^=x
    off+=ln
end=(off+16)&~15            # checksum is last byte of the 16-byte-aligned block
b[end-1]=ck
if hash_appended:
    b[end:end+32]=hashlib.sha256(bytes(b[:end])).digest()
open(dst,"wb").write(b); print("patched",dst,"checksum",hex(ck),"len",len(b))
