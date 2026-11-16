#!/bin/bash
# One obvious download per device, made from make_release.sh's staged folders (docs/RELEASE.md), beside the zips and
# the tarball RecompLauncher installs from:
#
#   NewBlitz-v<v>-Windows-Setup.exe   NSIS installer (platform/windows/installer.nsi): per user, no admin prompt
#   NewBlitz-v<v>-macOS.dmg           New Blitz.app (as signed in the zip) and an Applications alias, in a styled window
#                                     (dmgbuild: platform/macos/dmg_settings.py, tools/make_dmg_background.py)
#   NewBlitz-v<v>-x86_64.AppImage     the Linux folder with SDL2 and FreeType inside; Vulkan from the system. Needs the
#                                     AppImage runtime (below); without it the AppImage is skipped, loudly.
#   NewBlitz-v<v>-Android.apk         the Android device kit, as signed by the build (BLITZ_ANDROID_KEYSTORE)
#   NewBlitz-v<v>-iPhone.ipa          the iPhone device kit: unsigned, for Sideloadly (or AltStore, SideStore)
#
#   tools/release/make_downloads.sh <version> <stage> <out>
#     <stage>: make_release.sh's build/release/stage (mac/, windows/, linux/); scratch work goes in <stage>/downloads
#     RELEASE_NO_ANDROID=1   no .apk
#     APPIMAGE_RUNTIME=<file> the AppImage type 2 runtime for x86_64 (default build/appimage/runtime-x86_64); its
#                            SHA-256 must match tools/deps.sha256's "appimage-runtime-x86_64" line when that line exists
# Tools: makensis and mksquashfs and patchelf (brew install makensis squashfs patchelf), hdiutil, llvm-readelf, and
# dmgbuild and Pillow in .venv (make venv: requirements.txt).
# Nothing is uploaded.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
V=${1:?version}; STAGE=${2:?stage folder}; OUT=${3:?output folder}
STAGE=$(cd "$STAGE" && pwd); mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
W="$STAGE/downloads"
rm -rf "$W"; mkdir -p "$W"
DEVICES=${DEVICES:-build/devices}
LLVM=/opt/homebrew/opt/llvm/bin
say() { printf '\n== %s\n' "$*"; }
need() { command -v "$1" >/dev/null || { echo "missing $1: $2"; exit 1; }; }
need makensis "brew install makensis"
need mksquashfs "brew install squashfs"
need patchelf "brew install patchelf"
need hdiutil "macOS"
[ -x .venv/bin/dmgbuild ] || { echo "missing .venv/bin/dmgbuild: make venv (requirements.txt)"; exit 1; }

# --- Phones: the device kits, renamed --------------------------------------------------------------------------------
say "phones"
IPA="$DEVICES/ios/NewBlitz.ipa"
[ -f "$IPA" ] || { echo "no $IPA"; exit 1; }
# Unsigned: no provisioning profile and no team in the app (make_release.sh refuses a team kit; this checks the file).
mkdir -p "$W/ipa" && ditto -x -k "$IPA" "$W/ipa"
app=$(find "$W/ipa/Payload" -maxdepth 1 -name '*.app' | head -1)
[ -n "$app" ] || { echo "$IPA has no Payload/*.app"; exit 1; }
[ ! -e "$app/embedded.mobileprovision" ] || { echo "$IPA holds a provisioning profile: not the unsigned kit"; exit 1; }
team=$(codesign -dv "$app" 2>&1 | sed -n 's/^TeamIdentifier=//p')
[ -z "$team" ] || [ "$team" = "not set" ] || { echo "$IPA is signed for a team"; exit 1; }
cp "$IPA" "$OUT/NewBlitz-v$V-iPhone.ipa"
echo "NewBlitz-v$V-iPhone.ipa (unsigned)"

