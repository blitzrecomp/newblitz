#!/bin/bash
# Works a PSP (or PPSSPP) from the Mac (docs/PSP_PORT.md §5.2, §5.5). The same subcommands as tools/android/remote.sh:
#   remote.sh [--via host0|worker] cmd '<line>'      queue a command: ping | stat | log <name> [file] | ls <name> [dir] |
#                                                     get <name> <file> | shot <name> | set <key> <value> | peek <name> <a> <n> | exit
#   remote.sh [--via ...] res <name> [out]            a result, waiting up to 40 s for it (worker: res/psp-<name>)
#   remote.sh [--via ...] ping                        cmd ping, then its result
#   remote.sh [--via ...] shot <name>                 cmd shot <name>, then build/psp/shots/<name>.png
#   remote.sh [--via ...] seen                        when the PSP last looked (host0: its seen file; worker: seen?box=psp)
#   remote.sh [--via ...] list                        host0: run logs and results; worker: the uploaded files
#   remote.sh [--via ...] get <file|id> [out]         host0: a file under blitz-psp/ (run/<launch>.log, res/<name>); worker: an upload
# --via host0 (the default) uses build/psp/host0/blitz-psp/ (HOST0_DIR= for another): PSPLink's usbhostfs_pc build/psp/host0,
#   or PPSSPP through tools/psp/run_ppsspp.sh, which links the app folder's blitz-psp/ there. No signing: it is a cable.
# --via worker posts to the read key's mailbox with ?box=psp, aimed at the PSP's pid (@psp-<digits>, from seen; BLITZ_PSP_PID=
#   to pick one), signed: "<seq> <line> #<first 16 hex of HMAC-SHA256(cmd_key, '<seq>|<line>')>", seq from build/psp/cmd_seq.
#   The real Worker's read URL comes from build/blitz_logs_hub and the key from build/psp_cmd_key; neither is ever printed.
#   For tools/psp/mock_worker.py: PSP_READ_BASE=http://127.0.0.1:8788/mockread PSP_CMD_KEY_FILE=<file with the test key>.
# NB Remote (docs/PSP_PORT.md "NB Remote"; PSP/GAME/NBREMOTE polls as psp-remote-<digits>), --via worker only:
#   remote.sh --via worker stat                       NB Remote's stat (or the game's, if a game is what's polling)
#   remote.sh --via worker run <game>                 start classic|modern|probe|remote; a game that's running is told to
#                                                     exit first (home=remote brings it back to NB Remote), then run
#   remote.sh --via worker update <game> [--run]      upload build/psp/app_<game>/EBOOT.PBP (probe: build/psp/probe, remote:
#                                                     build/psp/nbremote; FILE= for another) to the Worker's file store as
#                                                     eboot-<game>.pbp, send the signed update with its sha256 and size, wait
#                                                     for NB Remote's verified result; --run then starts it
#   remote.sh --via worker rollback <game>            EBOOT.PBP.bak back in place
#   remote.sh --via worker restart [<game>]           a running game restarts itself (or switches); from NB Remote, run
#   remote.sh --via worker menu                       NB Remote goes back to the XMB
#   --to remote|game|<pid> picks who gets a cmd (default: NB Remote for run/update/rollback/menu/sendlog, else whoever polled
#   last). WAIT_RES=<s> waits longer for a result (default 40; update 300).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
VIA=host0 TO=""
while [ "${1:-}" = "--via" ] || [ "${1:-}" = "--to" ]; do
    if [ "$1" = "--via" ]; then VIA="$2"; else TO="$2"; fi
    shift 2
done
SUB="${1:-}"; shift || true
H0="${HOST0_DIR:-$ROOT/build/psp/host0/blitz-psp}"
PY="$ROOT/.venv/bin/python"; [ -x "$PY" ] || PY=python3

res_name() {   # the result name a command line writes: shot <n> -> shot-<n>; log/ls/get/peek <n> -> <n>; else the verb
    set -- $1
    case "$1" in shot) echo "shot-$2" ;; log|ls|get|peek) echo "$2" ;; *) echo "$1" ;; esac
}
remote_verb() { case "${1%% *}" in run|update|rollback|menu|sendlog) return 0 ;; *) return 1 ;; esac; }

if [ "$VIA" = worker ]; then
    if [ -n "${PSP_READ_BASE:-}" ]; then READ="$PSP_READ_BASE"; else . "$ROOT/build/blitz_logs_hub"; fi
    KEYFILE="${PSP_CMD_KEY_FILE:-$ROOT/build/psp_cmd_key}"
    SEQFILE="${PSP_SEQ_FILE:-$ROOT/build/psp/cmd_seq}"
fi

h0_cmd() {
    mkdir -p "$H0/cmd" "$H0/res"
    rm -f "$H0/res/$(res_name "$1")" "$H0/res/error"
    # the PSP runs the lowest seq first, so seqs must rise: microseconds (a random tail could put a later command first)
    local seq; seq="$("$PY" -I -c 'import time; print(time.time_ns() // 1000)')"
    printf '%s\n' "$1" > "$H0/cmd/$seq.tmp" && mv "$H0/cmd/$seq.tmp" "$H0/cmd/$seq.txt"
}
h0_res() {
    local out="${2:-/dev/stdout}"
    for _ in $(seq 1 80); do
        if [ -f "$H0/res/$1" ]; then cat "$H0/res/$1" > "$out"; return 0; fi
        if [ -f "$H0/res/error" ]; then cat "$H0/res/error" >&2; return 1; fi
        /bin/sleep 0.5
    done
    echo "no result $1" >&2; return 1
}

