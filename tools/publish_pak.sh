#!/bin/sh
# Publish the paks to the season server (docs/design/SEASON_PAK.md): every pak the catalog names that's in build/paks
# first, under its versioned name, then the catalog that names them, so a device never reads a catalog pointing at a file
# that isn't there yet. Since 1.0.6 the catalog may name several (the season, the Pac-12 and CFL league paks: make
# pak-2026, make pak-league), and every New Blitz that reads it installs and updates them by itself.
#   make pak-2026 && HUB_UPLOAD_SECRET=<the Worker's UPLOAD_SECRET> tools/publish_pak.sh [server]
# server: the season server's public base (".../paks/"), default build/pak_server (the same line the app is built
# with). Uploads go to <host>/upload/paks/... with the secret; nothing here needs or prints the hub key.
# To withdraw a pak (the kill switch): edit build/paks/catalog.json ("available": false, or remove the entry) and run
# this with --catalog-only. Switches: "headshots": false / "live_rosters": false in the season's entry; "auto": false in
# any entry offers it in Add-Ons (Get, Check) without pushing it to everyone.
set -eu
cd "$(dirname "$0")/.."
only_catalog=0
if [ "${1:-}" = "--catalog-only" ]; then only_catalog=1; shift; fi
server="${1:-$(cat build/pak_server 2>/dev/null || true)}"
[ -n "$server" ] || { echo "no server: pass it, or put it in build/pak_server"; exit 2; }
[ -n "${HUB_UPLOAD_SECRET:-}" ] || { echo "HUB_UPLOAD_SECRET isn't set"; exit 2; }
case "$server" in */paks/) ;; */paks) server="$server/" ;; *) echo "the server's address ends in /paks/"; exit 2 ;; esac
upload="${server%paks/}upload/paks/"
cat=build/paks/catalog.json
[ -f "$cat" ] || { echo "no $cat (make pak-2026, make pak-league)"; exit 2; }
# id and file of each entry, one a line.
entries="$(python3 -c 'import json,sys
for k, v in json.load(open(sys.argv[1]))["paks"].items(): print(k, v["file"])' "$cat")"
if [ "$only_catalog" = 0 ]; then
    echo "$entries" | while read -r id file; do
        [ -n "$file" ] || continue
        if [ ! -f "build/paks/$file" ]; then
            echo "$id: build/paks/$file isn't here: taken as already on the server (the Worker refuses the catalog if it isn't)"
            continue
        fi
        python3 tools/make_season_pak.py check "build/paks/$file"
        curl -fsS -X PUT -H "Authorization: Bearer $HUB_UPLOAD_SECRET" --data-binary "@build/paks/$file" "$upload$file"; echo
    done
fi
curl -fsS -X PUT -H "Authorization: Bearer $HUB_UPLOAD_SECRET" --data-binary "@$cat" "${upload}catalog.json"; echo
echo "published: ${server}catalog.json ($(echo "$entries" | cut -d' ' -f1 | tr '\n' ' '))"
