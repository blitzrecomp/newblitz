#!/bin/bash
# Drive a running dev session step by step: look at a screenshot, decide, press, look again.
# Same app data as run_dev.sh (build/appdata), so saves carry over between the two.
#
#   tools/live.sh start [ENV=VALUE...]   launch in the background (log: build/live/app.log)
#   tools/live.sh press WHAT [HOLD] ...  press buttons one after another, 0.35 s apart; each
#                                        WHAT is as in BLITZ_INPUT (A, DOWN, A+B, STICK -1,0; P2:A for player 2)
#                                        and may carry its own hold as WHAT:HOLD (A:0.5)
#   tools/live.sh wait SECONDS           let the game run
#   tools/live.sh shot [NAME] [WIDTH]    screenshot the game window -> build/shots/live/NAME.png,
#                                        plus NAME_small.png scaled to WIDTH (default 640) for review
#   tools/live.sh gfx KEY=VALUE ...      change graphics options in this session: ar=Original|Expand,
#                                        hr=Original|Clamp16x9|Full, rr=Original|Display|Manual, rr_manual=N
#   tools/live.sh dump                   RDRAM dump -> <app folder>/dumps/rdram_<t>.bin (+ .txt with the display list)
#   tools/live.sh pad -2 | +2            unplug / plug in virtual controller 2 (start with BLITZ_VIRTUAL_PADS=N)
#   tools/live.sh menu [TAB]             open Settings on a tab (general, controls, controllers, season, ...)
#   tools/live.sh savestate | loadstate  as F5 / F7: save / load the current slot
#   tools/live.sh quitprompt             as Cmd+Q: the quit prompt (Save this game? during a game)
#   tools/live.sh resize W H [centre]    resize the window, as dragging its corner (BLITZ_RESIZE_STRESS=<s> flips it); centre: on its screen
#   tools/live.sh restart                what Settings → Restart does (quit, then relaunch into the game)
#   tools/live.sh config TAB OPTION VALUE set a setting as its page would (true/false, enum index, number)
#   tools/live.sh stop
# The app is stopped automatically after 30 minutes (BLITZ_LIVE_MAX=<seconds>) or once its log passes 50 MB.
# BLITZ_APP=<build dir> and BLITZ_DATA=<app folder> pick another build or data folder (as in run_dev.sh).
# Instances start muted (BLITZ_MUTE=1); BLITZ_LIVE_SOUND=1 lets them play sound.
# Two instances at once: give the second its own BLITZ_LIVE_DIR (and BLITZ_DATA); every command then
# needs the same BLITZ_LIVE_DIR.
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
APP="${BLITZ_APP:-$APP_DIR_DEFAULT}/BlitzRecompiled.app/Contents/MacOS/BlitzRecompiled"
DATA="${BLITZ_DATA:-$DATA_DEFAULT}"
LIVE="${BLITZ_LIVE_DIR:-$ROOT/build/live}"   # BLITZ_LIVE_DIR=<dir>: a second instance with its own commands, pid and log
CMDS="$LIVE/input.txt"
LOG="$LIVE/app.log"
SHOTS="$ROOT/build/shots/live"
MAX_SECONDS=${BLITZ_LIVE_MAX:-1800}   # BLITZ_LIVE_MAX=<s> for long runs (a full played match)
LOG_CAP=$((50 * 1024 * 1024))

running() { [ -f "$LIVE/pid" ] && kill -0 "$(cat "$LIVE/pid")" 2>/dev/null; }

case "${1:-}" in
start)
    shift
    running && { echo "already running (pid $(cat "$LIVE/pid"))"; exit 0; }
    [ -x "$APP" ] || { echo "No app build at $APP (run: make app)"; exit 1; }
    python3 "$ROOT/tools/romtool.py" verify >/dev/null || exit 1
    if [ ! -x "$ROOT/build/tools/winid" ]; then
        mkdir -p "$ROOT/build/tools"
        swiftc -O "$ROOT/tools/winid.swift" -o "$ROOT/build/tools/winid" || { echo "building tools/winid.swift failed"; exit 1; }
    fi
    mkdir -p "$LIVE" "$SHOTS" "$DATA"
    # Always (re)copy, normalised to big-endian .z64, so an earlier BLITZ_ROM never lingers.
