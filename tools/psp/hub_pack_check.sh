#!/bin/sh
# The live rosters hub as the PSP sees it (docs/PSP_PORT.md §6 "As built (rosters job)"): the manifest lists psp/pack.txt for
# the PSP only and the JSON files for the other platforms, the pack downloads over plain http:// with the manifest's SHA-1, and
# paths outside the Worker's PATH regex are refused. Read-only. Never prints the hub's address or key (build/rosters_hub).
#   tools/psp/hub_pack_check.sh            exit 0: all good
set -e
cd "$(dirname "$0")/../.."
[ -f build/rosters_hub ] || { echo "no build/rosters_hub"; exit 2; }
hub=$(tr -d '[:space:]' < build/rosters_hub)
hub=${hub%/}
plain="http://${hub#https://}"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail=0
code() { curl -s -o "${2:-/dev/null}" -w '%{http_code}' "$1"; }

[ "$(code "$hub/manifest.json" "$tmp/manifest.json")" = 200 ] || { echo "manifest: not 200"; exit 1; }
[ "$(code "$plain/psp/pack.txt" "$tmp/pack.txt")" = 200 ] || { echo "psp/pack.txt over http://: not 200"; : > "$tmp/pack.txt"; fail=1; }
python3 -I - "$tmp/manifest.json" "$tmp/pack.txt" <<'EOF' || fail=1
import hashlib, json, os, sys
m = json.load(open(sys.argv[1]))
files, targets = m.get("files", {}), m.get("targets", {})
print("manifest: latest %s, updated %s, %d files" % (m.get("latest"), m.get("updated"), len(files)))
ok = m.get("format") == "blitz-rosters/1" and "psp/pack.txt" in files
ok &= targets.get("psp/pack.txt", {}).get("platforms") == ["PSP"]
others = ["macOS", "Windows", "Linux", "iOS", "Android", "PS5"]
hand = [f for f in files if targets.get(f, {}).get("by_hand")]  # roster_season.py target --platforms: kept as set
bad = [f for f in files if f != "psp/pack.txt" and f not in hand and targets.get(f, {}).get("platforms") != others]
if hand: print("set by hand (not checked): " + " ".join(hand))
ok &= not bad
print("targets: psp/pack.txt -> %s; %s" % (targets.get("psp/pack.txt", {}).get("platforms"), "every JSON file -> " + " ".join(others) if not bad else "wrong for " + " ".join(bad)))
if os.path.getsize(sys.argv[2]):
    data = open(sys.argv[2], "rb").read()
    sha = hashlib.sha1(data).hexdigest()
    head = data.split(b"\n", 1)[0].decode(errors="replace")
    print("pack: %d bytes, %s; sha1 %s the manifest's" % (len(data), head, "matches" if sha == files.get("psp/pack.txt") else "DOESN'T MATCH"))
    ok &= sha == files.get("psp/pack.txt")
sys.exit(0 if ok else 1)
EOF
for p in psp/pack.json psp/sub/pack.txt pack.txt psp/.. upload/psp/pack.txt; do
    c=$(code "$hub/$p")
    [ "$c" = 404 ] || { echo "refuse $p: got $c"; fail=1; }
done
[ "$(code "${hub%/*}/not-the-key/psp/pack.txt")" = 404 ] || { echo "a wrong key wasn't refused"; fail=1; }
[ $fail = 0 ] && echo "hub: ok (out-of-regex paths and a wrong key refused)" || echo "hub: FAILED"
exit $fail