# the newest poller of a kind: remote (psp-remote-*), game (psp-<digits>), any; with its age in ms ("<pid> <ms>")
w_newest() {
    curl -fsS "$READ/seen?box=psp" | KIND="$1" "$PY" -I -c 'import json,os,sys
d=json.load(sys.stdin); p=d.get("polls_ago_ms") or {}; kind=os.environ["KIND"]
def ok(k):
    if not k.startswith("psp-"): return False
    if kind == "remote": return k.startswith("psp-remote-")
    if kind == "game": return not k.startswith("psp-remote-")
    return True
c=sorted((v,k) for k,v in p.items() if ok(k))
print(f"{c[0][1]} {int(c[0][0])}" if c else "")'
}
w_pid() {   # $1: the command line (picks NB Remote for its verbs)
    if [ -n "${BLITZ_PSP_PID:-}" ]; then echo "$BLITZ_PSP_PID"; return; fi
    local kind=any
    case "$TO" in remote|game) kind="$TO" ;; "") remote_verb "${1:-}" && kind=remote ;; *) echo "$TO"; return ;; esac
    w_newest "$kind" | cut -d' ' -f1
}
w_cmd() {
    [ -f "$KEYFILE" ] || { echo "no command key ($KEYFILE)" >&2; exit 2; }
    local seq=$(( $(cat "$SEQFILE" 2>/dev/null || echo 0) + 1 ))
    mkdir -p "$(dirname "$SEQFILE")"; echo "$seq" > "$SEQFILE"
    local pid; pid="$(w_pid "$1")"
    [ -n "$pid" ] || echo "no PSP has polled yet; the line goes to whichever asks first" >&2
    local tag; tag="$(KEYFILE="$KEYFILE" SEQ="$seq" LINE="$1" "$PY" -I -c 'import hmac,hashlib,os
k=open(os.environ["KEYFILE"]).read().strip().encode()
print(hmac.new(k,(os.environ["SEQ"]+"|"+os.environ["LINE"]).encode(),hashlib.sha256).hexdigest()[:16])')"
    printf '%s' "${pid:+@$pid }$seq $1 #$tag" | curl -fsS -X POST --data-binary @- "$READ/cmd?box=psp" >/dev/null
}
# The Worker keeps the last result under each name and can't delete it, so a result counts only once it differs from what was
# there when the command went out (w_mark).
w_mark() { curl -fsS "$READ/res/psp-$1" 2>/dev/null | shasum | cut -c1-40 > "$ROOT/build/psp/.res_mark_$1" || true; }
w_res() {   # a new res/psp-<name>, or a new res/psp-error (printed to stderr, status 1)
    local out="${2:-/dev/stdout}" tmp; tmp="$(mktemp)"
    local old; old="$(cat "$ROOT/build/psp/.res_mark_$1" 2>/dev/null || true)"
    local olderr; olderr="$(cat "$ROOT/build/psp/.res_mark_error" 2>/dev/null || true)"
    for _ in $(seq 1 "${WAIT_RES:-40}"); do
        if curl -fsS -o "$tmp" "$READ/res/psp-$1" 2>/dev/null && [ "$(shasum < "$tmp" | cut -c1-40)" != "$old" ]; then
            cat "$tmp" > "$out"; rm -f "$tmp" "$ROOT/build/psp/.res_mark_$1"; return 0
        fi
        if [ "$1" != error ] && curl -fsS -o "$tmp" "$READ/res/psp-error" 2>/dev/null && [ "$(shasum < "$tmp" | cut -c1-40)" != "$olderr" ]; then
            cat "$tmp" >&2; rm -f "$tmp" "$ROOT/build/psp/.res_mark_error"; return 1
        fi
        /bin/sleep 1
    done
    rm -f "$tmp"; echo "no new result psp-$1" >&2; return 1
}

w_send() { mkdir -p "$ROOT/build/psp"; w_mark "$(res_name "$1")"; w_mark error; w_cmd "$1"; }
docmd() { if [ "$VIA" = worker ]; then w_send "$1"; else h0_cmd "$1"; fi; }
dores() { if [ "$VIA" = worker ]; then w_res "$@"; else h0_res "$@"; fi; }

