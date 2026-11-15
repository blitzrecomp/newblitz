#!/bin/bash
# M3(b) lockstep comparison (docs/PSP_PORT.md §3.4): two psp-host builds on virtual time, from identical app folders, compared
# by the RDRAM CRC32 they log every 100 VIs.
#   tools/psp/lockstep.sh <binary A> <binary B> [VIS=3000] [input script]
# Each run gets a fresh app folder under build/psp/lockstep/<a|b> seeded with the same ROM and the same Controller Pak
# (a blank pak has a random serial, which would differ). Prints the first VI whose CRC differs, or "identical".
# Modern (M8): SEED_APP=<a kit's NEWBLITZ folder> seeds from that folder (its merged ROM, kit.txt, rosters) instead of --rom,
# in LOCKSTEP_DIR (default build/psp/lockstep; use another for Modern, e.g. build/psp/lockstep_modern).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
A="$1"; B="$2"; VIS="${3:-3000}"; INPUT="${4:-}"
ROM="${BLITZ_PSP_ROM:-$ROOT/rom/NFL Blitz 2000 (USA) (Rev 1).z64}"
OUT="${LOCKSTEP_DIR:-$ROOT/build/psp/lockstep}"
SEED="$OUT/seed"
mkdir -p "$OUT"
if [ -z "$(find "$SEED/saves" -name 'blitz2000_pak1.mpk' 2>/dev/null | head -1)" ]; then
    # One short run makes a pak; every comparison starts from that same file.
    rm -rf "$SEED"
    if [ -n "${SEED_APP:-}" ]; then mkdir -p "$SEED" && cp -R "$SEED_APP/." "$SEED/"; fi
    "$A" --app "$SEED" --rom "$ROM" --lockstep 10 > "$OUT/seed.txt" 2>&1 || true
    rm -rf "$SEED/logs"
fi
args=(--lockstep "$VIS")
[ -n "$INPUT" ] && args+=(--input "$INPUT")
for side in a b; do
    rm -rf "$OUT/$side"
    mkdir -p "$OUT/$side"
    cp -R "$SEED/." "$OUT/$side/"
done
"$A" --app "$OUT/a" "${args[@]}" > "$OUT/a.txt" 2>&1 &
pa=$!
"$B" --app "$OUT/b" "${args[@]}" > "$OUT/b.txt" 2>&1 &
pb=$!
wait $pa || true
wait $pb || true
grep -o 'vi=[0-9]* crc=[0-9A-F]*' "$OUT/a.txt" > "$OUT/a.crc" || true
grep -o 'vi=[0-9]* crc=[0-9A-F]*' "$OUT/b.txt" > "$OUT/b.crc" || true
na=$(wc -l < "$OUT/a.crc" | tr -d ' '); nb=$(wc -l < "$OUT/b.crc" | tr -d ' ')
first=$(diff "$OUT/a.crc" "$OUT/b.crc" | grep -m1 '^<' | sed 's/^< //' || true)
if [ -z "$first" ] && [ "$na" = "$nb" ] && [ "$na" -gt 0 ]; then
    echo "identical: $na CRCs (to vi=$(tail -1 "$OUT/a.crc" | sed 's/ .*//; s/vi=//'))"
else
    echo "differ: first at ${first:-<none>} (A has $na CRCs, B $nb); logs in $OUT/{a,b}.txt"
    exit 1
fi
