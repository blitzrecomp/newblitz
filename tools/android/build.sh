#!/bin/bash
# Configure and build the Android app's native part (docs/ANDROID.md): build/android/app_<flavour>, staged for
# tools/android/build_apk.sh. `make android-classic` / `make android-modern` run this, the content step and the APK.
#   tools/android/build.sh classic|modern [extra cmake args]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/tools/android/env.sh"
flavour="$1"; shift
[ -f "$ROOT/build/android/prefix/lib/libSDL2.so" ] || "$ROOT/tools/android/build_deps.sh"
cmake -S "$ROOT" -B "$ROOT/build/android/app_$flavour" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo "${ANDROID_CMAKE_ARGS[@]}" \
    -DBLITZ_FLAVOUR="$flavour" "$@" >/dev/null
cmake --build "$ROOT/build/android/app_$flavour" --target BlitzRecompiled
