#!/bin/bash
# Pack a built Android app (build/android/app_<flavour>/apk, staged by platform/android/android_stage.cmake) into a
# signed APK with the SDK's own tools: javac + d8 for the Java shell (SDL's activity and BlitzActivity), aapt2 for the
# manifest, icon and assets, zipalign (16 KB pages for the native libraries) and apksigner. No Gradle.
#   tools/android/build_apk.sh build/android/app_classic        -> build/android/app_classic/ClassicBlitz.apk
# Signing key: BLITZ_ANDROID_KEYSTORE (+ _PASS, _ALIAS), else the SDK's debug key (~/.android/debug.keystore). Keep
# using the same key: Android only updates an app signed by the key that installed it.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/tools/android/env.sh"
BUILD="$(cd "$1" && pwd)"
APK="$BUILD/apk"
[ -f "$APK/AndroidManifest.xml" ] && [ -f "$APK/lib/arm64-v8a/libBlitzRecompiled.so" ] || { echo "$APK isn't staged: build the app first"; exit 2; }
BT="$ANDROID_BUILD_TOOLS"
JAR="$ANDROID_HOME/platforms/android-$ANDROID_TARGET_API/android.jar"
[ -f "$JAR" ] || JAR="$(ls "$ANDROID_HOME"/platforms/android-*/android.jar | sort -V | tail -1)"
label="$(sed -n 's/^label=//p' "$APK/app.properties")"
OUT="$BUILD/${label// /}.apk"
W="$BUILD/apk-work"
rm -rf "$W" && mkdir -p "$W/classes" "$W/dex" "$W/res"

# The test logs' upload address (docs/BLITZ_DS.md), when this Mac has one. Upload only; reading needs another key.
# Never in a public build (BLITZ_CONTENT=public, from the Makefile): it is the developer's own log and remote-testing
# mailbox (BlitzRemote.java), so without it Send Logs and the remote stay off.
rm -f "$APK/assets/res/logs_hub"
if [ "${BLITZ_CONTENT:-private}" = public ]; then
    echo "public: no logs address in the APK"
elif [ -f "$ROOT/build/blitz_logs_hub" ]; then
    (. "$ROOT/build/blitz_logs_hub"; printf '%s\n' "$UPLOAD" > "$APK/assets/res/logs_hub")
fi

# The commit, for the stamp on every uploaded log (BlitzActivity.java). A + means files had changed since that commit, or a
# submodule isn't exactly its patches. Applied patches always leave them dirty, so git can't say that one.
commit="$(sed -n 's/^commit=//p' "$APK/app.properties")"
if ! git -C "$ROOT" diff --quiet --ignore-submodules=dirty HEAD 2>/dev/null || ! "$ROOT/tools/apply_patches.sh" --check; then
    commit="$commit+"
fi
printf '%s\n' "$commit" > "$APK/assets/res/build_commit"

# Java: SDL's activity (tools/android/build_deps.sh copied it) and ours.
find "$ROOT/build/android/prefix/java" "$ROOT/platform/android/java" -name '*.java' > "$W/sources.txt"
"$JAVA_HOME/bin/javac" -nowarn --release 11 -classpath "$JAR" -d "$W/classes" @"$W/sources.txt" 2> >(grep -v '^Note:' >&2)
"$BT/d8" --release --min-api "$ANDROID_API" --lib "$JAR" --output "$W/dex" $(find "$W/classes" -name '*.class')

# Manifest, icon, assets.
"$BT/aapt2" compile --dir "$APK/res" -o "$W/res.zip"
"$BT/aapt2" link -o "$W/base.apk" -I "$JAR" --manifest "$APK/AndroidManifest.xml" \
    --min-sdk-version "$ANDROID_API" --target-sdk-version "$ANDROID_TARGET_API" -A "$APK/assets" "$W/res.zip" \
    -0 so --no-compress-regex '\.(z64|wms|ttf|png)$'
# The dex, and the native libraries stored (uncompressed: loaded straight from the APK, extractNativeLibs=false).
(cd "$W/dex" && zip -q -X "$W/base.apk" classes.dex)
(cd "$APK" && zip -q -X -0 -r "$W/base.apk" lib)
"$BT/zipalign" -f -P 16 4 "$W/base.apk" "$W/aligned.apk"

KS="${BLITZ_ANDROID_KEYSTORE:-$HOME/.android/debug.keystore}"
PASS="${BLITZ_ANDROID_KEYSTORE_PASS:-android}"
ALIAS="${BLITZ_ANDROID_KEYSTORE_ALIAS:-androiddebugkey}"
if [ ! -f "$KS" ]; then
    "$JAVA_HOME/bin/keytool" -genkeypair -keystore "$KS" -storepass "$PASS" -alias "$ALIAS" -keypass "$PASS" \
        -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Android Debug,O=Android,C=US" >/dev/null
fi
"$BT/apksigner" sign --ks "$KS" --ks-pass "pass:$PASS" --ks-key-alias "$ALIAS" --out "$OUT" "$W/aligned.apk"
"$BT/zipalign" -c -P 16 4 "$OUT" >/dev/null && echo "aligned for 16 KB pages"
rm -f "$OUT.idsig"
du -h "$OUT" | cut -f1 | xargs echo "$OUT:"
