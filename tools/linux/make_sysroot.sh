#!/usr/bin/env bash
# Builds build/linux/sysroot-x86_64 from Debian 12 "bookworm" amd64 packages (glibc 2.36: runs on SteamOS, current Ubuntu/Fedora).
# No dpkg needed: fetches the package index, resolves dependencies in python3, then ar+tar extracts each .deb.
# Idempotent: downloads are cached in build/linux/debcache (deleted at the end unless KEEP_DEBS=1) and finished
# packages are recorded in the sysroot's .installed list, so re-running only fetches what is missing.
#   tools/linux/make_sysroot.sh            # then: cmake -DCMAKE_TOOLCHAIN_FILE=platform/linux/toolchain-x86_64.cmake ...
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SYSROOT="${SYSROOT:-$ROOT/build/linux/sysroot-x86_64}"
CACHE="$ROOT/build/linux/debcache"
MIRROR="${MIRROR:-https://deb.debian.org/debian}"
SUITE=bookworm
READELF="${READELF:-/opt/homebrew/opt/llvm/bin/llvm-readelf}"

# Top-level packages; dependencies (library packages only) are pulled in by the resolver.
WANT="libc6 libc6-dev linux-libc-dev libgcc-s1 libgcc-12-dev libstdc++6 libstdc++-12-dev
 libsdl2-2.0-0 libsdl2-dev libfreetype6 libfreetype-dev zlib1g zlib1g-dev libpng16-16 libbrotli1 libbz2-1.0
 libvulkan1 libvulkan-dev libx11-6 libx11-dev libxrandr2 libxrandr-dev libxext6 libxrender1 libxcb1 libxau6
 libxdmcp6 x11proto-dev libdbus-1-3 libdbus-1-dev libxext-dev libxrender-dev libatomic1 ${EXTRA_PACKAGES:-}"

mkdir -p "$CACHE" "$SYSROOT"
[ -s "$CACHE/Packages" ] || {
  curl -fsSL "$MIRROR/dists/$SUITE/main/binary-amd64/Packages.xz" -o "$CACHE/Packages.xz"
  python3 -I -c "import lzma,sys;open(sys.argv[2],'wb').write(lzma.open(sys.argv[1]).read())" "$CACHE/Packages.xz" "$CACHE/Packages"
}

# Prints "package filename" for the closure of $WANT.
resolve() {
python3 -I - "$CACHE/Packages" $WANT <<'PY'
import sys, re
pk = {}; prov = {}
for blk in open(sys.argv[1], encoding='utf-8').read().split('\n\n'):
    f = {}; key = None
    for ln in blk.split('\n'):
        if ln[:1] in ' \t':
            continue
        if ':' in ln:
            key, v = ln.split(':', 1); f[key] = v.strip()
    if 'Package' in f:
        pk[f['Package']] = f
        for p in f.get('Provides', '').split(','):
            p = re.sub(r'[ (].*', '', p.strip())
            if p: prov.setdefault(p, f['Package'])
# Only library-ish packages are followed; tools, perl, debconf, dpkg and the like are never needed to link.
follow = re.compile(r'^(lib|zlib|x11proto|xorg-sgml|linux-libc|xtrans)')
skip = {'libc-bin', 'libc-dev-bin', 'libc-l10n', 'libgcc-s1:i386', 'libsystemd-shared', 'dbus', 'dbus-bin', 'dbus-daemon',
        'dbus-session-bus-common', 'dbus-system-bus-common', 'libsdl2-dev:any', 'libfreetype-dev:any', 'libglib2.0-0',
        'libcrypt-dev', 'libglib2.0-0'}
seen = []; todo = list(sys.argv[2:])
while todo:
    n = todo.pop(0)
    n = re.sub(r':.*', '', n)
    if n not in pk: n = prov.get(n, n)
    if n in seen or n not in pk or n in skip: continue
    seen.append(n)
    f = pk[n]
    if n.endswith('-dev') and n != 'libc6-dev': continue   # headers only: runtime libs come from WANT
    for fld in ('Depends', 'Pre-Depends'):
        for grp in f.get(fld, '').split(','):
            alt = [re.sub(r'[ (:].*', '', a.strip()) for a in grp.split('|') if a.strip()]
            for a in alt:
                real = a if a in pk else prov.get(a)
                if real and follow.match(real) and real not in skip:
                    todo.append(real); break
for n in seen:
    print(n, pk[n]['Filename'])
PY
}

LIST="$(resolve)"
echo "$LIST" | wc -l | xargs echo "packages in closure:"
touch "$SYSROOT/.installed"
echo "$LIST" | while read -r name file; do
  grep -qx "$name" "$SYSROOT/.installed" && continue
  deb="$CACHE/$(basename "$file")"
  [ -s "$deb" ] || curl -fsSL "$MIRROR/$file" -o "$deb"
  tmp="$(mktemp -d "$CACHE/x.XXXXXX")"
  ( cd "$tmp" && ar x "$deb" && tar -xf data.tar.* -C "$SYSROOT" --no-same-owner 2>/dev/null || tar -xf data.tar.* -C "$SYSROOT" )
  rm -rf "$tmp"
  echo "$name" >> "$SYSROOT/.installed"
  echo "  installed $name"
done

# Absolute symlinks (/lib/x86_64-linux-gnu/libfoo.so.1) -> relative, so they resolve inside the sysroot.
python3 -I - "$SYSROOT" <<'PY'
import os, sys
root = sys.argv[1]
for d, dirs, files in os.walk(root):
    for n in dirs + files:
        p = os.path.join(d, n)
        if os.path.islink(p):
            t = os.readlink(p)
            if t.startswith('/'):
                new = os.path.relpath(os.path.join(root, t.lstrip('/')), d)
                os.remove(p); os.symlink(new, p)
PY

# Linker scripts with absolute paths: use plain names (found via the sysroot's lib dirs).
for f in "$SYSROOT"/usr/lib/x86_64-linux-gnu/lib{c,pthread,m}.so; do
  [ -f "$f" ] && [ ! -L "$f" ] && grep -q GROUP "$f" 2>/dev/null || continue
  sed -E -e 's#/(usr/)?lib/x86_64-linux-gnu/##g' -e 's#/(usr/)?lib64/##g' "$f" > "$f.tmp" && mv "$f.tmp" "$f"
done

# Every NEEDED library of every shared object must exist in the sysroot, or lld cannot check symbols.
missing=0
for so in "$SYSROOT"/usr/lib/x86_64-linux-gnu/libSDL2-2.0.so.0* "$SYSROOT"/usr/lib/x86_64-linux-gnu/libvulkan.so.1; do
  [ -f "$so" ] || continue
  for n in $("$READELF" -d "$so" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
    [ -e "$SYSROOT/usr/lib/x86_64-linux-gnu/$n" ] || [ -e "$SYSROOT/lib/x86_64-linux-gnu/$n" ] || { echo "MISSING NEEDED: $n (of $(basename "$so"))"; missing=1; }
  done
done
[ "${KEEP_DEBS:-0}" = 1 ] || rm -rf "$CACHE"
echo "sysroot: $SYSROOT ($(du -sh "$SYSROOT" | cut -f1))"
exit $missing
