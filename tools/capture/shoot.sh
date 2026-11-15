#!/bin/bash
# Trailer footage: the Settings shots, filmed by script (docs/TRAILER_SHOTS.md). Gameplay comes from session.sh.
#
#   tools/capture/shoot.sh
#
# Runs New Blitz on a copy of your data (build/capture_data, so your own settings and tournaments aren't touched) in a
# 1920x1080 window, opens each Settings page the trailer wants, records it, and cuts each into a named clip:
#   S10_qr_screen      Settings → Players with Phone Controllers on: the QR code
#   S12_bracket        Settings → Bracket with an eight-player tournament made
#   S20_rules          Settings → Rules: Quick Setup, Receiver Buttons, the house rules
# Output: ~/Movies/New Blitz Trailer/shots/<date time>/clips/. Hands off the keyboard and controllers while it runs (a
# minute or so).
set -u
source "$(dirname "$0")/common.sh"
export BLITZ_DATA="$ROOT/build/capture_data"
OUT="$HOME/Movies/New Blitz Trailer/shots/$(date +%Y-%m-%d\ %H.%M)"
capture_prepare "$OUT"
rsync -a --delete --exclude savestates --exclude "*.log" --exclude dumps --exclude packs/pac12 "$ROOT/build/appdata2026/" "$BLITZ_DATA/"
EV="$OUT/events.jsonl"

# shot NAME SECONDS WHAT: marks the next SECONDS of the recording as clip NAME, and waits them out.
shot() {
    sleep 1.5  # the page settles
    printf '{"t": %s, "kind": "shot", "name": "%s", "seconds": %s, "what": "%s"}\n' "$(now_ms)" "$1" "$2" "$3" >> "$EV"
    sleep "$2"
}

capture_start_game "$OUT"
sleep 8  # past the title and credits, to the game's own screens
capture_record "$OUT" || { L stop; exit 1; }

L config general phone_controllers true
L menu players
shot S10_qr_screen 10 "Settings → Players: phones scan the QR code"

L config party names TURBO,BLITZ,ROOKIE,HAMMER,NEON,BOLT,CHROME,ACE
sleep 0.5
L config party create true
L menu party
shot S12_bracket 8 "Settings → Bracket: an eight-player tournament"

L menu rules
shot S20_rules 8 "Settings → Rules"

L stop >/dev/null
capture_finish "$OUT"
open "$OUT/clips" 2>/dev/null
