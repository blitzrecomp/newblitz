#!/usr/bin/env python3
"""Compares a PSP screenshot with a reference render (docs/PSP_PORT.md M6) and writes a side-by-side PNG.

  compare_shots.py <reference.png> <psp.png> <side_by_side.png>   -> prints "ssim=<v> ssim_half=<v> psnr=<dB>"

The reference (tools/gbi_dump.py --render --render-scale 1: the N64's 512 x 256 frame, minus the last row and column the
scissor leaves out) is put on a 512 x 256 canvas and scaled to the PSP's 480 x 272, as the renderer scales the frame.
SSIM is computed on luma (BT.601) with a 7 x 7 uniform window, K1 0.01, K2 0.03, sample covariances (scikit-image's
structural_similarity defaults), over the whole image; ssim_half is the same after a 2 x 2 box downscale of both. The side-by-side is reference | PSP | |difference| x 4, at 2x.
Pure Python + PIL (no numpy in the venv).
"""
import math
import sys

from PIL import Image, ImageChops

W, H = 480, 272


def load_ref(path):
    ref = Image.open(path).convert("RGB")
    canvas = Image.new("RGB", (512, 256))
    canvas.paste(ref, (0, 0))
    return canvas.resize((W, H), Image.BILINEAR)


def luma(img):
    px = img.tobytes()
    return [0.299 * px[i] + 0.587 * px[i + 1] + 0.114 * px[i + 2] for i in range(0, len(px), 3)]


def integral(v, w, h):
    out = [0.0] * ((w + 1) * (h + 1))
    for y in range(h):
        row = 0.0
        base, prev = (y + 1) * (w + 1), y * (w + 1)
        for x in range(w):
            row += v[y * w + x]
            out[base + x + 1] = out[prev + x + 1] + row
    return out


def ssim(a, b, w, h, win=7):
    c1, c2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    ia, ib = integral(a, w, h), integral(b, w, h)
    iaa, ibb = integral([x * x for x in a], w, h), integral([x * x for x in b], w, h)
    iab = integral([x * y for x, y in zip(a, b)], w, h)
    n = win * win
    cov = n / (n - 1)
    r = win // 2
    total, count = 0.0, 0
    W1 = w + 1
    for y in range(r, h - r):
        y0, y1 = (y - r) * W1, (y + r + 1) * W1
        for x in range(r, w - r):
            x0, x1 = x - r, x + r + 1

            def s(t):
                return t[y1 + x1] - t[y0 + x1] - t[y1 + x0] + t[y0 + x0]
            ma, mb = s(ia) / n, s(ib) / n
            va = (s(iaa) / n - ma * ma) * cov
            vb = (s(ibb) / n - mb * mb) * cov
            vab = (s(iab) / n - ma * mb) * cov
            total += ((2 * ma * mb + c1) * (2 * vab + c2)) / ((ma * ma + mb * mb + c1) * (va + vb + c2))
            count += 1
    return total / count


def psnr(a, b):
    mse = sum((x - y) ** 2 for x, y in zip(a, b)) / len(a)
    return 99.0 if mse == 0 else 10 * math.log10(255 * 255 / mse)


def main():
    ref, psp = load_ref(sys.argv[1]), Image.open(sys.argv[2]).convert("RGB").resize((W, H))
    la, lb = luma(ref), luma(psp)
    s, p = ssim(la, lb, W, H), psnr(la, lb)
    # the same at 240 x 136 (2 x 2 box): texel-level noise (point vs bilinear sampling, LOD tiles, dither) weighs less
    ha, hb = luma(ref.resize((W // 2, H // 2), Image.BOX)), luma(psp.resize((W // 2, H // 2), Image.BOX))
    s2 = ssim(ha, hb, W // 2, H // 2)
    diff = ImageChops.difference(ref, psp).point(lambda v: min(255, v * 4))
    out = Image.new("RGB", (W * 3 + 8, H), (255, 255, 255))
    out.paste(ref, (0, 0))
    out.paste(psp, (W + 4, 0))
    out.paste(diff, (2 * W + 8, 0))
    out.resize((out.width * 2, out.height * 2), Image.NEAREST).save(sys.argv[3])
    print(f"ssim={s:.4f} ssim_half={s2:.4f} psnr={p:.2f}")


if __name__ == "__main__":
    main()
