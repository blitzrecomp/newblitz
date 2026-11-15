#!/bin/bash
# NB Remote in headless PPSSPP against tools/psp/mock_worker.py (docs/PSP_PORT.md "NB Remote"): NB Remote boots and connects;
# a signed ping is answered, an unsigned and a replayed one refused; 'update classic' replaces NBCLASSIC's EBOOT on PPSSPP's
# memory stick with a verified copy of build/psp/app_classic/EBOOT.PBP (a wrong sha256 refused first); 'run classic' starts
# Classic; Classic's 'exit' brings NB Remote back (home=remote). NB Remote reaches the mock as localhost (its resolver path).
#   tools/psp/nbremote_ppsspp_test.sh [SECS]     (make psp-remote-ppsspp; needs make psp-remote and make psp-classic)
# Test keys only. NBCLASSIC's EBOOT starts as the copy already on the memory stick (or the kit's), and NBCLASSIC/data/net.cfg
# (test keys, home=remote) is removed afterwards. The mock runs on 127.0.0.1:8788.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SECS="${1:-240}"
MS="${MEMSTICK:-$HOME/.config/ppsspp}/PSP/GAME"
T="$ROOT/build/psp/nbremote_ppsspp"
NEW="$ROOT/build/psp/app_classic/EBOOT.PBP"
[ -f "$ROOT/build/psp/nbremote/EBOOT.PBP" ] && [ -f "$NEW" ] || { echo "make psp-remote and make psp-classic first" >&2; exit 2; }
[ -f "$MS/NBCLASSIC/rom/blitz.z64" ] || { echo "no Classic ROM on PPSSPP's stick (make psp-kit PPSSPP=1)" >&2; exit 2; }
if lsof -nP -iTCP:8788 -sTCP:LISTEN >/dev/null 2>&1; then echo "something already listens on 8788" >&2; exit 2; fi
rm -rf "$T"; mkdir -p "$T/store"
echo "nbremote-ppsspp-test-key-$RANDOM$RANDOM" > "$T/cmd_key"
KEY="$(cat "$T/cmd_key")"
mkdir -p "$MS/NBREMOTE/data" "$MS/NBCLASSIC/data"
rm -f "$MS/NBREMOTE/data/cmd_seq" "$MS/NBREMOTE/logs/remote.log" "$MS/NBREMOTE/data/last_run" "$MS/NBCLASSIC/data/cmd_seq" \
      "$MS/NBCLASSIC/EBOOT.PBP.bak" "$MS/NBCLASSIC/EBOOT.PBP.tmp"
printf 'logs_base=http://localhost:8788/mockpsp\ncmd_key=%s\nwifi_profile=6\ndevice=psp1001\npoll_s=2\n' "$KEY" > "$MS/NBREMOTE/data/net.cfg"
printf 'logs_base=http://127.0.0.1:8788/mockpsp\ncmd_key=%s\nwifi_profile=6\ndevice=psp1001\nhome=remote\n' "$KEY" > "$MS/NBCLASSIC/data/net.cfg"
# the "old" Classic: whatever EBOOT the stick has, unless it already is the new build (then a copy with one byte changed in its
# padding would not run, so use the kit's or the probe's EBOOT as the stand-in old one)
if [ -f "$MS/NBCLASSIC/EBOOT.PBP" ] && ! cmp -s "$MS/NBCLASSIC/EBOOT.PBP" "$NEW"; then cp "$MS/NBCLASSIC/EBOOT.PBP" "$T/old.pbp"
elif [ -f "$ROOT/build/psp/kit/PSP/GAME/NBCLASSIC/EBOOT.PBP" ] && ! cmp -s "$ROOT/build/psp/kit/PSP/GAME/NBCLASSIC/EBOOT.PBP" "$NEW"; then
    cp "$ROOT/build/psp/kit/PSP/GAME/NBCLASSIC/EBOOT.PBP" "$T/old.pbp"
else cp "$ROOT/build/psp/probe/EBOOT.PBP" "$T/old.pbp"; fi
cp "$T/old.pbp" "$MS/NBCLASSIC/EBOOT.PBP"

