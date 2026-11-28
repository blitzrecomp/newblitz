#!/bin/bash
# Two screens on the Mac, start to finish: BLITZ DS's two windows (the upper screen and the game's own, the lower), driven
# into an Arcade game and shot as the Duo is held, upper above lower, at each moment that matters. For checking the
# two-screen layout (docs/SURFACE_DUO.md "Playtest rounds") without the Duo. No touch on the Mac: the controller look is
# Duo-only. Uses the Modern Mac app (make app-modern) and its dev data (build/appdata2026), like tools/live.sh.
#
#   tools/ds_harness.sh [options]
#     --stadium N     Rules → Stadium for the run (1 Day, 2 Night, 3 Snow, 4 Old day, 5 Old night, 6 Old snow, 7 City,
#                     8 Future, 9 Roman; 0 game decides). Put back as it was afterwards.
#     --plays N       how many play-select → snap → result cycles to shoot after the kickoff (default 3)
#     --tall K=V      a TALL dial for the run (repeatable): --tall msg_up=12 --tall panel=-21
#     --live CMD      any other live command once the game's up (repeatable): --live "PLAYART full" --live "BOARD -1"
#     --from MARK     skip the menus: load that saved mark (savestates/mark-<MARK>.state) and shoot from there
#     --keep          leave the app running afterwards (tools/live.sh stop ends it)
#     --size WxH      the game's (lower) window (default 900x675)
#     --second WxH    the upper window, when it isn't the same shape (a wide top: 1200x675 at Retroid Pocket Duo
#                     proportions with --size 902x676; AYN Thor: --size 680x592 --second 1204x677)
#     --no-marks      don't save the ds-kickoff / ds-line marks on the way
#
# Out: build/shots/ds/<time>/ with each moment's stacked shot (NN_<moment>.png), sheet.png (all of them on one page),
# and log.txt (the run's [tall] [board] [playart] [touch] lines). Marks saved on the way: ds-kickoff, ds-line.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
L=tools/live.sh
I=build/live/input.txt
LOG=build/live/app.log
STADIUM="" PLAYS=3 KEEP=0 FROM="" SIZE=900x675 SECOND="" MARKS=1
TALLS=() LIVES=()
while [ $# -gt 0 ]; do
    case "$1" in
    --stadium) STADIUM="$2"; shift 2 ;;
    --plays) PLAYS="$2"; shift 2 ;;
    --tall) TALLS+=("$2"); shift 2 ;;
    --live) LIVES+=("$2"); shift 2 ;;
    --from) FROM="$2"; shift 2 ;;
    --keep) KEEP=1; shift ;;
    --size) SIZE="$2"; shift 2 ;;
    --second) SECOND="$2"; shift 2 ;;
    --no-marks) MARKS=0; shift ;;
    *) sed -n '2,20p' "$0"; exit 1 ;;
    esac
done

OUT="$ROOT/build/shots/ds/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT" build/tools
if [ ! -x build/tools/ds_windows ] || [ tools/ds_windows.swift -nt build/tools/ds_windows ]; then
    swiftc -O tools/ds_windows.swift -o build/tools/ds_windows || exit 1
fi

say() { echo "[ds] $*"; }
send() { echo "$*" >> "$I"; }
count() { local n; n=$(grep -c -- "$1" "$LOG" 2>/dev/null); echo "${n:-0}"; }
# wait_for PATTERN SECONDS [BEFORE]: until the log has more PATTERN lines than BEFORE (default 0)
wait_for() {
    local pat="$1" secs="$2" before="${3:-0}" t=0
    while [ "$t" -lt $((secs * 4)) ]; do
        [ "$(count "$pat")" -gt "$before" ] && return 0
        sleep 0.25; t=$((t + 1))
    done
    return 1
}