if [ -z "${RELEASE_NO_ANDROID:-}" ]; then
    APK="$DEVICES/android/NewBlitz.apk"
    [ -f "$APK" ] || { echo "no $APK"; exit 1; }
    BT=$(ls -d "${ANDROID_HOME:-$HOME/Library/Android/sdk}"/build-tools/* 2>/dev/null | sort -V | tail -1)
    [ -x "$BT/apksigner" ] || { echo "no apksigner in the Android SDK's build-tools"; exit 1; }
    signer=$("$BT/apksigner" verify --print-certs "$APK" | sed -n 's/^Signer #1 certificate SHA-256 digest: //p' | head -1)
    [ -n "$signer" ] || { echo "$APK doesn't verify"; exit 1; }
    if [ -n "${BLITZ_ANDROID_KEYSTORE:-}" ]; then
        # The APK must be signed with the release key, or Android won't update players' copies later.
        want=$(keytool -list -v -keystore "$BLITZ_ANDROID_KEYSTORE" -storepass:env BLITZ_ANDROID_KEYSTORE_PASS \
            -alias "${BLITZ_ANDROID_KEYSTORE_ALIAS:-newblitz}" 2>/dev/null | sed -n 's/^[[:space:]]*SHA256: //p' | tr -d ':' | tr 'A-F' 'a-f')
        [ -n "$want" ] || { echo "couldn't read the certificate from BLITZ_ANDROID_KEYSTORE (password, alias?)"; exit 1; }
        [ "$want" = "$signer" ] || { echo "$APK isn't signed with BLITZ_ANDROID_KEYSTORE's key: rebuild it (make android-modern device-kits)"; exit 1; }
        echo "NewBlitz-v$V-Android.apk (the release key)"
    else
        echo "NewBlitz-v$V-Android.apk: WARNING, BLITZ_ANDROID_KEYSTORE isn't set, so this is whatever key built it (this Mac's debug key?)"
    fi
    cp "$APK" "$OUT/NewBlitz-v$V-Android.apk"
fi

# --- Mac: the disk image ---------------------------------------------------------------------------------------------
say "Mac disk image"
MACAPP="$STAGE/mac/New Blitz.app"
[ -d "$MACAPP" ] || { echo "no $MACAPP"; exit 1; }
codesign --verify --deep --strict "$MACAPP"
mkdir -p "$W/dmg"
ditto --norsrc --noextattr --noqtn "$MACAPP" "$W/dmg/New Blitz.app"
DMG="$OUT/NewBlitz-v$V-macOS.dmg"
# The window: navy background with the logo and an arrow (made here, 1x and 2x), the app at left, Applications at
# right, the app's icon as the volume's. dmgbuild writes the Finder's .DS_Store itself: no Finder scripting, no prompt.
.venv/bin/python tools/make_dmg_background.py "$W/dmg_bg" > /dev/null
rm -f "$DMG"
# BRANDING: the disk image's volume name
.venv/bin/dmgbuild -s platform/macos/dmg_settings.py -D app="$W/dmg/New Blitz.app" -D background="$W/dmg_bg/background.png" \
    "New Blitz" "$DMG" > "$W/dmgbuild.log" 2>&1 || { cat "$W/dmgbuild.log"; exit 1; }
# What a player gets: the app inside still verifies and is the right version.
MNT="$W/mnt"; mkdir -p "$MNT"
hdiutil attach -quiet -readonly -nobrowse -noautoopen -mountpoint "$MNT" "$DMG"
ok=1
codesign --verify --deep --strict "$MNT/New Blitz.app" || ok=0
grep -q "<string>$V</string>" "$MNT/New Blitz.app/Contents/Info.plist" || { echo "the app in the dmg isn't version $V"; ok=0; }
[ "$(readlink "$MNT/Applications")" = /Applications ] || { echo "the dmg's Applications alias is missing"; ok=0; }
for f in .DS_Store .background.tiff .VolumeIcon.icns; do [ -f "$MNT/$f" ] || { echo "the dmg has no $f (its window)"; ok=0; }; done
[ ! -e "$MNT/.fseventsd" ] || { echo "the dmg holds .fseventsd"; ok=0; }
# The window's .DS_Store keeps an alias to its background: volume paths only, never this Mac's home folder.
! strings -a "$MNT/.DS_Store" | grep -q -e "$HOME" -e '/Users/' -e '/private/' || { echo "the dmg's .DS_Store names a local path"; ok=0; }
hdiutil detach -quiet "$MNT" || hdiutil detach -force -quiet "$MNT"
[ $ok = 1 ] || exit 1
echo "NewBlitz-v$V-macOS.dmg (the app verifies inside)"

# --- Windows: the installer ------------------------------------------------------------------------------------------
say "Windows installer"
WSRC="$STAGE/windows/New Blitz"
[ -f "$WSRC/New Blitz.exe" ] || { echo "no $WSRC/New Blitz.exe"; exit 1; }
# The icon built into New Blitz.exe (platform/windows/windows_stage.cmake), for the installer and the uninstaller.
ICO=${WIN_ICO:-build/win_modern/blitz.ico}
if [ ! -f "$ICO" ]; then
    ICO="$W/blitz.ico"
    python3 tools/windows/make_ico.py icons/app_2026.png "$ICO"
fi
# Windows wants a four-number version: 0.9.5-rc1 -> 0.9.5.0
nums=$(echo "${V%%[-+]*}" | tr -cd '0-9.')
IFS=. read -r a b c d <<< "$nums.0.0.0"
VIV="${a:-0}.${b:-0}.${c:-0}.${d:-0}"
SETUP="$OUT/NewBlitz-v$V-Windows-Setup.exe"
makensis -V2 -INPUTCHARSET UTF8 -DVERSION="$V" -DVIVERSION="$VIV" -DSRC="$WSRC" -DICON="$ROOT/$ICO" \
    -DOUTFILE="$SETUP" platform/windows/installer.nsi > "$W/makensis.log" 2>&1 || { cat "$W/makensis.log"; exit 1; }
# It holds the whole app folder.
command -v 7z >/dev/null && {
    want=$(cd "$WSRC" && find . -type f | wc -l | xargs)
    got=$(7z l -ba "$SETUP" | grep -vc -E '\$PLUGINSDIR|Uninstall New Blitz\.exe|\[NSIS\]' || true)
    [ "$got" -ge "$want" ] || { echo "the installer lists $got files, the folder has $want"; exit 1; }
}
echo "NewBlitz-v$V-Windows-Setup.exe"

# --- Linux: the AppImage ---------------------------------------------------------------------------------------------
say "Linux AppImage"
LSRC="$STAGE/linux/New Blitz"
[ -f "$LSRC/New Blitz" ] || { echo "no $LSRC/New Blitz"; exit 1; }
SYSLIB=build/linux/sysroot-x86_64/usr/lib/x86_64-linux-gnu
SYSDOC=build/linux/sysroot-x86_64/usr/share/doc
[ -d "$SYSLIB" ] || { echo "no $SYSLIB (tools/linux/make_sysroot.sh): the AppImage bundles its libraries"; exit 1; }
APPDIR="$W/New Blitz.AppDir"
mkdir -p "$APPDIR/usr/lib" "$APPDIR/usr/share/doc"
ditto "$LSRC" "$APPDIR/New Blitz"
# Bundled: SDL2 and FreeType (Debian 12's, as linked), what they need that a desktop may lack, and their licences. The
# system keeps glibc, libstdc++, X11/Wayland, ALSA/PulseAudio, Mesa (drm, gbm), D-Bus, zlib and the Vulkan loader.
BUNDLE="libSDL2-2.0.so.0:libsdl2-2.0-0 libfreetype.so.6:libfreetype6 libpng16.so.16:libpng16-16
 libbrotlidec.so.1:libbrotli1 libbrotlicommon.so.1:libbrotli1 libsamplerate.so.0:libsamplerate0
 libdecor-0.so.0:libdecor-0-0 libXss.so.1:libxss1"
for e in $BUNDLE; do
    lib=${e%%:*} pkg=${e#*:}
    cp -L "$SYSLIB/$lib" "$APPDIR/usr/lib/$lib"
    chmod 644 "$APPDIR/usr/lib/$lib"
    patchelf --set-rpath '$ORIGIN' "$APPDIR/usr/lib/$lib"
    [ -f "$SYSDOC/$pkg/copyright" ] && mkdir -p "$APPDIR/usr/share/doc/$pkg" && cp "$SYSDOC/$pkg/copyright" "$APPDIR/usr/share/doc/$pkg/"
done
# The executable finds them through its RUNPATH, not LD_LIBRARY_PATH, so the curl it starts for downloads keeps the
# system's own libraries.
patchelf --set-rpath '$ORIGIN/../usr/lib' "$APPDIR/New Blitz/New Blitz"
"$LLVM/llvm-readelf" -d "$APPDIR/New Blitz/New Blitz" | grep -q 'RUNPATH.*\$ORIGIN/\.\./usr/lib' || { echo "patchelf didn't set the RUNPATH"; exit 1; }
# Every library the executable and the bundle ask for is bundled or one the system keeps.
SYSTEM_LIBS="libc.so.6 libm.so.6 libstdc++.so.6 libgcc_s.so.1 libdbus-1.so.3 libz.so.1 libasound.so.2 libpulse.so.0
 libX11.so.6 libXext.so.6 libXcursor.so.1 libXi.so.6 libXfixes.so.3 libXrandr.so.2 libdrm.so.2 libgbm.so.1
 libwayland-egl.so.1 libwayland-client.so.0 libwayland-cursor.so.0 libxkbcommon.so.0 ld-linux-x86-64.so.2"
for f in "$APPDIR/New Blitz/New Blitz" "$APPDIR"/usr/lib/*.so*; do
    for n in $("$LLVM/llvm-readelf" -d "$f" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
        [ -f "$APPDIR/usr/lib/$n" ] || [[ " $(echo $SYSTEM_LIBS) " == *" $n "* ]] || { echo "$(basename "$f") needs $n: bundle it or add it to SYSTEM_LIBS"; exit 1; }
    done
done
cat > "$APPDIR/AppRun" <<'EOF'
#!/bin/sh
# New Blitz's AppImage: the app folder is "New Blitz" beside this script; its libraries are in usr/lib (RUNPATH).
HERE="$(dirname "$(readlink -f "$0")")"
exec "$HERE/New Blitz/New Blitz" "$@"
EOF
chmod 755 "$APPDIR/AppRun"
# BRANDING: the Linux desktop entry
cat > "$APPDIR/new-blitz.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=New Blitz
Comment=Arcade football: NFL Blitz 2000, native, with your own cartridge dump
Exec=new-blitz
Icon=new-blitz
Categories=Game;SportsGame;
Terminal=false
X-AppImage-Version=$V
EOF
cp "$LSRC/icon.png" "$APPDIR/new-blitz.png"
ln -s new-blitz.png "$APPDIR/.DirIcon"
chmod -R u+rwX,go+rX "$APPDIR"
SQFS="$W/NewBlitz.squashfs"
mksquashfs "$APPDIR" "$SQFS" -root-owned -noappend -no-xattrs -comp zstd -mkfs-time 0 -all-time 0 -quiet -no-progress
RUNTIME=${APPIMAGE_RUNTIME:-build/appimage/runtime-x86_64}
if [ -f "$RUNTIME" ]; then
    [ "$(head -c4 "$RUNTIME" | od -An -tx1 | tr -d ' \n')" = 7f454c46 ] || { echo "$RUNTIME isn't an ELF file"; exit 1; }
    pin=$(awk '/appimage-runtime-x86_64/{print $1}' tools/deps.sha256 2>/dev/null || true)
    if [ -n "$pin" ]; then
        [ "$(shasum -a 256 "$RUNTIME" | cut -d' ' -f1)" = "$pin" ] || { echo "$RUNTIME doesn't match tools/deps.sha256"; exit 1; }
    else
        echo "note: tools/deps.sha256 has no appimage-runtime-x86_64 line; this runtime is $(shasum -a 256 "$RUNTIME" | cut -d' ' -f1)"
    fi
    cat "$RUNTIME" "$SQFS" > "$OUT/NewBlitz-v$V-x86_64.AppImage"
    chmod 755 "$OUT/NewBlitz-v$V-x86_64.AppImage"
    echo "NewBlitz-v$V-x86_64.AppImage"
else
    echo "SKIPPED NewBlitz-v$V-x86_64.AppImage: no AppImage runtime at $RUNTIME (APPIMAGE_RUNTIME=<file>; docs/RELEASE.md)."
    echo "  The AppDir and its squashfs are ready: $APPDIR"
fi
