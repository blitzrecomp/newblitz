#!/bin/sh
# A rosters hub on this computer (docs/PLATFORMS.md "Content and updates"): serves a blitz-rosters folder (the
# manifest, players.json, lineups) on the local network, so phones, consoles and other computers can follow it with no
# GitHub token. On each device, put the address it prints in <app folder>/rosters/hub (one line).
#   tools/rosters_hub.sh [folder, default build/blitz-rosters] [port, default 8790]
set -e
cd "$(dirname "$0")/.."
dir="${1:-build/blitz-rosters}"
port="${2:-8790}"
[ -f "$dir/manifest.json" ] || { echo "$dir has no manifest.json (tools/rosters_publish.sh clones blitz-rosters there)"; exit 2; }
ip="$(ipconfig getifaddr en0 2>/dev/null || hostname -I 2>/dev/null | cut -d' ' -f1)"
echo "rosters hub: http://${ip:-<this computer>}:$port/  (Ctrl-C stops it)"
exec python3 -m http.server "$port" --bind 0.0.0.0 --directory "$dir"
