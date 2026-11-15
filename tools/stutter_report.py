#!/usr/bin/env python3
"""Stutter numbers from blitz.log files (docs/IOS_FEASIBILITY.md "Next pass: stutter").

The app logs a line every 5 seconds of play (src/main/fps_counter.cpp):
    [fps] 30.0 fps, worst gap 50 ms, 14 frames held 3 vblanks, 0 hitches
This sums those up per log, so two builds, devices or settings can be compared side by side:

    tools/stutter_report.py build/live/app.log ~/Desktop/iphone-blitz.log

A hitch is a frame held 4 or more vblanks (over 58 ms): the stutter. Logs from before 2026-11-14 count about 5 frames a
second held 3 vblanks everywhere, the Mac included: that was the counter reading the renderer's lagging copy of the VI
registers, not the game (which swaps every 2 VIs). Since then the line ends "VIs per frame 1/2/3/4+: ..." from the VI
thread's own registers, and 3s are rare. The one regular hitch left is the game's own: it stops drawing for ~65 ms at
the top of every play, as play select comes up (gfx_swaps_off). "after a scene change" counts the windows with a
hitch that start within 10 seconds of the game loading an overlay (loaded ovl_*): if the hitches cluster there, it's
likely shader compiles (step 5 of the notes), not pacing. Logs from before 2026-10-28 say "gaps over 50 ms", which
lumps both together: they're reported as such, with no hitch count.
"""
import re
import statistics
import sys

FPS = re.compile(r"\[fps\] ([\d.]+) fps, worst gap (\d+) ms, (\d+) (frames held 3 vblanks, (\d+) hitches|gaps over 50 ms)")
SCENE = re.compile(r"loaded (ovl_\w+)")


def report(path):
    windows, scene_age, near_scene = [], None, 0
    with open(path, errors="replace") as f:
        for line in f:
            if SCENE.search(line):
                scene_age = 0
                continue
            m = FPS.search(line)
            if not m:
                continue
            old = m[5] is None  # "gaps over 50 ms": 3-vblank frames and hitches together
            fps, worst, long = float(m[1]), int(m[2]), int(m[3]) if old else int(m[5])
            windows.append((fps, worst, long, old))
            if long and scene_age is not None and scene_age < 10:
                near_scene += 1
            if scene_age is not None:
                scene_age += 5
    if not windows:
        return f"{path}: no [fps] lines (the counter logs only while the game runs)"
    fps = [w[0] for w in windows]
    worst = sorted(w[1] for w in windows)
    long = sum(w[2] for w in windows)
    what = "gaps over 50 ms (old log: 3-vblank frames too)" if any(w[3] for w in windows) else "hitches (4+ vblanks)"
    bad = sum(1 for w in windows if w[2])
    minutes = len(windows) * 5 / 60
    p95 = worst[min(len(worst) - 1, int(len(worst) * 0.95))]
    return "\n".join([
        f"{path}",
        f"  {minutes:.1f} min of play ({len(windows)} windows of 5 s)",
        f"  frame rate: median {statistics.median(fps):.1f}, lowest {min(fps):.1f}",
        f"  worst gap per window: median {statistics.median(worst):.0f} ms, 95th {p95} ms, max {worst[-1]} ms",
        f"  {what}: {long} ({long / minutes:.1f} a minute), in {bad} of {len(windows)} windows "
        f"({100 * bad / len(windows):.0f}%), {near_scene} of those right after a scene change",
    ])


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    print("\n\n".join(report(p) for p in sys.argv[1:]))
