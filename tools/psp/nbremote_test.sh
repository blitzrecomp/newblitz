#!/bin/bash
# NB Remote's core on the Mac against tools/psp/mock_worker.py (docs/PSP_PORT.md "NB Remote"), driven by tools/psp/remote.sh
# exactly as the PSP would be: signed ping, unsigned / replayed / bad-tag lines refused, stat, update (verified, .bak kept, a bad
# sha256, a corrupted transfer and a full stick refused), rollback, ls/get/log and their path rules, run, menu.
#   tools/psp/nbremote_test.sh        (make psp-remote-test)
# Test keys only; everything lives in build/psp/nbremote_host/ and the mock runs on 127.0.0.1:8789.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T="$ROOT/build/psp/nbremote_host"
BIN="$T/nbremote-host"
PORT=8789
[ -x "$BIN" ] || { echo "no $BIN (make psp-remote-host)" >&2; exit 2; }
rm -rf "$T/stick" "$T/store" "$T/cmd_seq" "$T/out"
STICK="$T/stick"
G="$STICK/PSP/GAME"
mkdir -p "$G/NBCLASSIC/logs" "$G/NBREMOTE/data" "$G/NEWBLITZ" "$T/store"
pbp() { printf '\000PBP'; head -c $(($1 - 4)) /dev/urandom; }   # a fake EBOOT: the PBP magic, then noise
pbp 300000 > "$G/NBCLASSIC/EBOOT.PBP"; cp "$G/NBCLASSIC/EBOOT.PBP" "$T/old.pbp"
printf '[ 1.000] [main] a Classic log line\n' > "$G/NBCLASSIC/logs/blitz.log"
mkdir -p "$G/NBCLASSIC/data" && printf 'logs_base=http://127.0.0.1:%s/mockpsp\ncmd_key=secret\n' "$PORT" > "$G/NBCLASSIC/data/net.cfg"
pbp 1300000 > "$T/new.pbp"; head -c 400000 /dev/urandom > "$T/notpbp.bin"
echo "nbremote-test-key-0123456789abcdef" > "$T/cmd_key"
{ printf 'logs_base=http://127.0.0.1:%s/mockpsp\n' "$PORT"; printf 'cmd_key=%s\n' "$(cat "$T/cmd_key")"
  printf 'wifi_profile=1\ndevice=psp1001\npoll_s=1\n'; } > "$G/NBREMOTE/data/net.cfg"

pass=0 fail=0
check() { if eval "$2"; then echo "PASS $1"; pass=$((pass+1)); else echo "FAIL $1"; fail=$((fail+1)); fi; }

MOCK_PID="" HOST_PID=""
start_mock() { python3 -I "$ROOT/tools/psp/mock_worker.py" --port $PORT --store "$T/store" "$@" 2>>"$T/mock.log" & MOCK_PID=$!; sleep 0.7; }
cleanup() { [ -n "$HOST_PID" ] && kill "$HOST_PID" 2>/dev/null; [ -n "$MOCK_PID" ] && kill "$MOCK_PID" 2>/dev/null; }
trap cleanup EXIT
start_mock
"$BIN" --stick "$STICK" --secs 120 > "$T/out" 2> "$T/host.log" & HOST_PID=$!
sleep 1.5
PID="$(sed -n 's/^PID //p' "$T/out")"
export PSP_READ_BASE="http://127.0.0.1:$PORT/mockread" PSP_CMD_KEY_FILE="$T/cmd_key" PSP_SEQ_FILE="$T/cmd_seq" WAIT_RES=20
R() { "$ROOT/tools/psp/remote.sh" --via worker "$@"; }
LOG="$G/NBREMOTE/logs/remote.log"

check "pid is psp-remote-<digits> ($PID)" '[[ "$PID" =~ ^psp-remote-[0-9]+$ ]]'
check "the mailbox saw it poll" 'curl -fsS "$PSP_READ_BASE/seen?box=psp" | grep -q "$PID"'
out="$(R ping 2>&1)"; check "signed ping -> pong ($out)" '[[ "$out" == "pong remote host $PID "* ]]'

