#!/bin/bash
# The PS5's Vulkan driver for RT64 (docs/PS5_HOMEBREW.md): Mesa RADV from mihawk-99/PS5_Mesa, linked statically, as
# UnleashedRecomp-Prospero's tools/build-radv.sh builds it (same revision and options), adapted for a Mac:
#   brew install meson glslang bison make; build/ps5/sdk-m (tools/ps5/setup_sdk.sh), build/ps5/pydeps (mako, ply, pyyaml)
#   build/ps5/deps/PS5_Mesa at the revision below; build/ps5/radv-tooling/{ps5-cross.ini,clc-cache.tar.xz} from
#   UnleashedRecomp-Prospero's tooling/radv (GPL-3.0; the cache spares building LLVM's SPIR-V tools)
# -> build/ps5/radv/lib/libvulkan_radeon.ps5.a and build/ps5/radv/include/vulkan
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
W="$ROOT/build/ps5"
REV=0b2d6d1a61d9bbf89cf8beb88a696144f67c61f8
export PS5_PAYLOAD_SDK="$W/sdk-m"  # the PS5_PayloadSDK fork (tools/ps5/setup_sdk.sh): ps5platform, which the winsys uses
export LLVM_CONFIG=${LLVM_CONFIG:-/opt/homebrew/opt/llvm/bin/llvm-config}
export PATH="/opt/homebrew/opt/bison/bin:$PATH"
export PYTHONPATH="$W/pydeps${PYTHONPATH:+:$PYTHONPATH}"
SRC="$W/deps/PS5_Mesa"
[ "$(git -C "$SRC" rev-parse HEAD)" = "$REV" ] || { echo "PS5_Mesa isn't at $REV"; exit 1; }

# OpenCL kernels precompiled to SPIR-V: stand-ins for mesa_clc and vtn_bindgen2 copy them from the cache.
CLC="$W/radv-clc"
rm -rf "$CLC" && mkdir -p "$CLC/cache" "$CLC/bin"
tar -xJf "$W/radv-tooling/clc-cache.tar.xz" -C "$CLC/cache"
cat > "$CLC/bin/mesa_clc" <<SH
#!/bin/bash
set -euo pipefail
out=""; depfile=""
while [[ \$# -gt 0 ]]; do case "\$1" in -o) out="\$2"; shift 2;; --depfile) depfile="\$2"; shift 2;; --) break;; *) shift;; esac; done
cp -f "$CLC/cache/\$(basename "\$out")" "\$out"
[[ -n "\$depfile" ]] && printf '%s:\n' "\$out" > "\$depfile"
exit 0
SH
cat > "$CLC/bin/vtn_bindgen2" <<SH
#!/bin/bash
set -euo pipefail
cp -f "$CLC/cache/\$(basename "\$2")" "\$2"
cp -f "$CLC/cache/\$(basename "\$3")" "\$3"
SH
chmod +x "$CLC/bin/mesa_clc" "$CLC/bin/vtn_bindgen2"
export PATH="$CLC/bin:$PATH"

B="$W/radv-build"
printf "[constants]\nsdk = '%s'\n" "$PS5_PAYLOAD_SDK" > "$W/radv-constants.ini"
if [ ! -f "$B/build.ninja" ]; then
    meson setup "$B" "$SRC" --cross-file "$W/radv-constants.ini" --cross-file "$W/radv-tooling/ps5-cross.ini" \
        -Dvulkan-drivers=amd -Dgallium-drivers= -Dplatforms= -Dradv-winsys=ps5 -Dllvm=disabled -Damd-use-llvm=false \
        -Dvideo-codecs= -Dbuildtype=debugoptimized -Db_ndebug=true -Dglx=disabled -Degl=disabled -Dgbm=disabled \
        -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dzstd=disabled \
        -Dzlib=enabled --force-fallback-for=zlib -Dexpat=disabled -Dxmlconfig=disabled -Dshader-cache=enabled \
        -Dbuild-tests=false -Dvulkan-layers= -Dtools= -Dmesa-clc=system -Dradv-build-id=$REV > "$B.setup.log" 2>&1 \
        || { tail -30 "$B.setup.log"; exit 1; }
fi
ninja -C "$B" src/amd/vulkan/libvulkan_radeon.a > "$B.log" 2>&1 || { grep -E "error|FAILED" "$B.log" | head -30; exit 1; }
OUT="$W/radv"
rm -rf "$OUT" && mkdir -p "$OUT/lib" "$OUT/include"
cp "$B/src/amd/vulkan/libvulkan_radeon.a" "$OUT/lib/libvulkan_radeon.ps5.a"
cp -r "$SRC/include/vulkan" "$OUT/include/vulkan"
echo "-> $OUT/lib/libvulkan_radeon.ps5.a ($(du -h "$OUT/lib/libvulkan_radeon.ps5.a" | cut -f1))"
