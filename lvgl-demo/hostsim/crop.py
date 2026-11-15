#!/usr/bin/env python3
"""Crop a region from a raw RGB565 snapshot (main.cpp dump_fb format) into a
proper RGB888 PNG. Usage: crop.py src.raw dst.png x0 y0 x1 y1 [zoom]"""
import struct, sys, zlib

def main(argv):
    src, dst = argv[1], argv[2]
    x0, y0, x1, y1 = map(int, argv[3:7])
    z = int(argv[7]) if len(argv) > 7 else 1
    d = open(src, 'rb').read()
    W, H = struct.unpack_from('<ii', d, 4)
    px = d[12:]
    w, h = x1 - x0, y1 - y0
    raw = bytearray()
    for y in range(y0, y1):
        row = bytearray()
        for x in range(x0, x1):
            v = struct.unpack_from('<H', px, (y * W + x) * 2)[0]
            r = ((v >> 11) & 0x1F) << 3
            g = ((v >> 5) & 0x3F) << 2
            b = (v & 0x1F) << 3
            row += bytes((r, g, b)) * z   # zoom = pixel repetition
        raw += row * z
    def chunk(tag, payload):
        c = struct.pack('>I', len(payload)) + tag + payload
        return c + struct.pack('>I', zlib.crc32(tag + payload) & 0xFFFFFFFF)
    png = (b'\x89PNG\r\n\x1a\n'
           + chunk(b'IHDR', struct.pack('>IIBBBBB', w * z, h * z, 8, 2, 0, 0, 0))
           + chunk(b'IDAT', zlib.compress(bytes(raw), 6))
           + chunk(b'IEND', b''))
    open(dst, 'wb').write(png)
    print(dst, w * z, h * z)

main(sys.argv)
