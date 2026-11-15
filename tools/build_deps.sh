#!/bin/bash
# Build the app's bundled libraries (SDL2, FreeType) from source for macOS 13+, into build/deps/prefix.
#
#   tools/build_deps.sh            (once; make app / make app2026 then use them and the app runs on macOS 13+)
#
# Homebrew's builds target the Mac they were built on (macOS 26 here), so an app bundling them won't open on
# an older macOS. With build/deps/prefix present, CMakeLists.txt links these instead and the bundle declares
# macOS 13 (LSMinimumSystemVersion). FreeType is built without PNG, HarfBuzz, Brotli or BZip2 (the UI's fonts
# don't need them), so libpng no longer ships.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
DEPS=$ROOT/build/deps
PREFIX=$DEPS/prefix
MIN=13.0
SDL2_VER=2.32.10
FT_VER=2.13.3
mkdir -p "$DEPS/src"
cd "$DEPS/src"

fetch() {  # url file sha256
    if [ ! -f "$2" ]; then curl -fsSL -o "$2.part" "$1" && mv "$2.part" "$2"; fi
    echo "$3  $2" | shasum -a 256 -c - >/dev/null || { echo "checksum mismatch for $2"; rm -f "$2"; exit 1; }
}
fetch "https://github.com/libsdl-org/SDL/releases/download/release-$SDL2_VER/SDL2-$SDL2_VER.tar.gz" "SDL2-$SDL2_VER.tar.gz" \
      "${SDL2_SHA256:-$(cat "$ROOT/tools/deps.sha256" | awk '/SDL2/{print $1}')}"
fetch "https://download.savannah.gnu.org/releases/freetype/freetype-$FT_VER.tar.xz" "freetype-$FT_VER.tar.xz" \
      "${FT_SHA256:-$(cat "$ROOT/tools/deps.sha256" | awk '/freetype/{print $1}')}"

# No build-machine paths in the libraries (SDL's asserts keep __FILE__): sources are named from the checkout
# ("./build/..."), so a published app doesn't carry this computer's home folder (tools/release_scan.sh).
NO_PATHS=(-DCMAKE_C_FLAGS="-ffile-prefix-map=$ROOT=. -ffile-prefix-map=$HOME=~" -DCMAKE_CXX_FLAGS="-ffile-prefix-map=$ROOT=. -ffile-prefix-map=$HOME=~" -DCMAKE_OBJC_FLAGS="-ffile-prefix-map=$ROOT=. -ffile-prefix-map=$HOME=~")
common=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_OSX_DEPLOYMENT_TARGET=$MIN
        -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_INSTALL_NAME_DIR="$PREFIX/lib" "${NO_PATHS[@]}")

rm -rf "SDL2-$SDL2_VER" && tar xzf "SDL2-$SDL2_VER.tar.gz"
cmake -S "SDL2-$SDL2_VER" -B "SDL2-$SDL2_VER/build" "${common[@]}" -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF >/dev/null
cmake --build "SDL2-$SDL2_VER/build" >/dev/null
cmake --install "SDL2-$SDL2_VER/build" >/dev/null

rm -rf "freetype-$FT_VER" && tar xJf "freetype-$FT_VER.tar.xz"
cmake -S "freetype-$FT_VER" -B "freetype-$FT_VER/build" "${common[@]}" -DBUILD_SHARED_LIBS=ON \
      -DFT_DISABLE_PNG=ON -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_ZLIB=OFF >/dev/null
cmake --build "freetype-$FT_VER/build" >/dev/null
cmake --install "freetype-$FT_VER/build" >/dev/null

for f in "$PREFIX"/lib/libSDL2-2.0.0.dylib "$PREFIX"/lib/libfreetype.6.dylib; do
    echo "$(basename "$f"): minos $(otool -l "$f" | awk '/LC_BUILD_VERSION/{f=1} f&&/minos/{print $2; exit}')"
done
echo "build/deps/prefix ready: reconfigure (make app / make app2026) to use it"