# NB Remote must be the one polling: a game that is (home=remote) is told to exit, which brings NB Remote back
need_remote() {
    local any; any="$(w_newest any)"
    case "$any" in psp-remote-*) return 0 ;; "") echo "nothing has polled the psp mailbox yet" >&2; return 1 ;; esac
    local age="${any#* }"
    if [ "${age:-999999}" -lt 30000 ]; then
        echo "a game is polling (${any%% *}): telling it to exit, so NB Remote comes back" >&2
        TO=game w_send exit
        WAIT_RES=30 w_res exit >/dev/null || echo "(no reply to exit; waiting for NB Remote anyway)" >&2
    fi
    for _ in $(seq 1 45); do
        any="$(w_newest any)"
        case "$any" in psp-remote-*) [ "${any#* }" -lt 15000 ] && return 0 ;; esac
        /bin/sleep 2
    done
    echo "NB Remote didn't poll within 90 s (is it running? data/net.cfg home=remote in the game?)" >&2
    return 1
}
eboot_for() {
    if [ -n "${FILE:-}" ]; then echo "$FILE"; return; fi
    case "$1" in
        classic) echo "$ROOT/build/psp/app_classic/EBOOT.PBP" ;; modern) echo "$ROOT/build/psp/app_modern/EBOOT.PBP" ;;
        probe) echo "$ROOT/build/psp/probe/EBOOT.PBP" ;; remote) echo "$ROOT/build/psp/nbremote/EBOOT.PBP" ;;
        *) echo "" ;;
    esac
}
worker_only() { [ "$VIA" = worker ] || { echo "$1 needs --via worker (NB Remote talks to the Worker only)" >&2; exit 2; }; }

case "$SUB" in
    cmd) docmd "$*" ;;
    stat) docmd stat; dores stat ;;
    run)
        worker_only run
        [ -n "${1:-}" ] || { echo "usage: remote.sh --via worker run <classic|modern|probe|remote>" >&2; exit 2; }
        need_remote; TO=remote w_send "run $1"; TO=remote w_res run ;;
    update)
        worker_only update
        G="${1:-}"; F="$(eboot_for "$G")"
        [ -n "$F" ] || { echo "usage: remote.sh --via worker update <classic|modern|probe|remote> [--run]" >&2; exit 2; }
        [ -f "$F" ] || { echo "no $F (build it first)" >&2; exit 2; }
        SHA="$(shasum -a 256 "$F" | cut -c1-64)"; SIZE="$(stat -f %z "$F")"; NAME="eboot-$G.pbp"
        HAVE="$(curl -fsS "$READ/file/$NAME/meta" 2>/dev/null | sed -n 's/^sha256=//p' || true)"
        if [ "$HAVE" = "$SHA" ]; then echo "the store already has $NAME ($SIZE bytes, ${SHA:0:16})" >&2
        else
            echo "uploading $F ($SIZE bytes, sha256 ${SHA:0:16}) as $NAME" >&2
            curl -fsS -X POST --data-binary @"$F" "$READ/file/$NAME" > /dev/null
            [ "$(curl -fsS "$READ/file/$NAME/meta" | sed -n 's/^sha256=//p')" = "$SHA" ] || { echo "the store's copy doesn't match" >&2; exit 1; }
        fi
        need_remote
        TO=remote w_send "update $G $NAME $SHA $SIZE"
        WAIT_RES="${WAIT_RES:-300}" w_res update
        if [ "${2:-}" = --run ]; then TO=remote w_send "run $G"; TO=remote w_res run; fi ;;
    rollback) worker_only rollback; need_remote; TO=remote w_send "rollback ${1:?game}"; TO=remote w_res rollback ;;
    menu) worker_only menu; need_remote; TO=remote w_send menu; TO=remote w_res menu ;;
    restart)
        worker_only restart
        case "$(w_newest any)" in
            psp-remote-*) [ -n "${1:-}" ] || { echo "NB Remote is up: say which game (restart <game>)" >&2; exit 2; }
                          TO=remote w_send "run $1"; TO=remote w_res run ;;
            *) TO=game w_send "restart ${1:-}"; TO=game w_res restart ;;
        esac ;;
    res) dores "$@" ;;
    ping) docmd ping; dores ping ;;
    shot)
        mkdir -p "$ROOT/build/psp/shots"
        if [ "$VIA" = worker ]; then w_mark "shot-$1"; w_cmd "shot $1"; w_res "shot-$1" "$ROOT/build/psp/shots/$1.raw"
        else h0_cmd "shot $1"; h0_res "shot-$1" "$ROOT/build/psp/shots/$1.raw"; fi
        "$PY" -I "$ROOT/tools/psp/shot2png.py" "$ROOT/build/psp/shots/$1.raw" "$ROOT/build/psp/shots/$1.png" ;;
    seen) if [ "$VIA" = worker ]; then curl -fsS "$READ/seen?box=psp"; echo; else cat "$H0/seen"; fi ;;
    list)
        if [ "$VIA" = worker ]; then curl -fsS "$READ/list" | "$PY" -I -c 'import json,sys
for r in json.load(sys.stdin): print(f"{r[\"bytes\"]:>9}  {r[\"device\"]:<10} {r[\"kind\"]:<8} {r[\"id\"]}")'
        else (cd "$H0" && ls -l run res 2>/dev/null); fi ;;
    get)
        if [ "$VIA" = worker ]; then curl -fsS -o "${2:-/dev/stdout}" "$READ/get/$1"
        else cat "$H0/$1" > "${2:-/dev/stdout}"; fi ;;
    *) sed -n '2,33p' "$0" >&2; exit 2 ;;
esac
