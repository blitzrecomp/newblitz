#!/bin/bash
# Report whether this Mac has everything the Blitz recomp build needs.
# Exit status is the number of missing required items.

missing=0
ok()   { printf "  \033[32mok\033[0m    %-22s %s\n" "$1" "$2"; }
miss() { printf "  \033[31mMISS\033[0m  %-22s %s\n" "$1" "$2"; missing=$((missing+1)); }
later(){ printf "  \033[33mlater\033[0m %-22s %s\n" "$1" "$2"; }

ver() { "$@" 2>&1 | head -1; }

echo "Host"
arch=$(uname -m)
[ "$arch" = arm64 ] && ok arch "$arch" || miss arch "$arch (expected arm64)"
ok macOS "$(sw_vers -productVersion)"

echo "Host toolchain"
if xcode-select -p >/dev/null 2>&1; then ok xcode "$(xcodebuild -version 2>/dev/null | head -1 || echo 'command line tools')"; else miss xcode "run: xcode-select --install"; fi
for t in clang cmake ninja git python3; do
  if command -v $t >/dev/null; then ok $t "$(ver $t --version)"; else miss $t "brew install $t"; fi
done
for lib in sdl2 freetype libpng; do
  if brew list --versions $lib >/dev/null 2>&1; then ok $lib "$(brew list --versions $lib)"; else miss $lib "brew install $lib"; fi
done

echo "MIPS patch toolchain"
llvm_bin="$(brew --prefix 2>/dev/null)/opt/llvm/bin"
lld_bin="$(brew --prefix 2>/dev/null)/opt/lld/bin"
if [ -x "$llvm_bin/clang" ] && "$llvm_bin/clang" -print-targets 2>/dev/null | grep -q mips; then
  ok mips-clang "$(ver "$llvm_bin/clang" --version)"
else
  later mips-clang "brew install llvm"
fi
if [ -x "$lld_bin/ld.lld" ]; then ok ld.lld "$(ver "$lld_bin/ld.lld" --version)"; else later ld.lld "brew install lld"; fi

# platform/macos/ld64 (the app's link step) runs /usr/bin/python3 and imports macholib.
if /usr/bin/python3 -c "import macholib" 2>/dev/null; then
  ok macholib "for platform/macos/ld64 ($(/usr/bin/python3 -c 'import macholib, os; print(os.path.dirname(macholib.__file__))'))"
else
  miss macholib "/usr/bin/python3 -m pip install --user macholib (Xcode's Python ships it; used by platform/macos/ld64)"
fi

echo "Analysis tools"
py="$(dirname "$0")/../.venv/bin/python"
if [ -x "$py" ] && "$py" -c "import splat, spimdisasm, rabbitizer" 2>/dev/null; then
  ok splat "$("$py" -c 'import splat; print(splat.__version__)')"
else
  later splat "make venv"
fi
if /usr/libexec/java_home -v 21+ >/dev/null 2>&1; then ok java "$(/usr/libexec/java_home -v 21+)"; else later java "JDK 21+ for Ghidra"; fi
GHIDRA_HOME="${GHIDRA_HOME:-$(ls -d "$HOME"/Developer/ghidra_*_PUBLIC 2>/dev/null | tail -1)}"
if [ -n "$GHIDRA_HOME" ] && [ -x "$GHIDRA_HOME/support/analyzeHeadless" ]; then ok ghidra "$GHIDRA_HOME"; else later ghidra "unpack Ghidra into ~/Developer or set GHIDRA_HOME"; fi

echo
[ $missing -eq 0 ] && echo "Required host tools present." || echo "$missing required item(s) missing."
exit $missing
