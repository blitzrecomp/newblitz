#!/bin/sh
# Publish the 2026 Season pak to the season server (docs/design/SEASON_PAK.md): the pak first, under its versioned name,
# then the catalog that names it, so a device never reads a catalog pointing at a file that isn't there yet.
#   make pak-2026 && HUB_UPLOAD_SECRET=<the Worker's UPLOAD_SECRET> tools/publish_pak.sh [server]
# server: the season server's public base (".../paks/"), default build/pak_server (the same line the app is built
# with). Uploads go to <host>/upload/paks/... with the secret; nothing here needs or prints the hub key.
# To withdraw the pak (the kill switch): edit build/paks/catalog.json ("available": false, or remove the entry) and run
# this with --catalog-only. Switches: "headshots": false / "live_rosters": false in the entry.
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
[ -f "$cat" ] || { echo "no $cat (make pak-2026)"; exit 2; }
file="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["paks"]["season2026"]["file"])' "$cat")"
if [ "$only_catalog" = 0 ]; then
    python3 tools/make_season_pak.py check "build/paks/$file"
    curl -fsS -X PUT -H "Authorization: Bearer $HUB_UPLOAD_SECRET" --data-binary "@build/paks/$file" "$upload$file"; echo
fi
curl -fsS -X PUT -H "Authorization: Bearer $HUB_UPLOAD_SECRET" --data-binary "@$cat" "${upload}catalog.json"; echo
echo "published: ${server}catalog.json"
