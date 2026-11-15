# Shared by tools/capture/session.sh and shoot.sh (sourced): the game for footage, and its recorder.
#
# The game runs through tools/live.sh (your Modern build and data, sound on, your controllers work as ever), so a script
# can size its window: 1920x1080, centred on the screen it opened on (the main one), widescreen, 60 fps. The recorder
# (record.swift) takes only that picture and only the game's sound.

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
export BLITZ_FLAVOUR="${BLITZ_FLAVOUR:-2026}"
export BLITZ_LIVE_DIR="$ROOT/build/live_capture"
export BLITZ_LIVE_SOUND=1
export BLITZ_LIVE_MAX="${BLITZ_LIVE_MAX:-14400}"
REC="$ROOT/build/tools/blitz_record"
PICTURE_W="${BLITZ_CAPTURE_W:-1920}"
PICTURE_H="${BLITZ_CAPTURE_H:-1080}"
APP_BIN="build/app_modern/BlitzRecompiled.app/Contents/MacOS/BlitzRecompiled"

L() { "$ROOT/tools/live.sh" "$@"; }

now_ms() { python3 -c 'import time; print(int(time.time() * 1000))'; }

# capture_prepare OUT_DIR: checks nothing else is running, builds the recorder if needed.
capture_prepare() {
    if pgrep -f "$APP_BIN" >/dev/null; then echo "New Blitz is already running: quit it first."; exit 1; fi
    mkdir -p "$ROOT/build/tools" "$1"
    if [ ! -x "$REC" ] || [ "$ROOT/tools/capture/record.swift" -nt "$REC" ]; then
        swiftc -O -swift-version 5 "$ROOT/tools/capture/record.swift" -o "$REC" || { echo "building the recorder failed"; exit 1; }
    fi
}

# capture_start_game OUT_DIR: the game, its events into OUT_DIR/events.jsonl, its window sized and centred.
capture_start_game() {
    L start "BLITZ_CAPTURE_EVENTS=$1/events.jsonl" >/dev/null || exit 1
    for _ in $(seq 1 60); do
        sleep 0.5
        "$ROOT/build/tools/winid" BlitzRecompiled "$(cat "$BLITZ_LIVE_DIR/pid")" >/dev/null 2>&1 && break
    done
    sleep 2
    L resize "$PICTURE_W" "$PICTURE_H" centre
    L gfx ar=Expand hr=Clamp16x9 rr=Manual rr_manual=60
    sleep 2
}

# capture_record OUT_DIR: records OUT_DIR/session.mov in the background until the game quits; sets REC_PID.
capture_record() {
    "$REC" --pid "$(cat "$BLITZ_LIVE_DIR/pid")" --out "$1/session.mov" --content "${PICTURE_W}x${PICTURE_H}" \
        --codec "${BLITZ_CAPTURE_CODEC:-hevc}" > "$1/recorder.log" 2>&1 &
    REC_PID=$!
    for _ in $(seq 1 20); do sleep 0.25; [ -f "$1/session.json" ] && return 0; done
    echo "the recorder didn't start: $(cat "$1/recorder.log")"; return 1
}

# capture_finish OUT_DIR: waits for the recording to close, then cuts the clips.
capture_finish() {
    wait "$REC_PID" 2>/dev/null
    [ -f "$1/session.mov" ] || { echo "no recording made"; return 1; }
    "$ROOT/.venv/bin/python" "$ROOT/tools/capture/cut.py" "$1" 2>/dev/null || python3 "$ROOT/tools/capture/cut.py" "$1"
}
