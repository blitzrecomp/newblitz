#!/bin/bash
# One app with both games (docs/design/ONE_APP.md):
#
#   tools/make_bundle.sh OUT.app CLASSIC.app [MODERN.app] STARTER [SIGN_ID]
#
# OUT.app gets the starter as Contents/MacOS/BlitzRecomp, each engine's executable as Contents/MacOS/Classic and
# Contents/MacOS/Modern, and each engine's resources as Contents/Resources/Classic and Contents/Resources/Modern
# (the starter points the engine there with BLITZ_RESOURCES). Frameworks are shared (both engines link the same
# libraries; checked). Leave MODERN.app out (or pass "-") for Classic only: everything Modern then stays out
# (see docs/ZOINKITY.md). SIGN_ID: a codesigning identity, "-" (ad hoc) by default.
set -euo pipefail
OUT=$1; CLASSIC=$2; MODERN=${3:--}; STARTER=$4; SIGN_ID=${5:--}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

rm -rf "$OUT"
mkdir -p "$(dirname "$OUT")"
ditto "$CLASSIC" "$OUT"
C="$OUT/Contents"
mv "$C/MacOS/BlitzRecompiled" "$C/MacOS/Classic"
mkdir -p "$C/Resources.classic"
for f in "$C/Resources/"*; do mv "$f" "$C/Resources.classic/"; done
mv "$C/Resources.classic" "$C/Resources/Classic"
cp "$C/Resources/Classic/AppIcon.icns" "$C/Resources/AppIcon.icns"

if [ "$MODERN" != "-" ]; then
    cp "$MODERN/Contents/MacOS/BlitzRecompiled" "$C/MacOS/Modern"
    ditto "$MODERN/Contents/Resources" "$C/Resources/Modern"
    cp "$C/Resources/Modern/AppIcon.icns" "$C/Resources/AppIcon.icns"  # the app's icon: Modern's when it's in
    # Both engines must load the same frameworks.
    for f in "$MODERN/Contents/Frameworks/"*; do
        n=$(basename "$f")
        if [ ! -e "$C/Frameworks/$n" ]; then
            ditto "$f" "$C/Frameworks/$n"
        elif ! cmp -s "$f" "$C/Frameworks/$n" 2>/dev/null && [ ! -d "$f" ]; then
            echo "make_bundle: $n differs between the engines; rebuild both against the same libraries" >&2
            exit 1
        fi
    done
fi

# BRANDING: one-app bundle name BlitzRecomp (CFBundleName/DisplayName below)
cp "$STARTER" "$C/MacOS/BlitzRecomp"
P="$C/Info.plist"
/usr/libexec/PlistBuddy -c "Set :CFBundleExecutable BlitzRecomp" "$P"
/usr/libexec/PlistBuddy -c "Set :CFBundleName BlitzRecomp" "$P"
/usr/libexec/PlistBuddy -c "Set :CFBundleIdentifier local.blitzrecomp" "$P"
/usr/libexec/PlistBuddy -c "Add :CFBundleDisplayName string BlitzRecomp" "$P" 2>/dev/null || /usr/libexec/PlistBuddy -c "Set :CFBundleDisplayName BlitzRecomp" "$P"

FLAGS=(--options=runtime --sign "$SIGN_ID" --entitlements "$ROOT/platform/macos/entitlements_dist.plist" --force)
[ "$SIGN_ID" != "-" ] && FLAGS+=(--timestamp)
for exe in "$C/MacOS/Classic" "$C/MacOS/Modern"; do
    [ -e "$exe" ] && codesign "${FLAGS[@]}" "$exe"
done
codesign "${FLAGS[@]}" --deep "$OUT"
codesign --verify --deep --strict "$OUT"
echo "$OUT: $(ls "$C/MacOS" | tr '\n' ' ')"