N=0
# shot NAME: the upper window above the lower, as on the Duo held like a DS
shot() {
    N=$((N + 1))
    local name; name=$(printf "%02d_%s" "$N" "$1")
    local pid; pid=$(cat build/live/pid)
    local wins; wins=$(build/tools/ds_windows "$pid")
    local up low
    up=$(echo "$wins" | grep 'upper screen' | head -1 | cut -d' ' -f1)
    low=$(echo "$wins" | grep -v 'upper screen' | grep 'New Blitz' | head -1 | cut -d' ' -f1)
    [ -n "$low" ] || { say "no game window"; return 1; }
    screencapture -x -o -l "$low" "$OUT/_low.png"
    if [ -n "$up" ]; then
        screencapture -x -o -l "$up" "$OUT/_up.png"
    else
        rm -f "$OUT/_up.png"
    fi
    python3 - "$OUT" "$name" <<'PY'
import os, sys
from PIL import Image
out, name = sys.argv[1], sys.argv[2]
low = Image.open(f"{out}/_low.png").convert("RGB")
parts = [Image.open(f"{out}/_up.png").convert("RGB")] if os.path.exists(f"{out}/_up.png") else []
parts.append(low)
gap = 24  # the hinge
w = max(p.width for p in parts)
img = Image.new("RGB", (w, sum(p.height for p in parts) + gap * (len(parts) - 1)), (0, 0, 0))
y = 0
for p in parts:
    img.paste(p, ((w - p.width) // 2, y))
    y += p.height + gap
img.thumbnail((900, 1500))
img.save(f"{out}/{name}.png")
PY
}

# burst NAME COUNT GAP: COUNT shots GAP seconds apart (NAME_a, NAME_b, …)
burst() {
    local i=0 letters=abcdefghijklmnopqrstuvwxyz
    while [ "$i" -lt "$2" ]; do
        shot "$1_${letters:$i:1}"
        sleep "$3"
        i=$((i + 1))
    done
}

# Start (a running session is ended first: this is a fresh run).
$L stop >/dev/null 2>&1
BLITZ_FLAVOUR=2026 $L start BLITZ_DS=1 BLITZ_TALL_LOG=1 BLITZ_WINDOW_SIZE="$SIZE" ${SECOND:+BLITZ_DS_SECOND=$SECOND} >/dev/null || exit 1
say "started; out: $OUT"
wait_for "\[stamp\]" 60 || sleep 20
sleep 12
send "DSSTACK"  # one above the other, as the Duo is held
sleep 1

OLD_STADIUM=""
if [ -n "$STADIUM" ]; then
    OLD_STADIUM=$(python3 -c "
import json
names=['Game decides','Day','Night','Snow','Old day','Old night','Old snow','City','Future','Roman','None']
v=json.load(open('build/appdata2026/rules.json')).get('stadium','Game decides')
print(names.index(v) if v in names else 0)" 2>/dev/null || echo 0)
    send "CONFIG rules stadium $STADIUM"
    say "stadium $STADIUM for this run (was $OLD_STADIUM)"
fi
restore() {
    [ -n "$OLD_STADIUM" ] && send "CONFIG rules stadium $OLD_STADIUM" && sleep 1
    [ "$KEEP" = 1 ] || $L stop >/dev/null 2>&1
}
trap restore EXIT

for t in "${TALLS[@]+"${TALLS[@]}"}"; do send "TALL ${t%%=*} ${t#*=}"; done

if [ -n "$FROM" ]; then
    send "MARK load $FROM"
    wait_for "mark load: $FROM" 10 || { say "mark $FROM didn't load"; exit 1; }
    sleep 3
else
    # Title → Arcade → Play → controller select. Its first presses are often dropped, so press until team select shows.
    $L press START; sleep 3; shot title
    $L press A; sleep 3
    $L press A; sleep 3
    before=$(count "\[crew\] team select (")
    for try in 1 2 3 4 5; do
        $L press LEFT:0.6; sleep 1.2; $L press A; sleep 3
        [ "$(count "\[crew\] team select (")" -gt "$before" ] && break
    done
    [ "$(count "\[crew\] team select (")" -gt "$before" ] || { shot stuck; say "never reached team select"; exit 1; }
    shot team_select
    before=$(count "\[crew\] team select:")
    for try in 1 2 3 4; do
        $L press A:0.3; sleep 3
        [ "$(count "\[crew\] team select:")" -gt "$before" ] && break
    done
    sleep 3
    shot crew
    # Pick Your Crew is the app's own page: the arrow keys to Go Play, then Return.
    for k in Down Down Down Down Down Right Right Right Right Right Right Right; do send "KEY $k"; sleep 0.4; done
    before=$(count "loaded stadium")
    send "KEY Return"
    wait_for "loaded stadium" 30 "$before" || { shot stuck; say "the game didn't start"; exit 1; }
    for c in "${LIVES[@]+"${LIVES[@]}"}"; do send "$c"; done
    # VS screen, then the opening: rules, the flyover (both boards), the kickoff.
    sleep 2; shot vs
    sleep 4
    burst opening 10 0.7
    [ "$MARKS" = 1 ] && send "MARK save ds-kickoff"
    burst kickoff 6 1.0
fi

# Plays: at play select pick the first play (A), shoot the line (Play Art), snap (A), shoot the play and its result.
for p in $([ "$PLAYS" -gt 0 ] && seq 1 "$PLAYS"); do  # (BSD seq 1 0 counts down)
    sleep 2; shot "play${p}_select"
    $L press A; sleep 1.5; $L press A; sleep 2
    shot "play${p}_line"
    [ "$p" = 1 ] && [ "$MARKS" = 1 ] && send "MARK save ds-line"
    $L press A; sleep 1
    burst "play${p}_run" 3 0.8
    $L press B; sleep 0.6; $L press B
    burst "play${p}_result" 3 1.0
done

grep -E "^\[(tall|board|playart|touch|duo|ds)\]" "$LOG" | tail -400 > "$OUT/log.txt"
ls "$OUT"/[0-9]*.png >/dev/null 2>&1 && build/tools/contact_sheet "$OUT/sheet.png" 6 300 "$OUT"/[0-9]*.png 2>/dev/null
rm -f "$OUT/_up.png" "$OUT/_low.png"
say "done: $OUT ($N shots), sheet: $OUT/sheet.png"
