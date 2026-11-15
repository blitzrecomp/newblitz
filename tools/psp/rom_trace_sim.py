#!/usr/bin/env python3
"""Replays a BLITZ_ROM_TRACE file (psp-host: one "<vi> <rom offset> <len>" line per ROM read) against LRU block caches of
several sizes, for docs/PSP_PORT.md §4.3: misses, bytes read from the Memory Stick, and the worst VI (most misses in one VI).
  tools/psp/rom_trace_sim.py <trace> [--cache-kb 512,1024,1536,2048,3072,4096] [--block-kb 16,32,64]
Standard library only (run with python3 -I)."""
import argparse
from collections import OrderedDict, Counter


def simulate(reads, block, blocks):
    lru = OrderedDict()
    misses = 0
    per_vi = Counter()
    for vi, off, n in reads:
        first, last = off // block, (off + n - 1) // block
        for b in range(first, last + 1):
            if b in lru:
                lru.move_to_end(b)
                continue
            misses += 1
            per_vi[vi] += 1
            lru[b] = True
            if len(lru) > blocks:
                lru.popitem(last=False)
    worst = per_vi.most_common(1)[0] if per_vi else (0, 0)
    return misses, misses * block, worst


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--cache-kb", default="512,1024,1536,2048,3072,4096")
    ap.add_argument("--block-kb", default="16,32,64")
    a = ap.parse_args()
    reads = []
    with open(a.trace) as f:
        for line in f:
            p = line.split()
            if len(p) == 3:
                reads.append((int(p[0]), int(p[1]), int(p[2])))
    total = sum(n for _, _, n in reads)
    vis = reads[-1][0] if reads else 0
    print(f"{len(reads)} reads, {total / 1048576:.2f} MB requested over {vis} VIs")
    print(f"{'block KB':>8} {'cache KB':>8} {'misses':>8} {'MB read':>8} {'worst VI (misses)':>18}")
    for bk in [int(x) for x in a.block_kb.split(",")]:
        for ck in [int(x) for x in a.cache_kb.split(",")]:
            m, b, (wvi, wn) = simulate(reads, bk * 1024, max(1, ck // bk))
            print(f"{bk:>8} {ck:>8} {m:>8} {b / 1048576:>8.2f} {f'{wvi} ({wn})':>18}")


if __name__ == "__main__":
    main()
