# ppmdiff.py a.ppm b.ppm: mean abs difference (0-255) over the middle of two P6 frames (rows 120-660), sampled.
# nano's shots were stored mirrored, and on 2026-11-03 came out the right way round: both ways are tried, the closer wins.
import sys
def load(p):
    d = open(p, 'rb').read()
    parts = d.split(b'\n', 3)
    w, h = map(int, parts[1].split()); return w, h, parts[3]
wa, ha, a = load(sys.argv[1]); wb, hb, b = load(sys.argv[2])
if (wa, ha) != (wb, hb): print(255); sys.exit()
def diff(flip):
    tot = n = 0
    for y in range(120, 660, 6):
        row = y * wa * 3
        for x in range(0, wa, 5):
            xb = (wa - 1 - x) if flip else x
            for c in range(3):
                tot += abs(a[row + x * 3 + c] - b[row + xb * 3 + c]); n += 1
    return tot / n
print(round(min(diff(False), diff(True)), 1))
