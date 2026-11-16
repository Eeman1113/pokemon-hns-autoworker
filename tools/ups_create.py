#!/usr/bin/env python3
"""Create a UPS patch from a base ROM to a target ROM.

UPS stores the CRC32 of the source, the target and the patch itself, so a
correctly-built patch refuses to apply to the wrong base rather than silently
producing a corrupt ROM.
"""
import sys, zlib

def write_vlv(out, value):
    while True:
        b = value & 0x7F
        value >>= 7
        if value == 0:
            out.append(0x80 | b)
            break
        out.append(b)
        value -= 1

def create(src_path, dst_path, out_path):
    src = open(src_path, "rb").read()
    dst = open(dst_path, "rb").read()
    n = max(len(src), len(dst))

    body = bytearray(b"UPS1")
    write_vlv(body, len(src))
    write_vlv(body, len(dst))

    get = lambda buf, i: buf[i] if i < len(buf) else 0
    i = 0
    last = 0
    hunks = 0
    while i < n:
        if get(src, i) != get(dst, i):
            write_vlv(body, i - last)
            hunks += 1
            while i < n and get(src, i) != get(dst, i):
                body.append(get(src, i) ^ get(dst, i))
                i += 1
            body.append(0)      # terminator
            i += 1              # the terminating byte is itself consumed
            last = i
        else:
            i += 1

    body += zlib.crc32(src).to_bytes(4, "little")
    body += zlib.crc32(dst).to_bytes(4, "little")
    body += (zlib.crc32(bytes(body)) & 0xFFFFFFFF).to_bytes(4, "little")

    open(out_path, "wb").write(body)
    print(f"  wrote {out_path}  {len(body):,} bytes  ({hunks:,} hunks)")
    print(f"  src crc32 {zlib.crc32(src)&0xFFFFFFFF:08x}  dst crc32 {zlib.crc32(dst)&0xFFFFFFFF:08x}")

if __name__ == "__main__":
    create(*sys.argv[1:4])
