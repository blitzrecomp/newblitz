#!/bin/bash
# Build the core for every platform from this Mac (docs/PLATFORMS.md "One core, every device"): each target in turn,
# carrying on past a failure, logs in build/all/<target>.log, a table at the end. A platform whose toolchain isn't set
# up here is skipped, not failed.
#   tools/release/build_all.sh                 every platform
#   tools/release/build_all.sh ios android     only those (mac, windows, linux, ios, android, ps5)
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
mkdir -p build/all
want="${*:-mac windows linux ios android ps5}"

have() {  # is this platform's toolchain here?
    case "$1" in
        mac) true ;;
        windows) [ -d build/xwin/crt ] || [ -d build/xwin/redist ] ;;
        linux) [ -d build/linux/sysroot-x86_64/usr/include ] ;;
        ios) [ -f build/ios/prefix/lib/libSDL2.a ] ;;
        android) [ -d "${ANDROID_HOME:-$HOME/Library/Android/sdk}/ndk" ] ;;
        ps5) [ -d build/ps5/sdk-m ] ;;
    esac
}
# New Blitz only (team, 2026-10-25: Classic is the app's Original 1999 mode now); CLASSIC=1 builds the old Classic
# apps too (tools/regress builds its own reference copies either way).
targets() {
    local c=""
    case "$1" in
        mac) [ -n "${CLASSIC:-}" ] && c="app "; echo "${c}app-modern" ;;
        windows) [ -n "${CLASSIC:-}" ] && c="win-classic "; echo "${c}win-modern" ;;
        linux) [ -n "${CLASSIC:-}" ] && c="linux-classic "; echo "${c}linux-modern" ;;
        ios) [ -n "${CLASSIC:-}" ] && c="ios-classic "; echo "${c}ios-modern" ;;
        android) [ -n "${CLASSIC:-}" ] && c="android-classic "; echo "${c}android-modern" ;;
        ps5) [ -n "${CLASSIC:-}" ] && c="ps5-classic "; echo "${c}ps5-modern" ;;
    esac
}

results=()
for p in $want; do
    if ! have "$p"; then results+=("$p|-|skipped (toolchain not set up)"); continue; fi
    for t in $(targets "$p"); do
        start=$(date +%s)
        if make "$t" > "build/all/$t.log" 2>&1; then r=ok; else r=FAILED; fi
        results+=("$p|$t|$r ($(( $(date +%s) - start ))s)")
        [ "$r" = ok ] || grep -m3 -E "error:|Error [0-9]" "build/all/$t.log" | sed 's/^/    /'
    done
done
printf '\n%-8s %-16s %s\n' platform target result
for r in "${results[@]}"; do IFS='|' read -r p t s <<< "$r"; printf '%-8s %-16s %s\n' "$p" "$t" "$s"; done
! printf '%s\n' "${results[@]}" | grep -q FAILED
