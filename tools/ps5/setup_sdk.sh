#!/bin/bash
# The PS5 toolchain (docs/PS5_HOMEBREW.md): mihawk-99/PS5_PayloadSDK, the fork UnleashedRecomp-Prospero pins, which
# adds ps5platform (what RADV's PS5 winsys and the display surface use) over the upstream payload SDK release v0.42.
# Its own platform/tools/setup-sdk.sh does this on Linux; this is the same on a Mac (curl, shasum, gmake).
#   brew install llvm make; -> build/ps5/sdk-m, then SDL2, FreeType and libsamplerate for the PS5 into build/ps5/prefix,
#   and UnleashedRecomp-Prospero's title tooling (tools/ps5/link_title.sh) with ps5-native-tool built for the Mac
#   tools/ps5/setup_sdk.sh sdl   rebuilds SDL2 alone (with platform/sdl-ps5-patches) into build/ps5/prefix; make ps5-* runs
#   it when those patches changed since the installed library was built (its stamp, build/ps5/prefix/.sdl-patches)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
W="$ROOT/build/ps5"

# No build-machine paths in the libraries (SDL's asserts keep __FILE__): named from the checkout (tools/release_scan.sh)
NO_PATHS=(-DCMAKE_C_FLAGS="-ffile-prefix-map=$ROOT=. -ffile-prefix-map=$HOME=~" -DCMAKE_CXX_FLAGS="-ffile-prefix-map=$ROOT=. -ffile-prefix-map=$HOME=~")
# SDL2's PS5 port (input, events, audio; its video stays off: RT64 draws through Vulkan), with our fixes on top
# (platform/sdl-ps5-patches: the pads' instance ids). The stamp says which patches the installed library has.
build_sdl() {
    local SDL="$W/deps/SDL"
    [ -d "$SDL/.git" ] || git clone -q --depth 1 -b release-2.30.x-ps5 https://github.com/ps5-payload-dev/SDL.git "$SDL"
    git -C "$SDL" checkout -q -- .  # its own files back, then the patches in order
    for p in "$ROOT"/platform/sdl-ps5-patches/*.patch; do
        git -C "$SDL" apply --whitespace=nowarn "$p" || { echo "SDL: $(basename "$p") does not apply (SDL's branch moved?)" >&2; exit 1; }
    done
    rm -rf "$SDL/build-ps5"
    cmake -S "$SDL" -B "$SDL/build-ps5" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$PS5_PAYLOAD_SDK/toolchain/prospero.cmake" \
        -DCMAKE_BUILD_TYPE=Release -DSDL_OPENGL=NO -DSDL_SHARED=NO -DSDL_STATIC=YES -DSDL_TEST=NO \
        -DCMAKE_INSTALL_PREFIX="$W/prefix" -DCMAKE_VERBOSE_MAKEFILE=OFF "${NO_PATHS[@]}" >/dev/null
    cmake --build "$SDL/build-ps5" >/dev/null
    cmake --install "$SDL/build-ps5" >/dev/null
    cat "$ROOT"/platform/sdl-ps5-patches/*.patch | shasum -a 256 | cut -d' ' -f1 > "$W/prefix/.sdl-patches"
}
if [ "${1:-}" = "sdl" ]; then
    export PS5_PAYLOAD_SDK="$W/sdk-m"
    export LLVM_CONFIG=${LLVM_CONFIG:-/opt/homebrew/opt/llvm/bin/llvm-config}  # the SDK's clang wrapper finds clang through it
    [ -d "$PS5_PAYLOAD_SDK" ] || { echo "no build/ps5/sdk-m: run tools/ps5/setup_sdk.sh once first" >&2; exit 1; }
    build_sdl
    echo "-> SDL2 with platform/sdl-ps5-patches in $W/prefix"
    exit 0
fi

REV=95c08f27386fc698f6bbe21dde3030140a41d10b
export LLVM_CONFIG=${LLVM_CONFIG:-/opt/homebrew/opt/llvm/bin/llvm-config}
mkdir -p "$W/deps"
T="$W/deps/PS5_PayloadSDK"
[ -d "$T/.git" ] || git clone -q https://github.com/mihawk-99/PS5_PayloadSDK.git "$T"
git -C "$T" fetch -q origin "$REV" 2>/dev/null || true
git -C "$T" checkout -q "$REV"

ZIP="$W/sdk-v042.zip"
[ -f "$ZIP" ] || curl -sfL -o "$ZIP" https://github.com/ps5-payload-dev/sdk/releases/download/v0.42/ps5-payload-sdk.zip
echo "8cfbc7cd5811e719eb4f0c47eea668d3dc7b40bc8ab11c4a5031d40c23ec02da  $ZIP" | shasum -a 256 -c - >/dev/null
rm -rf "$W/sdk-m" "$W/sdk-m-stage" && mkdir "$W/sdk-m-stage"
unzip -q "$ZIP" -d "$W/sdk-m-stage"
S="$W/sdk-m-stage/ps5-payload-sdk"
rmdir "$S/target/include_common"
install -m 0755 "$T/host/bin/prospero-clang" "$T/host/bin/prospero-clang++" "$S/bin/"
export PS5_PAYLOAD_SDK="$S"
gmake -s -C "$T/include" install DESTDIR="$S"
gmake -s -C "$T/platform" clean
gmake -s -C "$T/platform" install DESTDIR="$S"
echo "$REV" > "$S/.ps5-sdk-revision"
mv "$S" "$W/sdk-m" && rmdir "$W/sdk-m-stage"
export PS5_PAYLOAD_SDK="$W/sdk-m"

build_sdl
# FreeType (RmlUi's fonts) and libsamplerate (SDL's PS5 audio), static
dep() {  # dir url tag cmake-options...
    local d="$W/deps/$1" url=$2 tag=$3; shift 3
    [ -d "$d/.git" ] || git clone -q --depth 1 -b "$tag" "$url" "$d"
    rm -rf "$d/build-ps5"
    cmake -S "$d" -B "$d/build-ps5" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$PS5_PAYLOAD_SDK/toolchain/prospero.cmake" \
        -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DCMAKE_INSTALL_PREFIX="$W/prefix" \
        -DCMAKE_VERBOSE_MAKEFILE=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5 "${NO_PATHS[@]}" "$@" >/dev/null
    cmake --build "$d/build-ps5" >/dev/null
    cmake --install "$d/build-ps5" >/dev/null
}
dep freetype https://github.com/freetype/freetype.git VER-2-13-3 -DFT_DISABLE_BROTLI=ON -DFT_DISABLE_BZIP2=ON \
    -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_PNG=ON -DFT_DISABLE_ZLIB=ON
dep libsamplerate https://github.com/libsndfile/libsamplerate.git 0.2.2 -DBUILD_TESTING=OFF -DLIBSAMPLERATE_EXAMPLES=OFF

# UnleashedRecomp-Prospero (GPL-3.0): its RADV link recipe, title start-up code, AGC link stubs, launch payload source
# and generated runtime module (runtime/libc.prx), used from this checkout, never copied into the repository.
UR="$W/deps/unleashed-prospero"
UREV=156630c0efaad0f64935569446954fcec9c183ce
[ -d "$UR/.git" ] || git clone -q https://github.com/Nazky/UnleashedRecomp-Prospero.git "$UR"
git -C "$UR" fetch -q origin "$UREV" 2>/dev/null || true
git -C "$UR" checkout -q "$UREV"
echo "e6ff45d16adf687855cc3b33b0c8a4132b6504360b221e0a34c7e99fb3ba0036  $UR/runtime/libc.prx" | shasum -a 256 -c - >/dev/null
N="$UR/tooling/native"
mkdir -p "$W/host"
xcrun clang++ -std=c++20 -O2 "$N/native_app_builder.cpp" "$N/self_container.cpp" "$N/elf_object.cpp" \
    "$N/sce_module_writer.cpp" -lz -o "$W/host/ps5-native-tool"
# The home screen's pictures as the console's BC7 DDS (make ps5-*; tools/ps5/make_title_art.py)
xcrun clang++ -std=c++20 -O3 -I "$UR/thirdparty/stb" "$UR/tools/png_to_bc7_dds/png_to_bc7_dds.cpp" -o "$W/host/png_to_bc7_dds"
echo "-> $W/sdk-m (fork $REV), SDL2/FreeType/libsamplerate in $W/prefix, $W/host/ps5-native-tool and png_to_bc7_dds"
