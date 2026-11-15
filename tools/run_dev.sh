#!/bin/bash
# Launch the dev build straight into the game, with all app data under build/appdata
# (config, saves, the stored ROM), so dev runs never touch ~/Library/Application Support.
#
# Usage: tools/run_dev.sh SECONDS [SHOT_SECONDS...]
#   SECONDS       stop the app after this long
#   SHOT_SECONDS  take a screenshot of the game window (only) at these times -> build/shots/
# Output: build/run.log, capped at 50 MB (the app is stopped if it logs more).
# BLITZ_APP=<build dir>, BLITZ_DATA=<app folder> and BLITZ_LOG=<file> override the defaults, e.g. to run
# a second instance alongside a play session without sharing its saves.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# BLITZ_FLAVOUR=2026 (or modern) runs Modern Blitz: the original game with 32 teams (build/app_modern, data in
# build/appdata2026, the plain ROM; docs/BLITZ2026.md "The original-game engine"). BLITZ_FLAVOUR=teamplus runs Modern
# on the Team Extension (build/app2026, ROM build/teamplus.z64 or BLITZ_ROM=<any ROM with the same code>).
if [ "${BLITZ_FLAVOUR:-}" = "teamplus" ]; then
    APP_DIR_DEFAULT="$ROOT/build/app2026"; DATA_DEFAULT="$ROOT/build/appdata2026"
    ROM_SRC="${BLITZ_ROM:-$ROOT/build/teamplus.z64}"; ROM_NAME="blitz2000.teamplus.n64.us.z64"; GAME_ID="blitz2026"
elif [ "${BLITZ_FLAVOUR:-}" = "2026" ] || [ "${BLITZ_FLAVOUR:-}" = "modern" ]; then
    APP_DIR_DEFAULT="$ROOT/build/app_modern"; DATA_DEFAULT="$ROOT/build/appdata2026"
    ROM_SRC="${BLITZ_ROM:-$ROOT/build/baserom.z64}"; ROM_NAME="blitz2000.n64.us.1.1.z64"; GAME_ID="blitz2026"
else
    APP_DIR_DEFAULT="$ROOT/build/app"; DATA_DEFAULT="$ROOT/build/appdata"
    ROM_SRC="${BLITZ_ROM:-$ROOT/build/baserom.z64}"; ROM_NAME="blitz2000.n64.us.1.1.z64"; GAME_ID="blitz2000"
fi
# Weekly rosters (src/game/rosters_live.cpp) come from the private blitz-rosters repo: dev runs read it with
# the gh login's token, passed in the environment only (never written to the app folder).
if [ -n "${BLITZ_FLAVOUR:-}" ] && [ "${BLITZ_FLAVOUR}" != "classic" ] && [ -z "${BLITZ_ROSTER_TOKEN:-}" ] && command -v gh >/dev/null; then
    BLITZ_ROSTER_TOKEN="$(gh auth token 2>/dev/null)"; export BLITZ_ROSTER_TOKEN
fi
APP="${BLITZ_APP:-$APP_DIR_DEFAULT}/BlitzRecompiled.app/Contents/MacOS/BlitzRecompiled"
DATA="${BLITZ_DATA:-$DATA_DEFAULT}"
LOG="${BLITZ_LOG:-$ROOT/build/run.log}"
SECS="${1:?usage: run_dev.sh SECONDS [SHOT_SECONDS...]}"; shift
SHOTS=" $* "
LOG_CAP=$((50 * 1024 * 1024))

[ -x "$APP" ] || { echo "No app build at $APP (run: make app)"; exit 1; }
python3 "$ROOT/tools/romtool.py" verify >/dev/null || exit 1
if [ ! -x "$ROOT/build/tools/winid" ]; then
    mkdir -p "$ROOT/build/tools"
    swiftc -O "$ROOT/tools/winid.swift" -o "$ROOT/build/tools/winid" || { echo "building tools/winid.swift failed"; exit 1; }
fi

mkdir -p "$DATA" "$ROOT/build/shots"
# The runtime keeps the ROM as <app folder>/<game_id>.z64 and validates it by hash.
# Always (re)copy, normalised to big-endian .z64, so an earlier BLITZ_ROM never lingers.
python3 "$ROOT/tools/romtool.py" normalize "$ROM_SRC" "$DATA/$ROM_NAME" || exit 1

BLITZ_QUIET_NOTICES="${BLITZ_QUIET_NOTICES-1}" APP_FOLDER_PATH="$DATA" "$APP" --game "$GAME_ID" > "$LOG" 2>&1 &
PID=$!
for ((t = 1; t <= SECS; t++)); do
    sleep 1
    kill -0 "$PID" 2>/dev/null || break
    if [ "$(stat -f %z "$LOG")" -gt "$LOG_CAP" ]; then
        echo "[run_dev] log over 50 MB, stopping" >> "$LOG"; break
    fi
    if [[ "$SHOTS" == *" $t "* ]]; then
        if WID=$("$ROOT/build/tools/winid" BlitzRecompiled "$PID"); then
            screencapture -x -o -l "$WID" "$ROOT/build/shots/t${t}s.png" && echo "[run_dev] shot build/shots/t${t}s.png" >> "$LOG"
        else
            echo "[run_dev] no game window for shot at ${t}s" >> "$LOG"
        fi
    fi
done
if kill -0 "$PID" 2>/dev/null; then
    kill "$PID"; sleep 1; kill -9 "$PID" 2>/dev/null
    echo "[run_dev] stopped after ${SECS}s" >> "$LOG"
else
    wait "$PID"; echo "[run_dev] app exited with status $?" >> "$LOG"
fi
