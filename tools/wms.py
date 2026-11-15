#!/usr/bin/env python3
"""Convert a PNG to NFL Blitz's .wms image format (and decode() reads types 0, 8 and 19 back).

  wms.py encode IMAGE.png OUT.wms [--origin X Y] [--flags N] [--type 8|19]

Type 8 ("rgba16", the default): 16-bit colour with 1-bit alpha. Type 19: the same colour plane plus a
4-bit alpha plane (soft edges and drop shadows, as the game's button icons use).

The result can be dropped into <app folder>/art/files/<game file name>.wms (2026 build) to replace
that game image; see docs/BLITZ2026.md "Art drop-in". 2D sprites (logos, title boxes, banners) may
change size; uniform parts and endzones must keep their original size. Origin and flags should
match the image being replaced (e.g. blitzlog.wms: origin 108 200, flags 4).

Header (32 bytes): "wn", version 2, flags, then big-endian u32 type, u32 width, u32 height,
s32 origin x, s32 origin y, 8 zero bytes. Pixels: big-endian RGBA5551.
"""
import argparse
import struct
import sys

from PIL import Image


def to5551(r, g, b, a):
    return ((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | (1 if a >= 128 else 0)


def encode_rgba16(img, flags=4, ox=0, oy=0):
    img = img.convert("RGBA")
    w, h = img.size
    body = b"".join(struct.pack(">H", to5551(*p)) for p in (img.get_flattened_data() if hasattr(img, 'get_flattened_data') else img.getdata()))
    return b"wn\x02" + bytes([flags]) + struct.pack(">IIIii", 8, w, h, ox, oy) + bytes(8) + body


def encode_type19(img, flags=4, ox=0, oy=0):
    """RGBA5551 plane (padded to 8 bytes), then an I4 alpha plane: rows of ceil(w/2) bytes, high
    nibble first, no trailing pad. Matches the game's own button icons byte for byte."""
    img = img.convert("RGBA")
    w, h = img.size
    px = list(img.get_flattened_data() if hasattr(img, "get_flattened_data") else img.getdata())
    col = b"".join(struct.pack(">H", ((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | (1 if a >= 8 else 0))
                   for r, g, b, a in px)
    col += bytes((-len(col)) % 8)
    stride = (w + 1) // 2
    alpha = bytearray(stride * h)
    for y in range(h):
        for x in range(w):
            alpha[y * stride + x // 2] |= (px[y * w + x][3] >> 4) << (4 if x % 2 == 0 else 0)
    return b"wn\x02" + bytes([flags]) + struct.pack(">IIIii", 19, w, h, ox, oy) + bytes(8) + col + bytes(alpha)


def encode_ci8(img, flags=4, ox=0, oy=0):
    """Type 0: 8-bit colour indices (padded to 8 bytes), then a 256-entry RGBA5551 palette, as the game's own
    full-screen and badge images are. Up to 255 colours plus index 0 for transparent (hard alpha)."""
    from PIL import Image
    img = img.convert("RGBA")
    w, h = img.size
    px = list(img.get_flattened_data() if hasattr(img, "get_flattened_data") else img.getdata())
    rgb = Image.new("RGB", (w, h))
    rgb.putdata([p[:3] for p in px])
    q = rgb.quantize(colors=255, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    pal = q.getpalette()[:255 * 3]
    idx = list(q.get_flattened_data() if hasattr(q, "get_flattened_data") else q.getdata())
    body = bytes(0 if px[i][3] < 128 else idx[i] + 1 for i in range(w * h))
    body += bytes((-len(body)) % 8)
    entries = [0] + [to5551(pal[3 * k], pal[3 * k + 1], pal[3 * k + 2], 255) for k in range(len(pal) // 3)]
    entries += [0] * (256 - len(entries))
    return b"wn\x02" + bytes([flags]) + struct.pack(">IIIii", 0, w, h, ox, oy) + bytes(8) + body + b"".join(struct.pack(">H", e) for e in entries)


def from5551(v):
    return ((v >> 11) & 31) * 255 // 31, ((v >> 6) & 31) * 255 // 31, ((v >> 1) & 31) * 255 // 31, 255 if v & 1 else 0


def decode(data):
    """A .wms of type 0 (8-bit colour index + RGBA5551 palette), 8 or 19 -> (RGBA PIL image, header dict)."""
    from PIL import Image
    flags = data[3]
    typ, w, h, ox, oy = struct.unpack_from(">IIIii", data, 4)
    p, n = data[32:], w * h
    pad8 = lambda k: (k + 7) & ~7
    if typ == 0:
        pal = [from5551(struct.unpack_from(">H", p, pad8(n) + 2 * i)[0]) for i in range(256)]
        px = [pal[p[i]] for i in range(n)]
    elif typ in (8, 19):
        px = [from5551(struct.unpack_from(">H", p, 2 * i)[0]) for i in range(n)]
        if typ == 19:
            base, row = pad8(2 * n), (w + 1) // 2
            px = [c[:3] + ((((p[base + (i // w) * row + (i % w) // 2] >> (4 if (i % w) % 2 == 0 else 0)) & 15) * 17),)
                  for i, c in enumerate(px)]
    else:
        raise ValueError(f"wms type {typ} isn't decoded here (0, 8 and 19 are)")
    img = Image.new("RGBA", (w, h))
    img.putdata(px)
    return img, {"type": typ, "width": w, "height": h, "origin": (ox, oy), "flags": flags}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    e = sub.add_parser("encode")
    e.add_argument("png")
    e.add_argument("out")
    e.add_argument("--origin", nargs=2, type=int, default=[0, 0], metavar=("X", "Y"))
    e.add_argument("--flags", type=int, default=4)
    e.add_argument("--type", type=int, choices=(8, 19), default=8)
    args = ap.parse_args(argv)
    enc = encode_type19 if args.type == 19 else encode_rgba16
    data = enc(Image.open(args.png), args.flags, *args.origin)
    open(args.out, "wb").write(data)
    print(f"wrote {args.out}: {len(data)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
