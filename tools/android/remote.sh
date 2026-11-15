#!/bin/bash
# Works a test device from the Mac, on any network (platform/android/java/.../BlitzRemote.java):
#   remote.sh seen                        when the device last asked, and whether the game is running
#   remote.sh cmd <command line>          live A 0.2 | shot <name> | log <name> [file] | logcat <name> | ls <name> [dir] | get <name> <file>
#                                         put <name> <file> | mv <from> <to> | start | kill | restart
#   remote.sh res <name> [out]            a result, waiting up to 40 s for it
#   remote.sh file <name> <path>          a file for "put"
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT/build/blitz_logs_hub"
case "$1" in
    seen) curl -fsS "$READ/seen"; echo ;;
    cmd) shift  # BLITZ_REMOTE_PID=<pid from seen's polls_ago_ms>: only that process takes it, with two devices on the mailbox
        # Through stdin: curl would read a value starting with @ as a filename. Every line of a batch gets the name.
        printf '%s' "$*" | { if [ -n "${BLITZ_REMOTE_PID:-}" ]; then sed "s/^/@$BLITZ_REMOTE_PID /"; else cat; fi; } \
            | curl -fsS -X POST --data-binary @- "$READ/cmd" >/dev/null ;;
    file) curl -fsS -X POST --data-binary "@$3" "$READ/res/$2" >/dev/null ;;
    res)
        out=${3:-/dev/stdout}
        for _ in $(seq 1 40); do
            if curl -fsS -o "$out" "$READ/res/$2" 2>/dev/null; then exit 0; fi
            /bin/sleep 1
        done
        echo "no result $2" >&2; exit 1 ;;
    *) echo "see the top of $0" >&2; exit 2 ;;
esac
