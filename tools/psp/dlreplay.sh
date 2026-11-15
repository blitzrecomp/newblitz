#!/bin/bash
# M6 (docs/PSP_PORT.md 4.5): the display-list replay in PPSSPP, compared with tools/gbi_dump.py --render.
#   tools/psp/dlreplay.sh [dump numbers...]      default: 543 1439 1448 1529 1538 (build/dumps/rdram_<n>.bin + .txt)
# 1. copies the dumps into PPSSPP's memory stick (<ms>/PSP/GAME/NBDLREPLAY/dumps; game data: removed again afterwards,
#    never committed), waits until no other PPSSPP runs (polls every 20 s), runs build/psp/dlreplay/EBOOT.PBP there
#    through tools/psp/run_ppsspp.sh;
# 2. turns each <app>/shots/rdram_<n>.shot into a PNG (tools/psp/shot2png.py), renders the same dump with
#    tools/gbi_dump.py --render, and writes rdram_<n>_side.png (reference | PSP | difference) and the SSIM to
#    build/psp/runs/<stamp>/m6.txt, with the replay's own [dlreplay]/[gfx] lines.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PY="${PY:-$ROOT/.venv/bin/python}"
MEMSTICK="${MEMSTICK:-$HOME/.config/ppsspp}"
APPDIR="$MEMSTICK/PSP/GAME/NBDLREPLAY"
EBOOT="$ROOT/build/psp/dlreplay/EBOOT.PBP"
NUMS=("$@")
[ ${#NUMS[@]} -gt 0 ] || NUMS=(543 1439 1448 1529 1538)
[ -f "$EBOOT" ] || { echo "no $EBOOT (make psp-dlreplay)" >&2; exit 2; }

rm -rf "$APPDIR/dumps" "$APPDIR/shots"
mkdir -p "$APPDIR/dumps"
for n in "${NUMS[@]}"; do
    cp "$ROOT/build/dumps/rdram_$n.bin" "$ROOT/build/dumps/rdram_$n.txt" "$APPDIR/dumps/" || exit 2
done
# run_ppsspp.sh WAIT=1 polls every 20 s until no other PPSSPP is open (another engineer may be using it)
out="$(WAIT=1 FOLDER=NBDLREPLAY EXTRA_INI="${EXTRA_INI:-$ROOT/platform/psp/dlreplay/software.ini}" "$ROOT/tools/psp/run_ppsspp.sh" "$EBOOT" "${SECS:-300}")"
echo "$out"
STAMP="$(echo "$out" | sed -n 's/^run \([0-9-]*\): PPSSPP pid.*/\1/p' | head -1)"
RUN="$ROOT/build/psp/runs/$STAMP"
[ -n "$STAMP" ] && [ -d "$RUN" ] || { echo "no run folder" >&2; exit 1; }
cp -R "$APPDIR/shots" "$RUN/shots" 2>/dev/null
rm -rf "$APPDIR/dumps"
{
    echo "M6 display-list replay, run $STAMP"
    grep -E '\[(dlreplay|gfx)\]' "$RUN/app.txt"
    echo
    echo "screenshot vs tools/gbi_dump.py --render (SSIM on luma, 7x7 window, at 480x272):"
} > "$RUN/m6.txt"
for n in "${NUMS[@]}"; do
    shot="$RUN/shots/rdram_$n.shot"
    if [ ! -f "$shot" ]; then echo "rdram_$n: no screenshot" | tee -a "$RUN/m6.txt"; continue; fi
    "$PY" -I "$ROOT/tools/psp/shot2png.py" "$shot" "$RUN/rdram_${n}_psp.png" > /dev/null
    dl="$(sed -E 's/.*dlist=([0-9A-Fa-f]+).*/\1/' "$ROOT/build/dumps/rdram_$n.txt")"
    "$PY" "$ROOT/tools/gbi_dump.py" "$ROOT/build/dumps/rdram_$n.bin" "$dl" --render "$RUN/rdram_${n}_ref.png" --render-scale 1 \
        > /dev/null 2>&1
    rm -f "$RUN/rdram_${n}_ref_ids.png"
    r="$("$PY" -I "$ROOT/tools/psp/compare_shots.py" "$RUN/rdram_${n}_ref.png" "$RUN/rdram_${n}_psp.png" "$RUN/rdram_${n}_side.png")"
    echo "rdram_$n: $r" | tee -a "$RUN/m6.txt"
done
echo "-> $RUN/m6.txt"
