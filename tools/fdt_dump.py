#!/usr/bin/env python3
"""Minimal FDT (flattened device tree) dump for the MT6853 live dtb.

  fdt_dump.py DTB [node-path-regex ...]

Prints every node path matching the regexes (or all of them), with
property names and decoded values for common encodings.
"""
import re, struct, sys

FDT_BEGIN_NODE, FDT_END_NODE, FDT_PROP, FDT_NOP, FDT_END = 1, 2, 3, 4, 9


def load(path):
    b = open(path, 'rb').read()
    magic, _sz, so, ss, _mr, _r0, _r1, _v, _lc = struct.unpack('>9I', b[:36])
    assert magic == 0xd00dfeed
    return b, so, ss


def parse(path):
    b, so, ss = load(path)
    out, pos = [], so
    stack = []
    while True:
        tok = struct.unpack_from('>I', b, pos)[0]
        pos += 4
        if tok == FDT_NOP:
            continue
        if tok == FDT_BEGIN_NODE:
            name = b[pos:pos + b.find(b'\0', pos) - pos].decode(errors='replace')
            pos += len(name) + 1
            pos = (pos + 3) & ~3
            stack.append(name)
            out.append(('/'.join(stack) or '/', 'BEGIN', None))
            continue
        if tok == FDT_END_NODE:
            stack.pop()
            continue
        if tok == FDT_PROP:
            ln, off = struct.unpack_from('>2I', b, pos)
            pos += 8
            val = b[pos:pos + ln]
            pos = (pos + ln + 3) & ~3
            pname = b[ss + off:ss + off + b.find(b'\0', ss + off) - ss - off].decode(errors='replace')
            out.append(('/'.join(stack) or '/', pname, val))
            continue
        if tok == FDT_END:
            break
        raise ValueError(f'bad token {tok} @ {pos:x}')
    return out


def fmt(val):
    if not val:
        return '(bool)'
    if len(val) == 4:
        return f'0x{struct.unpack(">I", val)[0]:08x}'
    if len(val) % 4 == 0:
        return ' '.join(f'0x{struct.unpack(">I", val[i:i + 4])[0]:08x}'
                        for i in range(0, len(val), 4))
    try:
        return repr(val.split(b'\0')[0].decode())
    except UnicodeDecodeError:
        return val.hex()[:64]


def main():
    pats = [re.compile(p) for p in sys.argv[2:]] if len(sys.argv) > 2 else None
    cur = None
    for path, key, val in parse(sys.argv[1]):
        if key == 'BEGIN':
            cur = path
            if not pats or any(p.search(path) for p in pats):
                print(f'\n{path}:')
            continue
        if not pats or any(p.search(cur) for p in pats):
            print(f'  {key} = {fmt(val)}')


if __name__ == '__main__':
    main()
