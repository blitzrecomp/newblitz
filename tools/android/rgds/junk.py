# junk.py <run dir>...: the junk-label count in a run's strips (tab3.sh: the upper screen's bottom 96 rows, raw RGBA 1024 wide).
# The attract demo's four player boxes sit along the bottom; each shows TO JOIN IN or PRESS START. A box's text area is compared
# with the commonest looks of that box over all the runs given (the clean words); one far from all of them, in a frame where the
# box's frame is there at all, is junk. Writes <run>/junk.png (each suspect strip, for an eye) and prints the counts.
import sys, os, glob
from PIL import Image

W, H = 1024, 96

def load(path):
    d = open(path, 'rb').read()
    if len(d) < W * H * 4:
        return None
    return Image.frombytes('RGBA', (W, H), d[-W * H * 4:]).convert('RGB')

def small(img, box):
    # The box's words as a mask: white pixels only (the flames are orange, the box fill navy), on a 42x9 grid.
    c = img.crop(box).resize((42, 9))
    return [1 if (min(p) > 175 and max(p) - min(p) < 50) else 0 for p in c.getdata()]

def dist(a, b):
    return 100.0 * sum(1 for x, y in zip(a, b) if x != y) / len(a)  # % of cells that differ

def fill_ok(img, box):
    # The box is up: its fill, around the words, is the HUD's dark navy.
    x0, y0, x1, y1 = box
    pts = [img.getpixel((x, y)) for x in range(x0, x1, 12) for y in (y0 - 3, y1 + 2)]
    navy = sum(1 for (r, g, b) in pts if b >= r + 15 and r < 100 and g < 110)
    return navy >= len(pts) * 0.6

runs = sys.argv[1:]
frames = {}
for r in runs:
    frames[r] = []
    for p in sorted(glob.glob(os.path.join(r, 'strips', '*.raw'))):
        img = load(p)
        if img is not None:
            frames[r].append((os.path.basename(p), img))

# The four boxes: found by their bright frames in the first frames that have them (columns where the strip's mid rows
# are mostly light against the dark field). Fallback: quarters.
boxes = [(x + 44, 41, x + 168, 57) for x in (16, 272, 532, 792)]  # the words inside each of the four boxes (2026-11-03 strips)

# Each box's look across every frame; the clean looks are the commonest ones (greedy clusters).
looks = []
for r in runs:
    for name, img in frames[r]:
        for k, b in enumerate(boxes):
            looks.append((r, name, k, small(img, b), img))

def has_box(sig):
    return 15 <= sum(sig) <= 250  # some words, not a white-out

clusters = []  # [sig, count]
for r, name, k, sig, img in looks:
    if not has_box(sig) or not fill_ok(img, boxes[k]):
        continue
    for c in clusters:
        if dist(c[0], sig) < 6:
            c[1] += 1
            break
    else:
        clusters.append([sig, 1])
clusters.sort(key=lambda c: -c[1])
clean = [c[0] for c in clusters if c[1] >= 8][:12]
print(f"{len(looks)} box looks, {len(clusters)} clusters, {len(clean)} taken as clean (8+ alike)")

for r in runs:
    suspects = []
    with_box = 0
    for name, img in frames[r]:
        bad = []
        for k, b in enumerate(boxes):
            sig = small(img, b)
            if not has_box(sig) or not fill_ok(img, b):
                continue
            with_box += 1
            d = min((dist(c, sig) for c in clean), default=99)
            if d >= 10:
                bad.append((k, round(d, 1)))
        if bad:
            suspects.append((name, bad, img))
    print(f"{r}: {len(frames[r])} strips, {with_box} boxes seen, {len(suspects)} strips with an odd box")
    for name, bad, _ in suspects[:30]:
        print("   ", name, bad)
    if suspects:
        sheet = Image.new('RGB', (W, H * len(suspects)))
        for i, (_, _, img) in enumerate(suspects):
            sheet.paste(img, (0, H * i))
        sheet.save(os.path.join(r, 'junk.png'))