pass=0 fail=0
check() { if eval "$2"; then echo "PASS $1"; pass=$((pass+1)); else echo "FAIL $1"; fail=$((fail+1)); fi; }
MOCK=""
cleanup() {
    [ -n "$MOCK" ] && kill "$MOCK" 2>/dev/null
    rm -f "$MS/NBCLASSIC/data/net.cfg"   # test keys and home=remote off the stick again
}
trap cleanup EXIT
python3 -I "$ROOT/tools/psp/mock_worker.py" --store "$T/store" 2> "$T/mock.log" & MOCK=$!
sleep 1
STOP_ON_EXIT=0 FOLDER=NBREMOTE "$ROOT/tools/psp/run_ppsspp.sh" "$ROOT/build/psp/nbremote/EBOOT.PBP" "$SECS" > "$T/run.log" 2>&1 &
RUN=$!
export PSP_READ_BASE=http://127.0.0.1:8788/mockread PSP_CMD_KEY_FILE="$T/cmd_key" PSP_SEQ_FILE="$T/cmd_seq" WAIT_RES=40
R() { "$ROOT/tools/psp/remote.sh" --via worker "$@"; }
newest() { curl -fsS "$PSP_READ_BASE/seen?box=psp" | python3 -I -c 'import json,sys
p=json.load(sys.stdin).get("polls_ago_ms") or {}; k=sys.argv[1]
c=sorted((v,n) for n,v in p.items() if n.startswith(k) and (k!="psp-" or not n.startswith("psp-remote-")))
print(c[0][1] if c and c[0][0] < 12000 else "")' "$1"; }
wait_for() { for _ in $(seq 1 "$2"); do p="$(newest "$1")"; [ -n "$p" ] && { echo "$p"; return 0; }; sleep 1; done; return 1; }
LOG="$MS/NBREMOTE/logs/remote.log"

P1="$(wait_for psp-remote- 60)"; check "NB Remote boots in PPSSPP and polls ($P1)" '[ -n "$P1" ]'
out="$(R ping 2>&1)"; check "signed ping answered ($out)" '[[ "$out" == "pong remote eboot $P1 "* ]]'
seq_now="$(cat "$T/cmd_seq")"
tag_of() { KEY="$T/cmd_key" python3 -I -c 'import hmac,hashlib,os,sys
print(hmac.new(open(os.environ["KEY"]).read().strip().encode(),(sys.argv[1]+"|"+sys.argv[2]).encode(),hashlib.sha256).hexdigest()[:16])' "$1" "$2"; }
printf '%s' "@$P1 ping" | curl -fsS -X POST --data-binary @- "$PSP_READ_BASE/cmd?box=psp" > /dev/null
printf '%s' "@$P1 $seq_now ping #$(tag_of "$seq_now" ping)" | curl -fsS -X POST --data-binary @- "$PSP_READ_BASE/cmd?box=psp" > /dev/null
sleep 6
check "an unsigned and a replayed ping refused" 'grep -q "refused a command line (unsigned)" "$LOG" && grep -q "refused a command line (replayed or old seq)" "$LOG"'

# a wrong sha256: the store has the new EBOOT, the command names another hash
curl -fsS -X POST --data-binary @"$NEW" "$PSP_READ_BASE/file/eboot-classic.pbp" > /dev/null
out="$(R cmd "update classic eboot-classic.pbp $(printf '0%.0s' $(seq 1 64)) $(stat -f %z "$NEW")" 2>&1; R res update 2>&1)"
check "update with a wrong sha256 refused" 'grep -q "refused" <<<"$out" && cmp -s "$T/old.pbp" "$MS/NBCLASSIC/EBOOT.PBP"'
t0=$SECONDS
out="$(R update classic 2>&1)"; echo "  $(tail -1 <<<"$out")"
check "update classic: NBCLASSIC's EBOOT is the new build ($((SECONDS - t0)) s)" 'cmp -s "$NEW" "$MS/NBCLASSIC/EBOOT.PBP"'
check "  the old one kept as EBOOT.PBP.bak" 'cmp -s "$T/old.pbp" "$MS/NBCLASSIC/EBOOT.PBP.bak"'
out="$(R stat 2>&1)"
check "stat shows the new hash" 'grep -q "game classic NBCLASSIC EBOOT $(stat -f %z "$NEW") bytes sha256 $(shasum -a 256 "$NEW" | cut -c1-16)" <<<"$out"'
out="$(R run classic 2>&1)"; check "run classic accepted ($out)" 'grep -q "^launching classic" <<<"$out"'
G="$(wait_for psp- 120)"; check "Classic started and polls the mailbox ($G)" '[ -n "$G" ]'
out="$(R --to game ping 2>&1)"; check "Classic answers a signed ping ($(cut -c1-40 <<<"$out"))" '[[ "$out" == pong* ]]'
R --to game cmd exit > /dev/null 2>&1
P2="$(wait_for psp-remote- 90)"; check "Classic's exit brought NB Remote back ($P2)" '[ -n "$P2" ] && [ "$P2" != "$P1" ]'
RUNDIR="$ROOT/build/psp/runs/$(sed -n 's/^run \([0-9-]*\):.*/\1/p' "$T/run.log" | head -1)"
check "  Classic logged its way home" 'grep -q "home is NB Remote" "$RUNDIR/stdout.txt"'
out="$(R ping 2>&1)"; check "the new NB Remote answers ($out)" '[[ "$out" == "pong remote eboot $P2 "* ]]'
check "no key in any log" '! grep -q "$KEY" "$LOG" "$RUNDIR/stdout.txt" "$MS/NBCLASSIC/logs/blitz.log"'
kill "$RUN" 2>/dev/null; pkill -f "PPSSPPHeadless.*NBREMOTE" 2>/dev/null; wait "$RUN" 2>/dev/null
cp "$LOG" "$T/remote.log" 2>/dev/null; cp "$RUNDIR/stdout.txt" "$T/ppsspp_stdout.txt" 2>/dev/null
echo "nbremote-ppsspp: $pass passed, $fail failed (run $RUNDIR, logs in $T)"
[ "$fail" = 0 ]
