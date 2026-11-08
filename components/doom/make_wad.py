#!/usr/bin/env python3
"""doom1.wad -> doom1.wad.z, the game data main/app_doom.c downloads into flash at 0x650000 (at most 1.69 MB):
"DWZ1", the WAD's length, the zlib stream's length (little endian u32 each), then the zlib stream of the whole,
unmodified WAD. Usage: make_wad.py DOOM1.WAD doom1.wad.z"""
import struct, sys, zlib

wad = open(sys.argv[1], 'rb').read()
assert wad[:4] == b'IWAD', 'not an IWAD'
z = zlib.compress(wad, 9)
out = b'DWZ1' + struct.pack('<II', len(wad), len(z)) + z
assert len(out) <= 0x800000 - 0x650000, f'{len(out)} bytes: too big for the free flash'
open(sys.argv[2], 'wb').write(out)
print(f'{sys.argv[2]}: {len(out)} bytes ({0x800000 - 0x650000 - len(out)} to spare)')
