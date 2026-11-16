#!/usr/bin/env python3
"""Apply a UPS patch, verifying every checksum the format carries.

UPS stores CRC32 of the input, the output, and the patch itself in its last 12
bytes. A patcher that skips those will happily produce a broken ROM from the
wrong base and you find out hours later. This one refuses.
"""
import sys, zlib

def read_vlv(buf, pos):
    value, shift = 0, 1
    while True:
        b = buf[pos]; pos += 1
        value += (b & 0x7F) * shift
        if b & 0x80:
            break
        shift <<= 7
        value += shift
    return value, pos

def apply(rom_path, patch_path, out_path):
    rom = bytearray(open(rom_path, "rb").read())
    patch = open(patch_path, "rb").read()

    if patch[:4] != b"UPS1":
        sys.exit("not a UPS1 patch")

    # the trailing 12 bytes are three little-endian CRC32s
    body, tail = patch[:-12], patch[-12:]
    crc_in  = int.from_bytes(tail[0:4], "little")
    crc_out = int.from_bytes(tail[4:8], "little")
    crc_pat = int.from_bytes(tail[8:12], "little")

    actual_patch_crc = zlib.crc32(patch[:-4]) & 0xFFFFFFFF
    if actual_patch_crc != crc_pat:
        sys.exit(f"patch file is corrupt: crc {actual_patch_crc:08x} != {crc_pat:08x}")

    actual_rom_crc = zlib.crc32(rom) & 0xFFFFFFFF
    if actual_rom_crc != crc_in:
        sys.exit(f"WRONG BASE ROM: yours is {actual_rom_crc:08x}, patch expects {crc_in:08x}")
    print(f"  base ROM crc32   {actual_rom_crc:08x}  (patch expects {crc_in:08x})  OK")

    pos = 4
    size_in, pos = read_vlv(body, pos)
    size_out, pos = read_vlv(body, pos)
    print(f"  input  size      {size_in:,} bytes  (file is {len(rom):,})")
    print(f"  output size      {size_out:,} bytes")
    if size_in != len(rom):
        sys.exit("base ROM is the wrong size for this patch")

    out = bytearray(rom) + bytearray(max(0, size_out - len(rom)))
    offset, hunks = 0, 0
    while pos < len(body):
        rel, pos = read_vlv(body, pos)
        offset += rel
        hunks += 1
        while True:
            x = body[pos]; pos += 1
            if x == 0:
                offset += 1
                break
            if offset < len(out):
                out[offset] ^= x
            offset += 1
    out = out[:size_out]

    actual_out_crc = zlib.crc32(out) & 0xFFFFFFFF
    if actual_out_crc != crc_out:
        sys.exit(f"PATCH FAILED: result crc {actual_out_crc:08x} != expected {crc_out:08x}")
    print(f"  patched  crc32   {actual_out_crc:08x}  (patch expects {crc_out:08x})  OK")
    print(f"  applied {hunks:,} hunks")

    open(out_path, "wb").write(out)
    print(f"  wrote {out_path}")

if __name__ == "__main__":
    apply(*sys.argv[1:4])
