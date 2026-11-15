#!/usr/bin/env python3
"""Turns a PSP "shot" result (a "BLITZSHOT1 <w> <h> <format>" line, then raw rows) into a PNG.
Formats are sceDisplay's: 0 = 5650, 1 = 5551, 2 = 4444, 3 = 8888 (little-endian words, red in the low bits).
Usage: shot2png.py <shot file> <out.png>"""
import sys

from PIL import Image


def main():
    data = open(sys.argv[1], "rb").read()
    nl = data.index(b"\n")
    magic, w, h, fmt = data[:nl].decode().split()
    if magic != "BLITZSHOT1":
        sys.exit("not a BLITZSHOT1 file")
    w, h, fmt = int(w), int(h), int(fmt)
    px = data[nl + 1:]
    if fmt == 3:
        img = Image.frombytes("RGBA", (w, h), px[: w * h * 4]).convert("RGB")
    else:
        out = bytearray(w * h * 3)
        for i in range(w * h):
            v = px[i * 2] | px[i * 2 + 1] << 8
            if fmt == 0:
                r, g, b = (v & 31) << 3, ((v >> 5) & 63) << 2, ((v >> 11) & 31) << 3
            elif fmt == 1:
                r, g, b = (v & 31) << 3, ((v >> 5) & 31) << 3, ((v >> 10) & 31) << 3
            else:
                r, g, b = (v & 15) << 4, ((v >> 4) & 15) << 4, ((v >> 8) & 15) << 4
            out[i * 3:i * 3 + 3] = bytes((r, g, b))
        img = Image.frombytes("RGB", (w, h), bytes(out))
    img.save(sys.argv[2])
    print(f"{sys.argv[2]} ({w}x{h}, format {fmt})")


if __name__ == "__main__":
    main()
