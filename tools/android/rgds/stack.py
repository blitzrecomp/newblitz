# stack.py out.bmp in1.bmp in2.bmp ...: same-size 24-bit BMPs stacked top to bottom into one BMP
import sys, struct
def load(p):
    d = open(p, 'rb').read(); off = struct.unpack_from('<I', d, 10)[0]
    w, h = struct.unpack_from('<ii', d, 18); bpp = struct.unpack_from('<H', d, 28)[0] // 8
    row = ((w * bpp + 3) // 4) * 4
    rows = [d[off + y * row: off + y * row + w * bpp] for y in range(abs(h))]
    if h > 0: rows = rows[::-1]            # bottom-up file: make top-down
    if bpp == 4: rows = [b''.join(r[i:i+3] for i in range(0, len(r), 4)) for r in rows]
    return w, rows
imgs = [load(p) for p in sys.argv[2:]]
w = imgs[0][0]; rows = [r for _, rs in imgs for r in (rs + [b'\xff\xff\xff' * w] * 2)]
row = ((w * 3 + 3) // 4) * 4; pad = b'\0' * (row - w * 3); h = len(rows)
data = b''.join(r + pad for r in rows[::-1])
hdr = b'BM' + struct.pack('<IHHI', 54 + len(data), 0, 0, 54) + struct.pack('<IiiHHIIiiII', 40, w, h, 1, 24, 0, len(data), 2835, 2835, 0, 0)
open(sys.argv[1], 'wb').write(hdr + data)
