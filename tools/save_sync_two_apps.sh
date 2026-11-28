#!/bin/bash
# Cloud Sync and Sync over Wi-Fi on one Mac (docs/SAVE_SYNC.md §7.7): two copies of the Mac app, each with its own app
# folder (build/sync_test/A and B: their own saves, settings and stored ROM), syncing through the save sync Worker's own
# code running on this computer (tools/save_sync_worker/test/local_server.mjs, R2 in memory). No Cloudflare account, no
# login, nothing leaves the computer. Both copies speak to the local server through BLITZ_SYNC_SERVER.
#
#   tools/save_sync_two_apps.sh [MINUTES]       (default 30; both copies and the server stop after that, or on Ctrl-C)
#
# Then, in A: play an Advanced week and save, Settings → General → Saves → Cloud Sync → Turn On → Add a Device: a 6-digit
# number. In B: Saves → Cloud Sync → Join, and type it (or Join with the Full Code, from A's Show Full Code). Sync over Wi-Fi works between the two as well (Send My Saves in one, Get Saves
# in the other: the code names this Mac's own network address, so it reaches the copy beside). docs/HUMAN_TESTS.md lists
# what to try. Logs: build/sync_test/a.log, b.log, server.log ([cloud], [wifi sync], [sync] lines).
#
# Not while another New Blitz is running (it would share the window server and the controllers): it stops if one is,
# unless FORCE=1.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MIN="${1:-30}"
OUT="$ROOT/build/sync_test"
if pgrep -f BlitzRecompiled > /dev/null && [ "${FORCE:-}" != 1 ]; then
    echo "New Blitz is already running: close it first (or FORCE=1)."; exit 1
fi
mkdir -p "$OUT/A" "$OUT/B"
SERVER=
if [ -n "${BLITZ_SYNC_SERVER:-}" ]; then
    echo "sync server: $BLITZ_SYNC_SERVER (given)"   # e.g. the deployed Worker, after tools/save_sync_worker/DEPLOY.md
else
    command -v node > /dev/null || { echo "node is needed for the local sync server"; exit 1; }
    node "$ROOT/tools/save_sync_worker/test/local_server.mjs" 0 > "$OUT/server.log" 2>&1 &
    SERVER=$!
    PORT=
    for i in $(seq 1 20); do PORT=$(sed -n 's/^listening //p' "$OUT/server.log"); [ -n "$PORT" ] && break; sleep 0.3; done
    [ -n "$PORT" ] || { echo "the local sync server didn't start (build/sync_test/server.log)"; kill $SERVER; exit 1; }
    echo "sync server: http://127.0.0.1:$PORT/ (R2 in memory: it forgets everything when it stops)"
    export BLITZ_SYNC_SERVER="http://127.0.0.1:$PORT/" SYNC_LOG=1
fi
export BLITZ_QUIET_NOTICES=0
SECS=$((MIN * 60))
BLITZ_FLAVOUR=modern BLITZ_DATA="$OUT/A" BLITZ_LOG="$OUT/a.log" "$ROOT/tools/run_dev.sh" "$SECS" &
A=$!
sleep 3
BLITZ_FLAVOUR=modern BLITZ_DATA="$OUT/B" BLITZ_LOG="$OUT/b.log" "$ROOT/tools/run_dev.sh" "$SECS" &
B=$!
trap 'kill $A $B $SERVER 2>/dev/null' INT TERM EXIT
echo "A: build/sync_test/A (log a.log)   B: build/sync_test/B (log b.log)   for $MIN minutes"
wait $A $B
