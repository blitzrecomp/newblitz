#!/bin/bash
# Runs a PSP EBOOT in PPSSPP as a PSP-1000 (docs/PSP_PORT.md §8), with its output captured:
#   tools/psp/run_ppsspp.sh [APP] [SECS]        APP: probe (default) | a path to an EBOOT.PBP; SECS: wall-clock cap (default 120)
# The EBOOT is copied to <memstick>/PSP/GAME/<folder>/ (NBPROBE for the probe), so it runs from ms0: as on the device and its
# app folder is ms0:/PSP/GAME/<folder>. host0: in PPSSPP is the --root folder, and PPSSPP refuses an EBOOT outside it, so --root
# is the EBOOT's own folder (argv[0] then reads ms0:/PSP/GAME/<folder>//EBOOT.PBP) and <folder>/blitz-psp is a symlink to
# build/psp/host0/blitz-psp: host0:/blitz-psp is that folder, as with PSPLink's usbhostfs_pc build/psp/host0, and
# tools/psp/remote.sh --via host0 works the same either way.
# Output: build/psp/runs/<stamp>/stdout.txt (PPSSPP's whole log), app.txt (the game's own lines), probe.txt (its PROBE lines),
# and a copy of the app folder's logs/ afterwards.
# Environment:
#   KILL_AT=<regex>   kill PPSSPP (SIGKILL, like pulling the battery) as soon as a game line matches, e.g. 'PROBE begin bench'
#   MEMSTICK=<dir>    PPSSPP's memory stick folder (default ~/.config/ppsspp, what the GUI build uses)
#   PPSSPP=<binary>   default build/psp/ppsspp/PPSSPPHeadless if built (no window), else the GUI PPSSPPSDL; HEADLESS=0: the GUI one
#   FORCE=1           run even if another PPSSPP is open (it may be the team's)
#   EXTRA_INI=<file>  more ppsspp.ini keys for this run only, merged after fat.ini (tools/psp/dlreplay.sh: the software
#                     renderer, so the GE's output is in emulated VRAM when the EBOOT reads its frame buffer)
#   WAIT=1            wait (polling every 20 s) until no other PPSSPP is open, instead of stopping
#   STOP_ON_EXIT=0    keep running after the game logs its exit (NB Remote's test: Classic's exit starts NB Remote again)
# APP=classic|modern also lays out the kit in the memory stick first (tools/psp/kit.sh --ppsspp: ROMs, sidecars, Modern's
# merged ROM, rosters).
# PPSSPP's own ppsspp.ini is backed up into the run folder and restored afterwards, whatever happens.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
APP="${1:-probe}"
SECS="${2:-120}"
# Headless PPSSPP (build/psp/ppsspp/PPSSPPHeadless, built 2026-11-04 from source: no window, real PSP speed with --realtime)
# when it's there, else the GUI build. HEADLESS=0 forces the GUI one.
HEADLESS_BIN="$ROOT/build/psp/ppsspp/PPSSPPHeadless"
if [ -z "${PPSSPP:-}" ] && [ -x "$HEADLESS_BIN" ] && [ "${HEADLESS:-1}" != 0 ]; then PPSSPP="$HEADLESS_BIN"; fi
PPSSPP="${PPSSPP:-/Applications/PPSSPPSDL.app/Contents/MacOS/PPSSPPSDL}"
case "$PPSSPP" in *PPSSPPHeadless) IS_HEADLESS=1 ;; *) IS_HEADLESS=0 ;; esac
MEMSTICK="${MEMSTICK:-$HOME/.config/ppsspp}"
INI="$MEMSTICK/PSP/SYSTEM/ppsspp.ini"

case "$APP" in
    probe) EBOOT="$ROOT/build/psp/probe/EBOOT.PBP"; FOLDER=NBPROBE ;;
    classic) EBOOT="$ROOT/build/psp/app_classic/EBOOT.PBP"; FOLDER=NBCLASSIC ;;
    modern) EBOOT="$ROOT/build/psp/app_modern/EBOOT.PBP"; FOLDER=NEWBLITZ ;;
    *) EBOOT="$APP"; FOLDER="${FOLDER:-NBTEST}" ;;
esac
[ -f "$EBOOT" ] || { echo "no EBOOT at $EBOOT (make psp-probe?)" >&2; exit 2; }
[ -x "$PPSSPP" ] || { echo "no PPSSPP at $PPSSPP" >&2; exit 2; }
# By process name: -f would also match any shell whose command line mentions PPSSPP (a wait loop) and never see it go.
while pgrep -x PPSSPPSDL >/dev/null && [ "${FORCE:-0}" != 1 ]; do
    if [ "${WAIT:-0}" != 1 ]; then echo "PPSSPP is already running (FORCE=1 to run anyway, WAIT=1 to wait for it)" >&2; exit 3; fi
    echo "another PPSSPP is open: waiting 20 s" >&2; sleep 20
