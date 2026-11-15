#!/bin/bash
# The Visual C++ runtime DLLs the Windows build ships next to its .exe (docs/WINDOWS_VULKAN.md): the app links its own
# C runtime statically, but RT64's DXC (dxcompiler.dll, dxil.dll) needs msvcp140/vcruntime140, which a clean Windows
# PC doesn't have. Microsoft's redistributable package from the Visual Studio manifest xwin uses (its license accepted
# by whoever runs xwin), pinned by hash, into build/xwin/redist.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/build/xwin/redist"
URL=https://download.visualstudio.microsoft.com/download/pr/45d3b8dd-bced-4b37-9974-142f748d710c/4aaf54db0bfc9435f7c3660e1a00237a4b556042bfeea64bde44c2e0194e6ee5/Microsoft.VC.14.44.17.14.CRT.Redist.X64.base.vsix
SHA=4aaf54db0bfc9435f7c3660e1a00237a4b556042bfeea64bde44c2e0194e6ee5
mkdir -p "$OUT"
VSIX="$OUT/crt-redist-x64.vsix"
[ -f "$VSIX" ] || curl -sfL -o "$VSIX" "$URL"
echo "$SHA  $VSIX" | shasum -a 256 -c - >/dev/null
for dll in msvcp140.dll vcruntime140.dll vcruntime140_1.dll; do
    unzip -o -j -q "$VSIX" "*/$dll" -d "$OUT"
done
ls "$OUT"/*.dll
