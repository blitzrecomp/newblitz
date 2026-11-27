#!/bin/bash
# A public release's downloads, from this Mac (make release VERSION=x.y.z; docs/RELEASE.md):
#
#   build/release/<version>/NewBlitz-v<version>-macOS-arm64.zip     New Blitz.app (ad hoc signed, as every build is today)
#                           NewBlitz-v<version>-Windows-x64.zip     the New Blitz folder: New Blitz.exe and what it loads
#                           NewBlitz-v<version>-Linux-x64.tar.gz    the New Blitz folder: the executable and its resources
# Each archive also holds recomplauncher.json beside the app (platform/recomplauncher: RecompLauncher's manifest), and
# the names are the ones RecompLauncher picks a download by (system and processor in the name).
# Then one obvious file per device (tools/release/make_downloads.sh, from the same staged folders):
#                           NewBlitz-v<version>-Windows-Setup.exe   installer: per user, Start menu, uninstaller
#                           NewBlitz-v<version>-macOS.dmg           New Blitz.app and an Applications alias
#                           NewBlitz-v<version>-x86_64.AppImage     SDL2 and FreeType inside (needs the AppImage runtime)
#                           NewBlitz-v<version>-Android.apk         the Android kit (RELEASE_NO_ANDROID=1: none)
#                           NewBlitz-v<version>-iPhone.ipa          the iPhone kit, unsigned
#                           PPSA99961.zip                           the PS5 title folder for ProsperoStore (the kit Blitz
#                                                                   Builder sends, no launch.elf; make_ps5_store_zip.sh)
#                           SHA256SUMS (every NewBlitz-* file and PPSA99961.zip), RELEASE_NOTES.md
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

# The pinned stable PS5 kit goes out as built, so it must be a RELEASE=1 build: a dev build carries this machine's paths
# and fails release_scan.sh after the long build (2026-11-17: 803, and 789 before it). Checked first.
STABLE_EBOOT="${PS5_STABLE:-build/ps5_stable/current}/PPSA99961/eboot.bin"
if [ -f "$STABLE_EBOOT" ]; then
    me=$(id -un)
    if strings -a "$STABLE_EBOOT" | grep -q -F -e "/Users/$me" -e "/home/$me" -e "${HOME%/}"; then
        echo "the pinned stable PS5 eboot ($STABLE_EBOOT) has this machine's build paths: pin a RELEASE=1 build"; exit 1
    fi
fi

# The Android kit must be signed with the release key, or Android won't update a copy installed from an earlier release
# (2026-11-24: 1.0.3's first run was a plain `make release` and its APK came out with this Mac's debug key). Checked
# before the long build. RELEASE_NO_ANDROID=1 leaves Android out instead.
if [ -z "${RELEASE_NO_ANDROID:-}" ] && { [ -z "${BLITZ_ANDROID_KEYSTORE:-}" ] || [ -z "${BLITZ_ANDROID_KEYSTORE_PASS:-}" ]; }; then
    echo "no Android release key: run the read -rs line in docs/RELEASE.md (it sets BLITZ_ANDROID_KEYSTORE and its password)"
    echo "and make release from it, or RELEASE_NO_ANDROID=1 to leave Android out"; exit 1
fi

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
# RELEASE_NO_ANDROID=1 leaves the Android kit out of every download (2026-11-15: a release key to remake; the Builder
# then says "This build has no Android kit").
KITS="ps5/PPSA99961/eboot.bin ps5/launch.elf ps5/STABLE.txt android/NewBlitz.apk ios/NewBlitz.ipa"
[ -n "${RELEASE_NO_ANDROID:-}" ] && KITS="ps5/PPSA99961/eboot.bin ps5/launch.elf ps5/STABLE.txt ios/NewBlitz.ipa"
for k in $KITS; do
    [ -f "build/devices/$k" ] || { echo "missing device kit: build/devices/$k"; exit 1; }
done
# The PS5 kit a release sends by default is the pinned stable one (build/ps5_stable/current, tested on a PS5; 2026-11-16:
# the team has no PS5 after that): its STABLE.txt names it, and its eboot must still be the one that passed.
want=$(sed -n 's/^sha256: //p' build/devices/ps5/STABLE.txt)
have=$(shasum -a 256 build/devices/ps5/PPSA99961/eboot.bin | cut -d' ' -f1)
[ -n "$want" ] && [ "$want" = "$have" ] || { echo "the stable PS5 kit isn't the eboot that passed on a PS5 (STABLE.txt sha256: ${want:-none})"; exit 1; }
echo "PS5 stable: $(head -1 build/devices/ps5/STABLE.txt)$([ -d build/devices/ps5-beta ] && echo ", plus this version's beta")"
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
[ -n "${RELEASE_NO_ANDROID:-}" ] && rm -rf "$R/devices/android"
for k in $KITS; do
    [ "$k" = ps5/launch.elf ] && continue
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
    [ -n "${RELEASE_NO_ANDROID:-}" ] && rm -rf "$d/devices/android"
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

# --- One file per device: installer, disk image, AppImage, phone apps -----------------------------------------------
tools/release/make_downloads.sh "$V" "$STAGE" "$OUT"

# --- PS5: the store zip ---------------------------------------------------------------------------------------------
# ProsperoStore's catalog (docs/PROSPEROSTORE.md) installs a .zip of the title folder from the GitHub release: the PS5
# kit the release sends by default (build/devices/ps5, the pinned stable one), PPSA99961/ at the top, no launch.elf or
# STABLE.txt, contentVersion from this version. One name for every release (the catalog's advice; each tag has its own
# URL, and the update bot finds the next release's file by the same name). Never replace it once a release is out.
say "PS5 store zip"
tools/release/make_ps5_store_zip.sh build/devices/ps5 "$V" "$OUT/PPSA99961.zip"

