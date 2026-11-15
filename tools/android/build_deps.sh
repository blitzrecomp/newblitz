#!/bin/bash
# The Android build's libraries (docs/ANDROID.md): SDL2 (shared: SDLActivity loads libSDL2.so, then the game) and
# FreeType (static), for arm64-v8a, into build/android/prefix. The same versions and sources as the Mac's
# tools/build_deps.sh (its downloads are reused).
#   tools/android/build_deps.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/tools/android/env.sh"
SRC=$ROOT/build/deps/src
OUT=$ROOT/build/android/deps
PREFIX=$ROOT/build/android/prefix
SDL2_VER=2.32.10
FT_VER=2.13.3
[ -f "$SRC/SDL2-$SDL2_VER.tar.gz" ] && [ -f "$SRC/freetype-$FT_VER.tar.xz" ] || { echo "run tools/build_deps.sh first (it downloads both)"; exit 2; }
mkdir -p "$OUT"
cd "$OUT"
# No build-machine paths in the libraries (SDL's asserts keep __FILE__): sources are named from the checkout
# ("./build/..."), so a published app doesn't carry this computer's home folder (tools/release_scan.sh).
NO_PATHS=(-DCMAKE_C_FLAGS="-ffile-prefix-map=$ROOT=. -ffile-prefix-map=$HOME=~" -DCMAKE_CXX_FLAGS="-ffile-prefix-map=$ROOT=. -ffile-prefix-map=$HOME=~" -DCMAKE_OBJC_FLAGS="-ffile-prefix-map=$ROOT=. -ffile-prefix-map=$HOME=~")
common=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" "${ANDROID_CMAKE_ARGS[@]}" "${NO_PATHS[@]}")

rm -rf "SDL2-$SDL2_VER" && tar xzf "$SRC/SDL2-$SDL2_VER.tar.gz"
cmake -S "SDL2-$SDL2_VER" -B "SDL2-$SDL2_VER/build" "${common[@]}" -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF >/dev/null
cmake --build "SDL2-$SDL2_VER/build" >/dev/null
cmake --install "SDL2-$SDL2_VER/build" >/dev/null
# SDLActivity and friends: the Java half of SDL's Android port, compiled into the APK (tools/android/build_apk.sh).
rm -rf "$PREFIX/java" && mkdir -p "$PREFIX/java"
cp -R "SDL2-$SDL2_VER/android-project/app/src/main/java/org" "$PREFIX/java/"

rm -rf "freetype-$FT_VER" && tar xJf "$SRC/freetype-$FT_VER.tar.xz"
cmake -S "freetype-$FT_VER" -B "freetype-$FT_VER/build" "${common[@]}" -DBUILD_SHARED_LIBS=OFF \
      -DFT_DISABLE_PNG=ON -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_ZLIB=OFF >/dev/null
cmake --build "freetype-$FT_VER/build" >/dev/null
cmake --install "freetype-$FT_VER/build" >/dev/null

ls "$PREFIX"/lib/libSDL2.so "$PREFIX"/lib/libfreetype.a
echo "build/android/prefix ready"
