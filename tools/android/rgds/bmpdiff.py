# bmpdiff.py a.bmp b.bmp: mean abs difference (0-255) of two same-size 24/32-bit BMPs, sampled
import sys, struct
def load(p):
    d = open(p, 'rb').read()
    off = struct.unpack_from('<I', d, 10)[0]; w, h = struct.unpack_from('<ii', d, 18); bpp = struct.unpack_from('<H', d, 28)[0]
    return w, abs(h), bpp // 8, d[off:]
wa, ha, ba, a = load(sys.argv[1]); wb, hb, bb, b = load(sys.argv[2])
if (wa, ha, ba) != (wb, hb, bb): print(255); sys.exit()
row = ((wa * ba + 3) // 4) * 4
tot = n = 0
for y in range(0, ha, 3):
    for x in range(0, wa, 4):
        i = y * row + x * ba
        tot += abs(a[i] - b[i]) + abs(a[i+1] - b[i+1]) + abs(a[i+2] - b[i+2]); n += 3
print(round(tot / n, 1))
