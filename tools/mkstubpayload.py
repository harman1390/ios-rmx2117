#!/usr/bin/env python3
"""Pack the flat payload_stub binary + ADT into a SWEETXNU payload so the
shim loader's chainload path can be proven end-to-end without a real xnu
Mach-O (mkpayload.py needs the macOS-built kernel).

  mkstubpayload.py STUB_BIN ADT OUT
"""
import struct, sys

PAGE = 0x4000
BOOT_LINE_LENGTH = 608
CMDLINE = 0x48
SEG_TABLE = CMDLINE + BOOT_LINE_LENGTH

PHYS_BASE = 0x100000000       # matches mt6853.adt --highmem dram-base
MEM_SIZE = 0x80000000         # matches --highmem dram-size
CODE_VA = PHYS_BASE + 0x4000  # stub lands one 16K page above dram-base


def build(stub_path, adt_path):
    stub = open(stub_path, 'rb').read()
    adt = open(adt_path, 'rb').read()
    virt_base = PHYS_BASE                     # pa == va for the stub
    kernel_end = CODE_VA + PAGE               # one page of code
    hdr = SEG_TABLE + 32                      # 1 segment

    out = bytearray(hdr)
    out += stub
    out += b'\0' * (-len(out) & 0xf)
    adt_off = len(out)
    out += adt
    out += b'\0' * (-len(out) & 0xf)

    struct.pack_into('<8sII', out, 0, b'SWEETXNU', 1, 1)
    struct.pack_into('<7Q', out, 0x10, virt_base, CODE_VA, adt_off, len(adt),
                     kernel_end, PHYS_BASE, MEM_SIZE)
    cl = b'debug=0xc serial=3 cpus=1 keepsyms=1 stub=1'
    out[CMDLINE:CMDLINE + len(cl)] = cl
    struct.pack_into('<4Q', out, SEG_TABLE, CODE_VA, PAGE, hdr, len(stub))
    return bytes(out)


if __name__ == '__main__':
    blob = build(sys.argv[1], sys.argv[2])
    open(sys.argv[3], 'wb').write(blob)
    print(f'wrote {sys.argv[3]}: {len(blob)} bytes (entry 0x{CODE_VA:x})')