done
if [ "$APP" = classic ] || [ "$APP" = modern ]; then MEMSTICK="$MEMSTICK" "$ROOT/tools/psp/kit.sh" --ppsspp >/dev/null; fi

STAMP="$(date +%Y%m%d-%H%M%S)"
RUN="$ROOT/build/psp/runs/$STAMP"
mkdir -p "$RUN" "$ROOT/build/psp/host0/blitz-psp/"{cmd,res,run} "$MEMSTICK/PSP/GAME/$FOLDER"
cp "$EBOOT" "$MEMSTICK/PSP/GAME/$FOLDER/EBOOT.PBP"
LINK="$MEMSTICK/PSP/GAME/$FOLDER/blitz-psp"
if [ ! -L "$LINK" ]; then rm -rf "$LINK"; ln -s "$ROOT/build/psp/host0/blitz-psp" "$LINK"; fi
rm -f "$ROOT/build/psp/host0/blitz-psp/cmd/"*.txt   # nothing stale for the new run
[ -f "$INI" ] && cp "$INI" "$RUN/ppsspp.ini.bak"

PID=""
cleanup() {
    if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then kill -9 "$PID" 2>/dev/null; wait "$PID" 2>/dev/null; fi
    [ -f "$RUN/ppsspp.ini.bak" ] && cp "$RUN/ppsspp.ini.bak" "$INI"
}
trap cleanup EXIT INT TERM
[ -f "$INI" ] && python3 -I "$ROOT/tools/psp/ini_merge.py" "$INI" "$ROOT/platform/psp/fat.ini"
[ -f "$INI" ] && [ -n "${EXTRA_INI:-}" ] && python3 -I "$ROOT/tools/psp/ini_merge.py" "$INI" "$EXTRA_INI"

if [ "$IS_HEADLESS" = 1 ]; then
    # Its own config is the memstick's PSP/SYSTEM/ppsspp.ini, so fat.ini goes in by --appendconfig only; the game's printf
    # comes out bare (no "stdout:" prefix).
    "$PPSSPP" --appendconfig="$ROOT/platform/psp/fat.ini" ${EXTRA_INI:+--appendconfig="$EXTRA_INI"} --memstick="$MEMSTICK" \
        --root="$MEMSTICK/PSP/GAME/$FOLDER" --realtime --timeout-wall="$((SECS + 5))" "$MEMSTICK/PSP/GAME/$FOLDER/EBOOT.PBP" \
        > "$RUN/stdout.txt" 2>&1 &
else
"$PPSSPP" --appendconfig="$ROOT/platform/psp/fat.ini" --root="$MEMSTICK/PSP/GAME/$FOLDER" "$MEMSTICK/PSP/GAME/$FOLDER/EBOOT.PBP" \
    > "$RUN/stdout.txt" 2>&1 &
fi
PID=$!
echo "run $STAMP: PPSSPP pid $PID, $FOLDER, up to $SECS s -> build/psp/runs/$STAMP"

app_lines() { if [ "$IS_HEADLESS" = 1 ]; then cat "$RUN/stdout.txt"; else sed -n 's/.*stdout: //p' "$RUN/stdout.txt"; fi; }
end=$((SECONDS + SECS))
how="timeout"
while [ $SECONDS -lt $end ]; do
    sleep 1
    if ! kill -0 "$PID" 2>/dev/null; then how="ppsspp exited"; break; fi
    if [ -n "${KILL_AT:-}" ] && app_lines | grep -Eq "$KILL_AT"; then
        kill -9 "$PID"; wait "$PID" 2>/dev/null; how="killed at /$KILL_AT/"; break
    fi
    # the game's exit path logs "[log] exit:" just before sceKernelExitGame; PPSSPP itself stays open, so stop it here
    if [ "${STOP_ON_EXIT:-1}" != 0 ] && app_lines | grep -q '\[log\] exit:'; then sleep 1; how="game exited"; break; fi
done
cleanup
PID=""
app_lines > "$RUN/app.txt"
grep -E '^PROBE ' "$RUN/app.txt" > "$RUN/probe.txt" || true
[ -s "$RUN/probe.txt" ] || rm -f "$RUN/probe.txt"
cp -R "$MEMSTICK/PSP/GAME/$FOLDER/logs" "$RUN/logs" 2>/dev/null || true
echo "run $STAMP: $how; $(wc -l < "$RUN/app.txt" | tr -d ' ') game lines$( [ -f "$RUN/probe.txt" ] && echo ", $(grep -c '^PROBE result' "$RUN/probe.txt") PROBE result lines")"
grep -E '^PROBE done' "$RUN/app.txt" || true
