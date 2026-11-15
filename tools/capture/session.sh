#!/bin/bash
# Trailer footage, the easy way: play, and the highlights come out as clips (docs/TRAILER_SHOTS.md).
#
#   tools/capture/session.sh [NAME]
#
# Opens New Blitz (your Modern build and data) in a 1920x1080 window on your main screen, records its picture and only
# its sound at 60 fps while you play, and when you quit the game (Cmd+Q, or the game's own Quit) cuts every touchdown,
# turnover, big gain and bookmark into its own clip (tools/capture/cut.py). Bookmark anything else (a late hit, a great
# catch) with a screenshot: L3 or R3 + Select, or F12.
#
# Don't move, resize or fullscreen the window, or turn TV Mode on, while it records: it records that 1920x1080 area.
#
# Output: ~/Movies/New Blitz Trailer/sessions/<date time> NAME/: session.mov (everything), events.jsonl, clips/.
# Env: BLITZ_CAPTURE_CODEC=prores (ProRes 422 instead of HEVC), BLITZ_CAPTURE_W/H (another picture size).
set -u
source "$(dirname "$0")/common.sh"
OUT="$HOME/Movies/New Blitz Trailer/sessions/$(date +%Y-%m-%d\ %H.%M) ${1:-session}"
capture_prepare "$OUT"
echo "Session: $OUT"
capture_start_game "$OUT"
capture_record "$OUT" || { L stop; exit 1; }
echo "Recording. Play; quit the game when you're done and the clips get cut."
GAME=$(cat "$BLITZ_LIVE_DIR/pid")
while kill -0 "$GAME" 2>/dev/null; do sleep 2; done
capture_finish "$OUT"
open "$OUT/clips" 2>/dev/null
