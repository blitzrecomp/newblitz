import csv, re, sys, os, statistics as st
run = sys.argv[1]
p = lambda n: os.path.join(run, n)
start = int(open(p('field_from_line')).read().strip() or 0) if os.path.exists(p('field_from_line')) else 0
field = open(p('blitz.log'), errors='replace').read().splitlines()[start:]
fps = [float(m.group(1)) for l in field for m in [re.match(r'\[fps\] ([0-9.]+) fps, worst gap (\d+)', l)] if m]
gaps = [int(m.group(1)) for l in field for m in [re.match(r'\[fps\] [0-9.]+ fps, worst gap (\d+)', l)] if m]
out = []
out.append(f"field fps: median {st.median(fps):.1f}, min {min(fps):.1f}, windows {len(fps)}, worst gap median {st.median(gaps)} ms max {max(gaps)} ms" if fps else "field fps: none")
fr = [tuple(map(int, m.groups())) for l in field for m in [re.match(r'\[frames\] 5.0 s: swaps (\d+), first (\d+), second (\d+)', l)] if m]
if fr: out.append(f"shown/s: game {st.median([x[0] for x in fr])/5:.1f}, first {st.median([x[1] for x in fr])/5:.1f}, second {st.median([x[2] for x in fr])/5:.1f}")
gpu = [float(m.group(1)) for l in field for m in [re.search(r'frame pass GPU ([0-9.]+) ms', l)] if m]
if gpu: out.append(f"frame pass GPU median {st.median(gpu):.0f} ms per 5 s")
rows = list(csv.DictReader(open(p('sampler.csv')))) if os.path.exists(p('sampler.csv')) else []
def i(v):
    try: return int(float(v))
    except: return 0
late = [r for r in rows if i(r['game_pid']) > 0][len(rows)//3:]
if late:
    f = lambda k: st.median([i(r[k]) for r in late])
    out.append(f"avail {f('memavail_kb')//1024} MB, gpu load {f('gpu_load')}% at {f('gpu_mhz')} MHz, temp {f('temp')/1000:.1f} C")
import re as _re
mf=[int(m.group(1)) for l in field for m in [_re.match(r'\[threads\] \d+ s, all \d+%, (\d+) major faults', l)] if m]
hit=[int(m.group(1)) for l in field for m in [_re.search(r'held 3 vblanks, (\d+) hitches', l)] if m]
if mf: out.append(f"major faults per [threads] line: median {st.median(mf)}, hitches per 5 s: median {st.median(hit) if hit else '-'}, total {sum(hit)}")
print('\n'.join(out)); open(p('summary.txt'), 'w').write('\n'.join(out) + '\n')