# --- Notes and checksums -----------------------------------------------------------------------------------------
say "notes"
python3 -I - "$V" CHANGELOG.md "$OUT" <<'EOF'
import os, re, sys
v, changelog, out = sys.argv[1:]
text = open(changelog, encoding="utf-8").read()
m = re.search(r"^## .*?$\n(.*?)(?=^## |\Z)", text, re.S | re.M)
body = m.group(1).strip() if m else ""
have = set(os.listdir(out))
# BRANDING: release notes. One line per download that was made: what it's for, and how to open it. The hub model (team,
# 2026-11-15): the computer app comes first and sets up every other device; the phone files are for installing by hand.
per_device = [
    (f"NewBlitz-v{v}-Windows-Setup.exe", "**Windows** (x64, Windows 10 or 11)",
     "run it; on the SmartScreen prompt, **More info → Run anyway**. It installs for you alone (no administrator), adds "
     "New Blitz to Start, and Settings → Apps removes it again (your saves stay)."),
    (f"NewBlitz-v{v}-macOS.dmg", "**Mac** (Apple Silicon, macOS 13 or later)",
     "open it and drag New Blitz to Applications. Open the app once (it's blocked), then **System Settings → Privacy & "
     "Security → Open Anyway**."),
    (f"NewBlitz-v{v}-x86_64.AppImage", "**Linux** (x86_64, untested)",
     "`chmod +x` it (or tick \"Allow executing\" in its properties) and run it. SDL2 and FreeType are inside; Vulkan "
     "comes from your graphics drivers."),
]
by_hand = [
    (f"NewBlitz-v{v}-Android.apk", "**Android** (phones and handhelds)",
     "open it on the device and allow installs from that app when asked, then open New Blitz and pick your ROM with "
     "**Pick a File Instead**."),
    ("PPSA99961.zip", "**PS5** (jailbroken: etaHEN, or ShadowMountPlus with an ELF loader)",
     "unzip it and copy the `PPSA99961` folder to `/data/homebrew` over FTP; ShadowMountPlus adds New Blitz to the home "
     "screen. Then add your ROM: Blitz Builder → PS5 from the computer app, or copy the ROM to `/data/blitzrecomp` over "
     "FTP. It's the PS5 build Blitz Builder sends."),
    (f"NewBlitz-v{v}-iPhone.ipa", "**iPhone / iPad**",
     "install it with Sideloadly (sideloadly.io, on a Mac or Windows; your own Apple ID, a free one works) and turn on "
     "its AutoRefresh (the circular-arrows button), so it renews itself over Wi-Fi. Then open New Blitz and pick your ROM "
     "with **Pick a File Instead**, or share it to New Blitz from the Files app. Steps: newblitz.com/start."),
]
lines = [f"- {who}: `{f}`. {how[0].upper() + how[1:]}" for f, who, how in per_device if f in have]
hand_lines = [f"- {who}: `{f}`. {how[0].upper() + how[1:]}" for f, who, how in by_hand if f in have]
android = f"NewBlitz-v{v}-Android.apk" in have
# What the computer app sets up from Blitz Builder → Phone or Handheld, PS5, N64 or Emulator.
targets = ["an Android phone or handheld (over USB)"] if android else []
targets += ["an iPhone (it makes the .ipa with your ROM inside)", "a PS5 with etaHEN", "an N64 or an emulator (a `.z64`)"]
archives = [f for f in (f"NewBlitz-v{v}-macOS-arm64.zip", f"NewBlitz-v{v}-Windows-x64.zip",
                        f"NewBlitz-v{v}-Linux-x64.tar.gz") if f in have]
notes = f"""# New Blitz {v}

{body}

## Downloads

**Get New Blitz for your computer.** It sets up your phone, handheld, PS5 or an N64/emulator export too: open
**Blitz Builder** and pick the device; your ROM and setup go with it, so the device starts ready to play. You need your
own NFL Blitz 2000 (USA) v1.1 cartridge dump: the app asks for it. No game files are included.

""" + "\n".join(lines) + f"""

From the computer app's Blitz Builder: {", ".join(targets[:-1])}, or {targets[-1]}.

The downloads aren't signed with a developer certificate, so your computer asks before opening one the first time: the
steps above.

### Advanced: install on a device by hand

No computer to set it up from? Install the device's file yourself, then pick your ROM on the device.

""" + "\n".join(hand_lines) + f"""

**To unpack instead of install** (and for RecompLauncher): {", ".join(f"`{a}`" for a in archives)}. Unpack the whole
folder and run the app inside; the Linux one uses the system's SDL2, FreeType and Vulkan. `SHA256SUMS` lists every
download's SHA-256.
"""
open(os.path.join(out, "RELEASE_NOTES.md"), "w", encoding="utf-8").write(notes)
EOF
(cd "$OUT" && shasum -a 256 NewBlitz-* PPSA99961.zip > SHA256SUMS)

say "scan"
tools/release_scan.sh "$OUT"

say "done: $OUT"
ls -la "$OUT"
cat "$OUT/SHA256SUMS"
# What goes on the GitHub release (nothing is uploaded here): every download, PPSA99961.zip and SHA256SUMS.
echo
echo "To publish (docs/RELEASE.md):"
echo "  gh release create v$V --repo blitzrecomp/newblitz --title \"New Blitz $V\" --notes-file $OUT/RELEASE_NOTES.md \\"
echo "    $(cd "$OUT" && ls NewBlitz-* PPSA99961.zip SHA256SUMS | sed "s#^#$OUT/#" | tr '\n' ' ')"
