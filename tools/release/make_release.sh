#!/bin/bash
# A public release's downloads, from this Mac (make release VERSION=x.y.z; docs/RELEASE.md):
#
#   build/release/<version>/NewBlitz-v<version>-macOS-arm64.zip     New Blitz.app (ad hoc signed, as every build is today)
#                           NewBlitz-v<version>-Windows-x64.zip     the New Blitz folder: New Blitz.exe and what it loads
#                           NewBlitz-v<version>-Linux-x64.tar.gz    the New Blitz folder: the executable and its resources
# Each archive also holds recomplauncher.json beside the app (platform/recomplauncher: RecompLauncher's manifest), and
# the names are the ones RecompLauncher picks a download by (system and processor in the name).
#                           SHA256SUMS, RELEASE_NOTES.md
#
# Every app is the public one (BLITZ_CONTENT=public: no licensed content; the 2026 Season pak brings it) and built with
# RELEASE=1 (no build-machine paths inside) at VERSION. Each computer's app carries the same device kits for Blitz
# Builder: the PS5 title, the Android APK and the iPhone app as an unsigned .ipa (never one signed for a team). Then
# tools/release_scan.sh checks every file of every download, and a hit fails the release.
#
#   tools/release/make_release.sh <version>
#   RELEASE_SKIP_BUILD=1   package what's built already (after a packaging fix)
# Logs: build/release/logs/. Nothing is uploaded or published.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
V=${1:?version}
OUT=build/release/$V
LOGS=build/release/logs
STAGE=build/release/stage
mkdir -p "$LOGS"
say() { printf '\n== %s\n' "$*"; }

if [ -z "${RELEASE_SKIP_BUILD:-}" ]; then
    # One make: the game's code is generated once for all of them. Device kits first: the Mac app takes them in.
    say "building (log: $LOGS/build.log)"
    if ! make BLITZ_CONTENT=public RELEASE=1 VERSION="$V" \
            ps5-modern android-modern ios-modern device-kits app-modern win-modern linux-modern > "$LOGS/build.log" 2>&1; then
        grep -E "error:|Error [0-9]|\*\*\*" "$LOGS/build.log" | head -20
        echo "build failed: $LOGS/build.log"; exit 1
    fi
fi

# The kits every computer's app carries: all three, built public.
[ "$(cat build/devices/content.txt 2>/dev/null)" = public ] || { echo "build/devices isn't the public kits (make device-kits BLITZ_CONTENT=public)"; exit 1; }
for k in ps5/PPSA99961/eboot.bin ps5/launch.elf android/NewBlitz.apk ios/NewBlitz.ipa; do
    [ -f "build/devices/$k" ] || { echo "missing device kit: build/devices/$k"; exit 1; }
done
[ ! -e build/devices/ios/NewBlitz.app.zip ] || { echo "build/devices has a team-signed iPhone app: never in a release"; exit 1; }

rm -rf "$STAGE" "$OUT"
mkdir -p "$STAGE" "$OUT"

# --- Mac ---------------------------------------------------------------------------------------------------------
say "Mac"
MAC=build/app_modern_public/BlitzRecompiled.app
[ -d "$MAC" ] || { echo "no $MAC"; exit 1; }
# BRANDING: the Mac app's name in the download
mkdir -p "$STAGE/mac"
ditto --norsrc --noextattr --noqtn "$MAC" "$STAGE/mac/New Blitz.app"
R="$STAGE/mac/New Blitz.app/Contents/Resources"
chmod -R u+rwX,go+rX "$STAGE/mac/New Blitz.app"
rm -f "$R/devices/content.txt"
for k in ps5/PPSA99961/eboot.bin android/NewBlitz.apk ios/NewBlitz.ipa; do
    [ -f "$R/devices/$k" ] || { echo "the Mac app lacks the device kit $k (make app-modern BLITZ_CONTENT=public after device-kits)"; exit 1; }
done
grep -q "<string>$V</string>" "$STAGE/mac/New Blitz.app/Contents/Info.plist" || { echo "the Mac app isn't version $V"; exit 1; }
# Ad hoc, as `make dist` signs (no developer's certificate: Gatekeeper asks once; README "Opening it the first time"),
# with the distribution entitlements (no debugger attach).
codesign --options=runtime --sign - --entitlements platform/macos/entitlements_dist.plist --deep --force "$STAGE/mac/New Blitz.app"
codesign --verify --deep --strict "$STAGE/mac/New Blitz.app"
cp platform/recomplauncher/recomplauncher.json "$STAGE/mac/"
# The folder's contents (the app and the manifest) at the zip's top: a lone top-level folder would be unwrapped by
# RecompLauncher, and the app's insides with it.
(cd "$STAGE/mac" && ditto -c -k --norsrc --noextattr --noqtn . "$ROOT/$OUT/NewBlitz-v$V-macOS-arm64.zip")

