#!/usr/bin/env python3
"""Unpack an Android boot image header v3/v4 (GKI, e.g. Samsung b2q boot.img).

Layout (mkbootimg spec):
  page 0   header: 'ANDROID!' + kernel_sz, ramdisk_sz, os_version,
           hdr_size, resv[4], hdr_version, cmdline[1024+512]
  then     kernel, ramdisk pages aligned to 4096.
  v4 adds a boot signature size at the end of the header.
"""
import struct, sys, os

src, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
b = open(src, 'rb').read()
assert b[:8] == b'ANDROID!'
ks, rs, osv, hs, ver = struct.unpack_from('<5I', b, 8)
assert ver in (3, 4), f'header_version {ver}, expected 3 or 4'
cmdline = b[64:64+1024+512].split(b'\0')[0].decode()
info = dict(kernel_size=ks, ramdisk_size=rs, os_version=hex(osv),
            header_size=hs, header_version=ver, cmdline=cmdline)
PAGE = 4096
rnd = lambda n: (n + PAGE - 1) // PAGE * PAGE
off = hs if hs else PAGE
if hs > PAGE:
    vendor_cmd = b[PAGE:PAGE + hs - PAGE].split(b'\0')[0].decode()
    info['vendor_cmdline'] = vendor_cmd
for name, sz in [('kernel', ks), ('ramdisk', rs)]:
    if sz:
        open(f'{out}/{name}', 'wb').write(b[off:off+sz])
    info[name + '_offset'] = off
    off += rnd(sz)
if ver == 4:
    sig_sz = struct.unpack_from('<I', b, 1580)[0]
    info['signature_size'] = sig_sz
    if sig_sz:
        open(f'{out}/signature', 'wb').write(b[off:off+sig_sz])
    info['signature_offset'] = off
with open(f'{out}/header.txt', 'w') as f:
    for k, v in info.items():
        f.write(f'{k}: {v}\n')
print('\n'.join(f'{k}: {v}' for k, v in info.items()))
