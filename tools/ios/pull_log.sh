#!/bin/bash
# The iPhone's blitz.log (and the one before it) onto the Mac, for stutter numbers and crash notes:
#   tools/ios/pull_log.sh [modern|classic] [destination folder, default build/ios_logs]
# then tools/stutter_report.py build/ios_logs/modern-*.log. The phone must be connected (or paired) and unlocked.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
case "${1:-modern}" in
    classic) BUNDLE_ID=com.blitzrecomp.newblitz.classic; FOLDER=BlitzRecompiled ;;
    modern) BUNDLE_ID=com.blitzrecomp.newblitz; FOLDER=Blitz2026Recompiled ;;
    *) echo "usage: $0 [modern|classic] [folder]"; exit 1 ;;
esac
OUT="${2:-$ROOT/build/ios_logs}"
DEVICE=${BLITZ_IOS_DEVICE:-$(xcrun devicectl list devices 2>/dev/null | grep -v simulated | grep -v unavailable | grep -i iphone | grep -oE '[0-9A-F]{8}-[0-9A-F]{16}' | head -1)}
[ -n "$DEVICE" ] || { echo "No iPhone available: connect it and unlock it"; exit 1; }
mkdir -p "$OUT"
STAMP=$(date +%Y%m%d-%H%M)
for f in blitz.log blitz.prev.log; do
    dest="$OUT/${1:-modern}-$STAMP-$f"
    if xcrun devicectl device copy from --device "$DEVICE" --domain-type appDataContainer --domain-identifier "$BUNDLE_ID" \
        --source "Library/Application Support/$FOLDER/$f" --destination "$dest" >/dev/null 2>&1; then
        echo "$dest"
    fi
done
