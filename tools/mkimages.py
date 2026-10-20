#!/usr/bin/env python3
"""Writes test images in the simple formats that need no library: PPM, TGA
and a flattened Photoshop PSD. Usage: mkimages.py <output-dir>"""
import os
import struct
import sys


def gradient(w, h):
    rows = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            check = ((x // 16) + (y // 16)) % 2 == 0
            row += bytes((255 * x // max(1, w - 1), 255 * y // max(1, h - 1), 220 if check else 40))
        rows.append(bytes(row))
    return rows


def write_ppm(path, w, h, rows):
    with open(path, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (w, h))
        for r in rows:
            f.write(r)


def write_tga(path, w, h, rows):
    with open(path, "wb") as f:
        # uncompressed true-colour, origin top-left
        f.write(struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0, w, h, 24, 0x20))
        for r in rows:
            for i in range(0, len(r), 3):
                f.write(bytes((r[i + 2], r[i + 1], r[i])))  # BGR


def write_psd(path, w, h, rows):
    with open(path, "wb") as f:
        f.write(b"8BPS" + struct.pack(">H6xHIIHH", 1, 3, h, w, 8, 3))  # v1, 3 channels, 8-bit RGB
        f.write(struct.pack(">I", 0))  # colour mode data
        f.write(struct.pack(">I", 0))  # image resources
        f.write(struct.pack(">I", 0))  # layer and mask info
        f.write(struct.pack(">H", 0))  # raw image data, planar
        for c in range(3):
            for r in rows:
                f.write(bytes(r[c::3]))


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for tag, w, h in (("small", 240, 160), ("medium", 1000, 700)):
        rows = gradient(w, h)
        base = os.path.join(out, f"{tag}_{w}x{h}")
        write_ppm(base + ".ppm", w, h, rows)
        write_tga(base + ".tga", w, h, rows)
        write_psd(base + ".psd", w, h, rows)
        print("wrote", base + ".{ppm,tga,psd}")


if __name__ == "__main__":
    main()