# --- Windows and Linux: the app folder, its resources and the device kits ------------------------------------------
stage_folder() {  # build dir, dest, exe name
    local b=$1 d=$2 exe=$3
    mkdir -p "$d"
    cp "$b/$exe" "$d/"
    for f in recompcontrollerdb.txt icon.png; do [ -f "$b/$f" ] && cp "$b/$f" "$d/"; done
    for f in assets packs art paks; do [ -d "$b/$f" ] && cp -R "$b/$f" "$d/"; done
    ditto build/devices "$d/devices" && rm -f "$d/devices/content.txt"
    # The public build carries no licensed content, whatever an earlier private build left in the folder.
    rm -rf "$d/packs/blitz2026" "$d/packs/pac12" "$d/houston" "$d/uniforms" "$d/announcer" "$d/probowl" "$d/rosters" \
        "$d/art/vs_right"
    chmod -R u+rwX,go+rX "$d"  # readable by whoever unpacks it (build/pak_server is private to this Mac's user)
}
say "Windows"
W=build/win_modern
[ -f "$W/New Blitz.exe" ] || { echo "no $W/New Blitz.exe"; exit 1; }
# BRANDING: the Windows and Linux folder name
stage_folder "$W" "$STAGE/windows/New Blitz" "New Blitz.exe"
cp "$W"/*.dll "$STAGE/windows/New Blitz/"
cp platform/recomplauncher/recomplauncher.json "$STAGE/windows/"
(cd "$STAGE/windows" && zip -qry -X "$ROOT/$OUT/NewBlitz-v$V-Windows-x64.zip" "New Blitz" recomplauncher.json)

say "Linux"
L=build/linux/app_modern
[ -f "$L/New Blitz" ] || { echo "no $L/New Blitz"; exit 1; }
stage_folder "$L" "$STAGE/linux/New Blitz" "New Blitz"
# Debug info out (most of its size, and the build's own paths); the code stays as built.
/opt/homebrew/opt/llvm/bin/llvm-strip --strip-debug "$STAGE/linux/New Blitz/New Blitz"
chmod 755 "$STAGE/linux/New Blitz/New Blitz"
cp platform/recomplauncher/recomplauncher.json "$STAGE/linux/"
(cd "$STAGE/linux" && tar --uid 0 --gid 0 --uname root --gname root -czf "$ROOT/$OUT/NewBlitz-v$V-Linux-x64.tar.gz" "New Blitz" recomplauncher.json)

# --- Notes and checksums -----------------------------------------------------------------------------------------
say "notes"
python3 -I - "$V" CHANGELOG.md "$OUT/RELEASE_NOTES.md" <<'EOF'
import re, sys
v, changelog, out = sys.argv[1:]
text = open(changelog, encoding="utf-8").read()
m = re.search(r"^## .*?$\n(.*?)(?=^## |\Z)", text, re.S | re.M)
body = m.group(1).strip() if m else ""
# BRANDING: release notes
notes = f"""# New Blitz {v}

{body}

## Downloads

- **Mac** (Apple Silicon, macOS 13 or later): `NewBlitz-v{v}-macOS-arm64.zip`
- **Windows** (x64): `NewBlitz-v{v}-Windows-x64.zip`
- **Linux** (x86_64): `NewBlitz-v{v}-Linux-x64.tar.gz`

Each is the computer app and the **Builder**: it sets the game up with your own NFL Blitz 2000 (USA) v1.1 ROM and sends
it to a PS5 (etaHEN) or an Android device, saves the iPhone app (.ipa) for you to install with your own Apple ID, or
exports a `.z64`. No game files are included. `SHA256SUMS` lists every download's SHA-256.

## Opening it the first time

The downloads aren't signed with a developer certificate, so your computer asks first:

- **Mac:** open the app once (it's blocked), then **System Settings → Privacy & Security → Open Anyway**.
- **Windows:** unzip the whole folder first; on the SmartScreen prompt, **More info → Run anyway**.
- **Linux:** `tar xzf` it, `chmod +x "New Blitz/New Blitz"` if needed, then run it. It uses the system's SDL2, FreeType
  and Vulkan.
- **iPhone / iPad:** Blitz Builder → Somewhere else → A console or phone → Phone → **Save the iPhone app (.ipa)**. Install
  `NewBlitz.ipa` with AltStore, SideStore or Sideloadly, signed with your own Apple ID (a free one lasts 7 days at a
  time), then copy the saved ROM and setup into New Blitz's folder (Files → On My iPhone → New Blitz). The README saved
  beside it has the steps.
"""
open(out, "w", encoding="utf-8").write(notes)
EOF
(cd "$OUT" && shasum -a 256 NewBlitz-* > SHA256SUMS)

say "scan"
tools/release_scan.sh "$OUT"

say "done: $OUT"
ls -la "$OUT"
cat "$OUT/SHA256SUMS"