python3 "$ROOT/tools/romtool.py" normalize "$ROM_SRC" "$DATA/$ROM_NAME" || exit 1
    : > "$CMDS"
    # BLITZ_LIVE_LAUNCHER=1 stops at the launcher instead of starting the game.
    GAME_ARGS=(--game "$GAME_ID")
    [ -n "${BLITZ_LIVE_LAUNCHER:-}" ] && GAME_ARGS=()
    # Test instances are silent unless BLITZ_LIVE_SOUND=1 (the team may be in the room).
    MUTE=1; [ "${BLITZ_LIVE_SOUND:-}" = 1 ] && MUTE=0
    env BLITZ_QUIET_NOTICES=1 BLITZ_MUTE=$MUTE "$@" APP_FOLDER_PATH="$DATA" BLITZ_INPUT_LIVE="$CMDS" "$APP" ${GAME_ARGS[@]+"${GAME_ARGS[@]}"} > "$LOG" 2>&1 &
    PID=$!
    echo "$PID" > "$LIVE/pid"
    # Watchdog: lifetime and log size caps.
    (
        for ((t = 0; t < MAX_SECONDS; t++)); do
            sleep 1
            kill -0 "$PID" 2>/dev/null || exit 0
            if [ "$(stat -f %z "$LOG")" -gt "$LOG_CAP" ]; then
                echo "[live] log over 50 MB, stopping" >> "$LOG"; break
            fi
        done
        # SIGTERM alone can become a quit request the app ignores; follow up with SIGKILL.
        kill "$PID" 2>/dev/null; sleep 2; kill -9 "$PID" 2>/dev/null
    ) >/dev/null 2>&1 &
    echo "started pid $PID"
    ;;
press)
    shift
    running || { echo "not running"; exit 1; }
    first=1
    while [ $# -gt 0 ]; do
        what="$1"; shift
        hold=0.15
        if [[ "${what##*:}" =~ ^[0-9.]+$ && "$what" == *:* ]]; then hold="${what##*:}"; what="${what%:*}"; fi
        # STICK takes its x,y as the next argument.
        if [ "${what#P[1-4]:}" = "STICK" ]; then what="$what $1"; shift; fi
        [ $first -eq 1 ] || sleep 0.35
        first=0
        echo "$what $hold" >> "$CMDS"
        sleep "$hold"
    done
    ;;
wait)
    sleep "${2:-1}"
    ;;
shot)
    running || { echo "not running"; exit 1; }
    name="${2:-$(date +%H%M%S)}"
    WID=$("$ROOT/build/tools/winid" BlitzRecompiled "$(cat "$LIVE/pid")") || { echo "no game window"; exit 1; }
    screencapture -x -o -l "$WID" "$SHOTS/$name.png" || exit 1
    sips -Z "${3:-640}" "$SHOTS/$name.png" --out "$SHOTS/${name}_small.png" >/dev/null && echo "$SHOTS/${name}_small.png"
    ;;
gfx)
    shift
    running || { echo "not running"; exit 1; }
    echo "GFX $*" >> "$CMDS"
    ;;
dump)
    running || { echo "not running"; exit 1; }
    echo "DUMP" >> "$CMDS"
    ;;
pad)
    running || { echo "not running"; exit 1; }
    echo "PAD $2" >> "$CMDS"
    ;;
config)
    running || { echo "not running"; exit 1; }
    echo "CONFIG $2 $3 $4" >> "$CMDS"
    ;;
quit)
    running || { echo "not running"; exit 1; }
    echo "QUIT" >> "$CMDS"
    ;;
party)
    running || { echo "not running"; exit 1; }
    echo "PARTY" >> "$CMDS"
    ;;
quitprompt)
    running || { echo "not running"; exit 1; }
    echo "QUITPROMPT" >> "$CMDS"
    ;;
savestate|loadstate)
    running || { echo "not running"; exit 1; }
    echo "$1" | tr a-z A-Z >> "$CMDS"
    ;;
restart)
    running || { echo "not running"; exit 1; }
    echo "RESTART" >> "$CMDS"
    ;;
send)
    running || { echo "not running"; exit 1; }
    echo "SEND $2 ${3:-}" >> "$CMDS"
    ;;
resize)
    running || { echo "not running"; exit 1; }
    echo "RESIZE $2 $3 ${4:-}" >> "$CMDS"
    ;;
menu)
    running || { echo "not running"; exit 1; }
    echo "MENU ${2:-}" >> "$CMDS"
    ;;
stop)
    if running; then kill "$(cat "$LIVE/pid")"; sleep 1; kill -9 "$(cat "$LIVE/pid")" 2>/dev/null; echo "stopped"; else echo "not running"; fi
    rm -f "$LIVE/pid"
    ;;
*)
    sed -n '2,19p' "$0"; exit 1
    ;;
esac