# refused lines: unsigned, a replay of the last signed one, a bad tag, a stale seq
seq_now="$(cat "$T/cmd_seq")"
tag() { KEY="$T/cmd_key" "$ROOT/.venv/bin/python" -I -c 'import hmac,hashlib,os,sys
print(hmac.new(open(os.environ["KEY"]).read().strip().encode(),(sys.argv[1]+"|"+sys.argv[2]).encode(),hashlib.sha256).hexdigest()[:16])' "$1" "$2"; }
raw() { printf '%s' "@$PID $1" | curl -fsS -X POST --data-binary @- "$PSP_READ_BASE/cmd?box=psp" > /dev/null; }
before="$(grep -c 'refused a command line' "$LOG")"
raw "ping"
raw "$seq_now ping #$(tag "$seq_now" ping)"                       # replay of the ping just sent
raw "$((seq_now + 50)) ping #0123456789abcdef"                    # bad tag
raw "$((seq_now + 51)) stat #$(tag "$((seq_now + 51))" ping)"     # a tag for another line
sleep 4
after="$(grep -c 'refused a command line' "$LOG")"
check "4 bad lines refused ($((after - before)))" '[ $((after - before)) -eq 4 ]'
check "  unsigned, replayed, bad tag all named in the log" 'grep -q "(unsigned)" "$LOG" && grep -q "(replayed or old seq)" "$LOG" && grep -q "(bad tag)" "$LOG"'
check "  the seq didn't move on a refused line" '[ "$(cat "$G/NBREMOTE/data/cmd_seq")" = "$seq_now" ]'

out="$(R stat 2>&1)"
check "stat names NBCLASSIC's EBOOT and its sha256" 'grep -q "game classic NBCLASSIC EBOOT 300000 bytes sha256 $(shasum -a 256 "$T/old.pbp" | cut -c1-16)" <<<"$out"'

# update: verified, .bak kept
out="$(FILE="$T/new.pbp" R update classic 2>&1)"; echo "  $out" | tail -1
check "update classic replaced the EBOOT with a verified copy" 'cmp -s "$T/new.pbp" "$G/NBCLASSIC/EBOOT.PBP"'
check "  the old one is EBOOT.PBP.bak" 'cmp -s "$T/old.pbp" "$G/NBCLASSIC/EBOOT.PBP.bak"'
check "  no .tmp left" '[ ! -e "$G/NBCLASSIC/EBOOT.PBP.tmp" ]'
check "  reply says verified" 'grep -q "^updated classic: NBCLASSIC/EBOOT.PBP is 1300000 bytes" <<<"$out"'
check "  256 KB pieces (5 of them)" 'grep -q "5 pieces" <<<"$out"'

# a bad sha256 in the command: refused before any download
out="$(R cmd "update classic eboot-classic.pbp $(printf 'a%.0s' $(seq 1 64)) 1300000" 2>&1; R res update 2>&1)"
check "update with a wrong sha256 refused ($(tail -1 <<<"$out" | cut -c1-70))" 'grep -q "refused: the store" <<<"$out" && cmp -s "$T/new.pbp" "$G/NBCLASSIC/EBOOT.PBP"'

# a byte flipped in transit: refused after the download, nothing changed
kill "$MOCK_PID"; wait "$MOCK_PID" 2>/dev/null
start_mock --corrupt-at 200000
curl -fsS -X POST --data-binary @"$T/old.pbp" "$PSP_READ_BASE/file/eboot-classic.pbp" > /dev/null   # the new mock's store is empty
sleep 2
out="$(FILE="$T/old.pbp" R update classic 2>&1)"
check "a corrupted transfer refused ($(tail -1 <<<"$out" | cut -c1-60))" 'grep -q "what arrived hashes to" <<<"$out" && cmp -s "$T/new.pbp" "$G/NBCLASSIC/EBOOT.PBP" && [ ! -e "$G/NBCLASSIC/EBOOT.PBP.tmp" ]'
kill "$MOCK_PID"; wait "$MOCK_PID" 2>/dev/null
start_mock
sleep 2

out="$(FILE="$T/notpbp.bin" R update classic 2>&1)"
check "update of a file that isn't a PBP refused ($(tail -1 <<<"$out" | cut -c1-50))" 'grep -q "isn.t a PBP" <<<"$out" && cmp -s "$T/new.pbp" "$G/NBCLASSIC/EBOOT.PBP" && [ ! -e "$G/NBCLASSIC/EBOOT.PBP.tmp" ]'

out="$(R rollback classic 2>&1)"
check "rollback put the old EBOOT back ($(cut -c1-50 <<<"$out"))" 'cmp -s "$T/old.pbp" "$G/NBCLASSIC/EBOOT.PBP" && cmp -s "$T/new.pbp" "$G/NBCLASSIC/EBOOT.PBP.bak"'

out="$(R cmd "log l1 classic" 2>&1; R res l1 2>&1)"; check "log l1 classic" 'grep -q "a Classic log line" <<<"$out"'
out="$(R cmd "ls l2" 2>&1; R res l2 2>&1)"; check "ls shows the NB folders" 'grep -q "NBCLASSIC/" <<<"$out" && grep -q "NBREMOTE/" <<<"$out"'
out="$(R cmd "ls l3 NBCLASSIC" 2>&1; R res l3 2>&1)"; check "ls NBCLASSIC" 'grep -q "EBOOT.PBP.bak" <<<"$out"'
out="$(R cmd "get g1 ms0:/PSP/GAME/NBCLASSIC/logs/blitz.log" 2>&1; R res g1 2>&1)"; check "get by ms0: path" 'grep -q "a Classic log line" <<<"$out"'
out="$(R cmd "get g2 NBREMOTE/data/net.cfg" 2>&1; R res g2 2>&1)"; check "get net.cfg refused" '! grep -q cmd_key <<<"$out" && grep -q "never net.cfg" <<<"$out"'
out="$(R cmd "get g3 NBCLASSIC/data/NET.CFG" 2>&1; R res g3 2>&1)"; check "get NET.CFG refused too" '! grep -q secret <<<"$out"'
out="$(R cmd "ls l4 ../.." 2>&1; R res l4 2>&1)"; check "ls outside refused" 'grep -q "only under" <<<"$out"'
out="$(R cmd "get g4 SAVEDATA/x" 2>&1; R res g4 2>&1)"; check "get outside NB* refused" 'grep -q "usage: get" <<<"$out"'
out="$(R cmd "bogus" 2>&1; R res bogus 2>&1)"; check "unknown verb" 'grep -q "unknown verb" <<<"$out"'
check "no key in NB Remote's log" '! grep -q "nbremote-test-key" "$LOG" && ! grep -q "mockpsp" "$LOG"'

out="$(R run classic 2>&1)"; sleep 1
check "run classic -> launching, and the launch happened" 'grep -q "^launching classic" <<<"$out" && grep -q "^LAUNCH $G/NBCLASSIC/EBOOT.PBP" "$T/out"'
check "  last_run recorded" 'grep -q "^classic at" "$G/NBREMOTE/data/last_run"'
out="$(R menu 2>&1)"; sleep 1
check "menu -> back to the XMB with xmb_hold" 'grep -q "^back to the XMB" <<<"$out" && grep -q "^MENU" "$T/out" && [ -f "$G/NBREMOTE/data/xmb_hold" ]'
kill "$HOST_PID"; wait "$HOST_PID" 2>/dev/null

# a stick without room: refused before any download
NBR_FREE_BYTES=500000 "$BIN" --stick "$STICK" --secs 25 > "$T/out" 2>> "$T/host.log" & HOST_PID=$!
sleep 1.5
out="$(FILE="$T/new.pbp" R update classic 2>&1)"
check "update refused on a full stick ($(tail -1 <<<"$out" | cut -c1-60))" 'grep -q "not enough space" <<<"$out" && cmp -s "$T/old.pbp" "$G/NBCLASSIC/EBOOT.PBP"'
kill "$HOST_PID" 2>/dev/null; wait "$HOST_PID" 2>/dev/null; HOST_PID=""

# a low battery: refused before any download
NBR_BATTERY_LOW=6 "$BIN" --stick "$STICK" --secs 25 > "$T/out" 2>> "$T/host.log" & HOST_PID=$!
sleep 1.5
out="$(FILE="$T/new.pbp" R update classic 2>&1)"
check "update refused on a low battery ($(tail -1 <<<"$out" | cut -c1-50))" 'grep -q "battery is at 6%" <<<"$out" && cmp -s "$T/old.pbp" "$G/NBCLASSIC/EBOOT.PBP"'
kill "$HOST_PID" 2>/dev/null; wait "$HOST_PID" 2>/dev/null; HOST_PID=""

# the power went between save_seq's remove and rename: only cmd_seq.tmp is left, and its seq still holds
D="$G/NBREMOTE/data"; last="$(cat "$D/cmd_seq")"; mv "$D/cmd_seq" "$D/cmd_seq.tmp"
"$BIN" --stick "$STICK" --secs 8 > "$T/out" 2>> "$T/host.log" & HOST_PID=$!
sleep 1.5
PID="$(sed -n 's/^PID //p' "$T/out")"
before="$(grep -c 'replayed or old seq' "$LOG")"
raw "$last ping #$(tag "$last" ping)"
sleep 3
check "a lost cmd_seq (only .tmp left) still refuses an old line" '[ "$(grep -c "replayed or old seq" "$LOG")" -gt "$before" ]'
kill "$HOST_PID" 2>/dev/null; wait "$HOST_PID" 2>/dev/null; HOST_PID=""

# the VSH plugin's decisions (nbr_plugin.c on the Mac): it starts NB Remote, or stays in the XMB when it should
PL="$T/nbremote-plugin-host"
if [ -x "$PL" ]; then
    PS="$T/plugin_stick"; rm -rf "$PS"; mkdir -p "$PS/PSP/GAME/NBREMOTE/data"; cp "$T/old.pbp" "$PS/PSP/GAME/NBREMOTE/EBOOT.PBP"
    PLOG="$PS/PSP/GAME/NBREMOTE/logs/plugin.log"
    cfg() { printf 'plugin_autostart=%s\nplugin_delay_s=3\nplugin_dry_run=%s\n' "$1" "$2" > "$PS/PSP/GAME/NBREMOTE/data/net.cfg"; }
    plug() { NBR_MS0="$PS" "$PL" 2>/dev/null; }
    cfg off 0; out="$(plug)"; check "plugin: autostart off stays in the XMB" '[ -z "$out" ]'
    cfg on 1; out="$(plug)"; check "plugin: dry run only logs" '[ -z "$out" ] && grep -q "dry run: would start" "$PLOG"'
    cfg on 0; out="$(plug)"; check "plugin: starts NB Remote ($out)" '[ "$out" = "LAUNCH $PS/PSP/GAME/NBREMOTE/EBOOT.PBP" ]'
    touch "$PS/PSP/GAME/NBREMOTE/data/xmb_hold"; out="$(plug)"
    check "plugin: xmb_hold keeps the XMB once" '[ -z "$out" ] && [ ! -e "$PS/PSP/GAME/NBREMOTE/data/xmb_hold" ]'
    out="$(NBR_HELD=1 plug)"; check "plugin: a button held keeps the XMB" '[ -z "$out" ] && grep -q "a button was pressed" "$PLOG"'
    plug > /dev/null; plug > /dev/null; out="$(plug)"
    check "plugin: a 4th start in 5 minutes refused (crash-loop guard)" '[ -z "$out" ] && grep -q "3 times in the last 5 minutes" "$PLOG"'
    rm -f "$PS/PSP/GAME/NBREMOTE/data/plugin_launches"; head -c 5000 /dev/urandom > "$PS/PSP/GAME/NBREMOTE/EBOOT.PBP"
    out="$(plug)"; check "plugin: an EBOOT that isn't a PBP isn't started" '[ -z "$out" ] && grep -q "not a PBP" "$PLOG"'
    : > "$PS/PSP/GAME/NBREMOTE/EBOOT.PBP"
    out="$(plug)"; check "plugin: an empty EBOOT isn't started" '[ -z "$out" ]'
    cp "$T/old.pbp" "$PS/PSP/GAME/NBREMOTE/EBOOT.PBP"
    rm -rf "$PS/PSP/GAME/NBREMOTE"; mkdir -p "$PS/PSP/GAME"; out="$(plug)"
    check "plugin: no NBREMOTE folder: nothing done, nothing written" '[ -z "$out" ] && [ ! -e "$PS/PSP/GAME/NBREMOTE" ]'
fi
echo "nbremote-test: $pass passed, $fail failed (logs: $T/host.log, $LOG)"
[ "$fail" = 0 ]
