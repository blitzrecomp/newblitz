# smaps.py <smaps file>: the game's mappings ranked by resident + swapped, grouped by name
import sys, re, collections
cur = None; tot = collections.defaultdict(lambda: [0, 0, 0])
for line in open(sys.argv[1], errors='replace'):
    m = re.match(r'^([0-9a-f]+)-([0-9a-f]+) \S+ \S+ \S+ \S+\s*(.*)$', line)
    if m:
        name = m.group(3).strip() or '[anon]'
        name = re.sub(r'\[anon:(.*?)\]', r'anon:\1', name)
        cur = tot[name]; cur[2] += 1; continue
    m = re.match(r'^(Rss|Swap):\s+(\d+) kB', line)
    if m and cur is not None:
        cur[0 if m.group(1) == 'Rss' else 1] += int(m.group(2))
rows = sorted(tot.items(), key=lambda kv: -(kv[1][0] + kv[1][1]))
print(f"{'rss MB':>7} {'swap MB':>7} {'n':>5}  name")
for name, (rss, sw, n) in rows[:25]:
    print(f"{rss/1024:7.1f} {sw/1024:7.1f} {n:5d}  {name[:90]}")
print(f"total rss {sum(v[0] for v in tot.values())/1024:.0f} MB, swap {sum(v[1] for v in tot.values())/1024:.0f} MB")
