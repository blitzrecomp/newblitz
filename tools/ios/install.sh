#!/bin/bash
# Signs an iOS build (build/ios/app_<flavour>/BlitzRecompiled.app) for your Apple team and installs it on the connected
# iPhone, then launches it (docs/IOS_FEASIBILITY.md).
#   [BLITZ_IOS_TEAM=<team>] [BLITZ_IOS_BUNDLE_ID=<id>] [BLITZ_IOS_ENTITLEMENTS=<plist>] tools/ios/install.sh classic|modern [--no-launch]
# BLITZ_IOS_ENTITLEMENTS signs with other entitlements: platform/ios/FreeAccount.entitlements has only what a free
# Personal Team gets (no extended-virtual-addressing), so a paid team can try how a free account's install starts.
# The provisioning profile comes from a stub app with the same bundle ID and entitlements, built by Xcode with
# automatic signing (tools/ios/stub). The phone must be unlocked to launch.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
FLAVOUR=${1:?usage: tools/ios/install.sh classic|modern [--no-launch]}
TEAM=${BLITZ_IOS_TEAM:-$(cat "$ROOT/build/ios_team" 2>/dev/null || true)}
[ -n "$TEAM" ] || { echo "your Apple team ID: BLITZ_IOS_TEAM=<10 characters> or a file build/ios_team (Xcode → Settings → Accounts; docs/IOS_SIDELOAD.md)" >&2; exit 2; }
case $FLAVOUR in
    classic) BUNDLE_ID=com.blitzrecomp.newblitz.classic ;;
    modern) BUNDLE_ID=com.blitzrecomp.newblitz ;;
    *) echo "classic or modern" >&2; exit 2 ;;
esac
# Another bundle ID (BLITZ_IOS_BUNDLE_ID=com.you.newblitz): an Apple team can only sign IDs it owns, so anyone but the
# project's own team, a free Personal Team included, signs under their own. It installs as a separate app.
BUNDLE_ID=${BLITZ_IOS_BUNDLE_ID:-$BUNDLE_ID}
APP=$ROOT/build/ios/app_$FLAVOUR/BlitzRecompiled.app
[ -d "$APP" ] || { echo "no $APP: build it first" >&2; exit 2; }
/usr/libexec/PlistBuddy -c "Set :CFBundleIdentifier $BUNDLE_ID" "$APP/Info.plist"
DEVICE=${BLITZ_IOS_DEVICE:-$(xcrun devicectl list devices 2>/dev/null | grep -v simulated | grep -v unavailable | grep -i iphone | grep -oE '[0-9A-F]{8}-[0-9A-F]{16}' | head -1)}
[ -n "$DEVICE" ] || { echo "no paired iPhone connected" >&2; exit 2; }

# 1. The profile, from the stub (Xcode creates it the first time; later runs reuse it).
STUB=$ROOT/build/ios/stub-$FLAVOUR${BLITZ_IOS_BUNDLE_ID:+-$BLITZ_IOS_BUNDLE_ID}
cmake -S "$ROOT/tools/ios/stub" -B "$STUB" -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DBLITZ_IOS_TEAM=$TEAM -DBLITZ_IOS_BUNDLE_ID=$BUNDLE_ID \
    -DBLITZ_IOS_ENTITLEMENTS="${BLITZ_IOS_ENTITLEMENTS:-}" >/dev/null
xcodebuild -project "$STUB/BlitzStub.xcodeproj" -scheme BlitzStub -sdk iphoneos -configuration Debug \
    -destination "id=$DEVICE" -allowProvisioningUpdates build >"$STUB/xcodebuild.log" 2>&1 || { tail -20 "$STUB/xcodebuild.log"; exit 1; }
PROFILE=$STUB/Debug-iphoneos/BlitzStub.app/embedded.mobileprovision
[ -f "$PROFILE" ] || { echo "no profile from the stub build" >&2; exit 1; }

# 2. Sign: the profile in the bundle, the entitlements the profile allows (with the app's ID), the team's certificate.
security cms -D -i "$PROFILE" > "$STUB/profile.plist"
# The entitlements Xcode signed the stub with (no profile wildcards): the same app ID and capabilities.
codesign -d --entitlements - --xml "$STUB/Debug-iphoneos/BlitzStub.app" > "$STUB/entitlements.plist" 2>/dev/null
# The signing certificate: the one the profile names that's in this Mac's keychain.
IDENTITY=$(python3 -I - "$STUB/profile.plist" <<'PY'
import hashlib, plistlib, subprocess, sys
profile = plistlib.load(open(sys.argv[1], "rb"))
mine = subprocess.run(["security", "find-identity", "-v", "-p", "codesigning"], capture_output=True, text=True).stdout
for der in profile.get("DeveloperCertificates", []):
    sha = hashlib.sha1(der).hexdigest().upper()
    if sha in mine:
        print(sha)
        break
PY
)
[ -n "$IDENTITY" ] || { echo "none of the profile's certificates is in this keychain" >&2; exit 1; }
cp "$PROFILE" "$APP/embedded.mobileprovision"
codesign --force --sign "$IDENTITY" --entitlements "$STUB/entitlements.plist" --timestamp=none --generate-entitlement-der "$APP"
echo "signed $(basename "$APP") ($BUNDLE_ID) with $IDENTITY"

# 3. Install and launch.
xcrun devicectl device install app --device "$DEVICE" "$APP" | grep -E "App installed|bundleID|rror"
if [ "${2:-}" != "--no-launch" ]; then
    xcrun devicectl device process launch --device "$DEVICE" --terminate-existing "$BUNDLE_ID" | tail -1
fi
