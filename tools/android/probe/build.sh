#!/bin/bash
# build/android/probe/BlitzProbe.apk: the BLITZ DS probe (docs/BLITZ_DS.md), signed like the game. The upload address
# comes from build/blitz_logs_hub, never committed.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
source "$ROOT/tools/android/env.sh"
SRC="$ROOT/tools/android/probe"
W="$ROOT/build/android/probe"
rm -rf "$W" && mkdir -p "$W/classes" "$W/dex" "$W/res/values" "$W/lib/arm64-v8a"
. "$ROOT/build/blitz_logs_hub"
printf '<resources><string name="upload">%s</string></resources>\n' "$UPLOAD" > "$W/res/values/upload.xml"
BT="$ANDROID_BUILD_TOOLS"
JAR="$(ls "$ANDROID_HOME"/platforms/android-*/android.jar | sort -V | tail -1)"
HOST="$ANDROID_NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin"
# The shader lab's shaders, from the game's build (make android-modern).
python3 "$SRC/lab_shaders.py" "$ROOT/build/android/app_modern/src/shaders" "$ROOT/lib/rt64/src/contrib/dxc" "$SRC/lab_write.hlsl" "$W/lab_shaders.h"
"$HOST/aarch64-linux-android${ANDROID_API}-clang++" -std=c++17 -O2 -fPIC -shared -static-libstdc++ -Wl,-z,max-page-size=16384 \
    -I"$W" "$SRC/probe.cpp" -o "$W/lib/arm64-v8a/libblitzprobe.so" -landroid -llog -lvulkan -lm
"$JAVA_HOME/bin/javac" -nowarn --release 11 -classpath "$JAR" -d "$W/classes" $(find "$SRC/java" -name '*.java') 2> >(grep -v '^Note:' >&2)
"$BT/d8" --release --min-api "$ANDROID_API" --lib "$JAR" --output "$W/dex" $(find "$W/classes" -name '*.class')
"$BT/aapt2" compile --dir "$W/res" -o "$W/res.zip"
"$BT/aapt2" link -o "$W/base.apk" -I "$JAR" --manifest "$SRC/AndroidManifest.xml" \
    --min-sdk-version "$ANDROID_API" --target-sdk-version "$ANDROID_TARGET_API" "$W/res.zip"
(cd "$W/dex" && zip -q -X "$W/base.apk" classes.dex)
(cd "$W" && zip -q -X -0 -r base.apk lib)
"$BT/zipalign" -f -P 16 4 "$W/base.apk" "$W/aligned.apk"
KS="${BLITZ_ANDROID_KEYSTORE:-$HOME/.android/debug.keystore}"
"$BT/apksigner" sign --ks "$KS" --ks-pass "pass:${BLITZ_ANDROID_KEYSTORE_PASS:-android}" --ks-key-alias "${BLITZ_ANDROID_KEYSTORE_ALIAS:-androiddebugkey}" --out "$W/BlitzProbe.apk" "$W/aligned.apk"
rm -f "$W/BlitzProbe.apk.idsig"
ls -la "$W/BlitzProbe.apk"
