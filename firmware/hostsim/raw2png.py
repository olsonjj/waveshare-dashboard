#!/usr/bin/env python3
"""Convert a raw RGB565 snapshot (with 12-byte header written by main.cpp)
into a PNG, using only the Python standard library."""
import struct, sys, zlib

def main(src, dst):
    data = open(src, 'rb').read()
    # header: lv_img_header_t (12 bytes), then int32 W, int32 H
    W, H = struct.unpack_from("<ii", data, 4)
    px = data[12:12 + W * H * 2]
    raw = bytearray()
    for i in range(0, len(px), 2):
        v = struct.unpack_from('<H', px, i)[0]
        r = ((v >> 11) & 0x1F) << 3
        g = ((v >> 5) & 0x3F) << 2
        b = (v & 0x1F) << 3
        raw += bytes((r, g, b))
    def chunk(tag, payload):
        c = struct.pack('>I', len(payload)) + tag + payload
        return c + struct.pack('>I', zlib.crc32(tag + payload) & 0xFFFFFFFF)
    png = (b'\x89PNG\r\n\x1a\n'
           + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0))
           + chunk(b'IDAT', zlib.compress(bytes(raw), 6))
           + chunk(b'IEND', b''))
    open(dst, 'wb').write(png)
    print(f'{dst}: {W}x{H}')

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
