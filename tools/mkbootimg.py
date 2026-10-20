#!/usr/bin/env python3
"""Minimal mkbootimg for Android boot image header v0/v1/v2 (RMX2117 port).

Usage:
  mkbootimg.py --header_version 2 --kernel Image --ramdisk ramdisk \
      --second second --dtb dtb --pagesize 2048 --base 0x40078000 \
      --kernel_offset 0x8000 --ramdisk_offset 0x07c08000 \
      --second_offset 0xbff88000 --tags_offset 0x0bc08000 \
      --dtb_offset 0x0bc08000 --cmdline "" --os_version 12.0.0 \
      -o out.img
Missing components are treated as empty files.
"""
import argparse, hashlib, struct, sys

def pad(b, ps):
    return b + b'\0' * ((ps - (len(b) % ps)) % ps)

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--header_version', type=int, default=0)
    p.add_argument('--kernel'); p.add_argument('--ramdisk'); p.add_argument('--second'); p.add_argument('--dtb')
    p.add_argument('--recovery_dtbo')
    p.add_argument('--pagesize', type=int, default=2048)
    p.add_argument('--base', type=lambda x: int(x, 0), default=0x10000000)
    p.add_argument('--kernel_offset', type=lambda x: int(x, 0), default=0x8000)
    p.add_argument('--ramdisk_offset', type=lambda x: int(x, 0), default=0x01000000)
    p.add_argument('--second_offset', type=lambda x: int(x, 0), default=0x00f00000)
    p.add_argument('--tags_offset', type=lambda x: int(x, 0), default=0x00000100)
    p.add_argument('--dtb_offset', type=lambda x: int(x, 0), default=0x00000100)
    p.add_argument('--cmdline', default='')
    p.add_argument('--board', default='')
    p.add_argument('--os_version', default='0.0.0')
    p.add_argument('--os_patch_level', default='')
    p.add_argument('-o', '--output', required=True)
    a = p.parse_args()

    load = lambda f: open(f, 'rb').read() if f else b''
    kernel, ramdisk, second, dtb = (load(a.kernel), load(a.ramdisk),
                                    load(a.second), load(a.dtb))
    rdtbo = load(a.recovery_dtbo)

    # aosp: ((major<<11|minor<<4|patch) << 11) | ((year-2000)<<4 | month)
    mj, mn, pt = (a.os_version + '.0.0').split('.')[:3]
    osv = ((int(mj) & 0x7f) << 11 | (int(mn) & 0x7f) << 4 | (int(pt) & 0x7f)) << 11
    if a.os_patch_level:
        y, m, _ = (a.os_patch_level + '-01-01').split('-')[:3]
        osv |= (((int(y) - 2000) & 0x7f) << 4) | (int(m) & 0xf)

    sha = hashlib.sha1()
    for b in (kernel, ramdisk, second, rdtbo, dtb):
        sha.update(b)

    addr = lambda x: x & 0xFFFFFFFF  # load addresses wrap mod 2^32
    hsize = 1660 if a.header_version >= 2 else (1648 if a.header_version == 1 else 608)
    header = struct.pack(
        '<8s10I16s512s32s1024s',
        b'ANDROID!', len(kernel), addr(a.base + a.kernel_offset),
        len(ramdisk), addr(a.base + a.ramdisk_offset),
        len(second), addr(a.base + a.second_offset),
        addr(a.base + a.tags_offset), a.pagesize, a.header_version, osv,
        a.board.encode()[:16], a.cmdline.encode()[:512],
        sha.digest(), b'')
    if a.header_version >= 1:
        cur = a.pagesize
        for sz in (len(kernel), len(ramdisk), len(second)):
            cur += ((sz + a.pagesize - 1) // a.pagesize) * a.pagesize
        header += struct.pack('<IQI', len(rdtbo), cur if rdtbo else 0, hsize)
        cur += ((len(rdtbo) + a.pagesize - 1) // a.pagesize) * a.pagesize
    if a.header_version >= 2:
        header += struct.pack('<IQ', len(dtb), a.base + a.dtb_offset)
    header = pad(header, a.pagesize)
    img = header + pad(kernel, a.pagesize) + pad(ramdisk, a.pagesize) + \
          pad(second, a.pagesize)
    if a.header_version >= 1:
        img += pad(rdtbo, a.pagesize)
    if a.header_version >= 2:
        img += pad(dtb, a.pagesize)
    open(a.output, 'wb').write(img)
    print(f"wrote {a.output}: {len(img)} bytes (kernel {len(kernel)}, dtb {len(dtb)})")

if __name__ == '__main__':
    main()
