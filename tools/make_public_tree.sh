#!/bin/bash
# A sanitized snapshot of this repo for a public release, with a fresh one-commit history:
#
#   tools/make_public_tree.sh [OUT_DIR] [--author "Name <email>"]      (default OUT_DIR: build/public/blitzrecomp)
#
# This repo's history holds local paths and private page links throughout, so the public repo starts fresh from
# HEAD instead of rewriting it. What changes on the way (tools/public_exclude.txt lists the paths left out):
#   - excluded paths are dropped (the real-name NFL roster pack, private working pages);
#   - in text files, this machine's paths become <repo> / ~ and claude.ai page links become "(private page)";
#   - a check fails the export if a ROM-like file, a key or a leftover private link remains.
# Nothing is pushed: review OUT_DIR, then add a remote yourself.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
# BRANDING: public repo folder name
OUT=${1:-$ROOT/build/public/blitzrecomp}
[ "${1:-}" = "--author" ] && OUT=$ROOT/build/public/blitzrecomp
AUTHOR=""
while [ $# -gt 0 ]; do [ "$1" = "--author" ] && AUTHOR=$2 && shift; shift; done
# The public commit's author is always given: without it git would sign the commit with this checkout's own identity.
[ -n "$AUTHOR" ] || { echo "give the public identity: --author \"Name <email>\""; exit 1; }

rm -rf "$OUT"
mkdir -p "$OUT"
git archive HEAD | tar -x -C "$OUT"
# Submodules aren't in the archive: record them (the public repo keeps them as submodules).
cp .gitmodules "$OUT/" 2>/dev/null || true
# N64ModernRuntime's pin (ca568b6) is only on an upstream feature branch: the public repo fetches it from the project's
# own copy (github.com/blitzrecomp/N64ModernRuntime, branch newblitz-pin), so a deleted upstream branch can't break clones.
git config -f "$OUT/.gitmodules" submodule.lib/N64ModernRuntime.url https://github.com/blitzrecomp/N64ModernRuntime

while IFS= read -r line; do
    line=${line%%#*}; line=$(echo "$line" | xargs)
    [ -n "$line" ] && rm -rf "$OUT/$line"
done < tools/public_exclude.txt

# Sanitize text files.
cd "$OUT"
grep -rIl -e "$ROOT" -e "/Users/" -e "claude.ai/artifact" -e "claude.ai/code" . 2>/dev/null | while read -r f; do
    perl -pi -e "s#\Q$ROOT\E#<repo>#g; s#/Users/[A-Za-z0-9._-]+#~#g; s#https?://claude\.ai/(?:code/)?artifact/[A-Za-z0-9_-]+#(private page)#g" "$f"
done
# Windows home folders too (test reports): C:\Users\me -> C:\Users\me
grep -rIlE 'Users\\+[A-Za-z0-9._-]+' . 2>/dev/null | while read -r f; do
    perl -pi -e 's#(Users\\+)[A-Za-z0-9._-]+#${1}me#g' "$f"
done

# The developer's first name in comments and notes ("(team, 2026-10-25)", "the team's call") becomes "the team": the public
# project speaks as the Blitz Recomp team. Whole words only (Sample, Samsung and SAM stay); "The" at a sentence start.
grep -rIlwE 'the team|the team'"'"'s' . 2>/dev/null | while read -r f; do
    perl -pi -e '
        s/\(the team([,):])/(team$1/g;
        s/(^[\s\/#*\->|]*|[.!?]\s+|\*\*)the team\b/$1The team/g;
        s/\bSam\b/the team/g;
    ' "$f"
done

# Checks: nothing ROM-like, no keys, no private links, no archives, no photo locations, nothing naming the developer.
bad=$(find . -type f \( -iname '*.z64' -o -iname '*.n64' -o -iname '*.v64' -o -iname '*.mpk' -o -iname '*.bps' -o -iname '*.state' \) | head -5)
[ -z "$bad" ] || { echo "ROM-like files in the export:"; echo "$bad"; exit 1; }
# (This script and the exclude list name the patterns themselves, so they're skipped.)
if grep -rIlE 'BEGIN (RSA|OPENSSH|EC) PRIVATE|ghp_[A-Za-z0-9]{20}|github_pat_[A-Za-z0-9_]{20}|sk-[A-Za-z0-9]{20}|claude\.ai/(code/)?artifact' . \
        | grep -v -e '^./tools/make_public_tree.sh$' -e '^./tools/public_exclude.txt$' | head -3 | grep .; then
    echo "secrets or private links left (above)"; exit 1
fi
# Archives can't be read by the checks above or below (a crash report inside one named the test PC): leave them out.
bad=$(find . -type f \( -iname '*.zip' -o -iname '*.7z' -o -iname '*.tar' -o -iname '*.gz' -o -iname '*.tgz' \) | head -5)
[ -z "$bad" ] || { echo "archives in the export (add them to tools/public_exclude.txt, or unpack them):"; echo "$bad"; exit 1; }
# Phone photos carry where they were taken.
bad=$(find . -type f \( -iname '*.jpg' -o -iname '*.jpeg' -o -iname '*.heic' \) -print0 | xargs -0 python3 -c '
import struct, sys
for f in sys.argv[1:]:
    b = open(f, "rb").read(262144)
    i = b.find(b"Exif\0\0")
    if i < 0: continue
    t = b[i + 6:]
    e = "<" if t[:2] == b"II" else ">"
    try:
        off = struct.unpack(e + "I", t[4:8])[0]
        n = struct.unpack(e + "H", t[off:off + 2])[0]
        if any(struct.unpack(e + "H", t[off + 2 + 12 * k:off + 4 + 12 * k])[0] == 0x8825 for k in range(n)): print(f)
    except struct.error:
        print(f)
' | head -5)
[ -z "$bad" ] || { echo "photos with a GPS tag in the export (strip it, or exclude them):"; echo "$bad"; exit 1; }
# The developer's own name and address (this checkout's git identity, never written here) and any PUBLIC_DENY words:
# the family name, the e-mail address and the part of it before the @, in any file, text or not.
deny=()
me_name=$(cd "$ROOT" && git config user.name || true); me_mail=$(cd "$ROOT" && git config user.email || true)
[ -n "${me_name##* }" ] && [ "${me_name##* }" != "$me_name" ] && deny+=("${me_name##* }")
[ -n "$me_mail" ] && deny+=("$me_mail" "${me_mail%%@*}")
for w in ${PUBLIC_DENY:-}; do deny+=("$w"); done
# More words from build/public_deny.txt (one per line; gitignored, so the list itself never ships): account names,
# team IDs, other addresses.
if [ -f "$ROOT/build/public_deny.txt" ]; then
    while IFS= read -r w; do w=${w%%#*}; w=$(echo "$w" | xargs); [ -n "$w" ] && deny+=("$w"); done < "$ROOT/build/public_deny.txt"
fi
# The private repo's owner: the public repo lives under its own account, so naming this one is a leak.
owner=$(cd "$ROOT" && git remote get-url origin 2>/dev/null | sed -E 's#.*[:/]([^/]+)/[^/]+$#\1#' || true)
[ -n "$owner" ] && deny+=("$owner")
if grep -rIlw 'the team' . | head -3 | grep .; then echo "the first name is still there (above)"; exit 1; fi
for w in "${deny[@]:-}"; do
    [ ${#w} -ge 4 ] || continue
    if grep -railF -- "$w" . | head -3 | grep .; then echo "the export names the developer (\"$w\", above)"; exit 1; fi
done
git init -q -b main
git add -A
# The submodules, at the commits this repo pins (git archive leaves them out): recorded as gitlinks, so a clone
# with --recurse-submodules fetches them from their upstream URLs in .gitmodules.
(cd "$ROOT" && git submodule status) | while read -r sha path _; do
    sha=${sha#[-+U]}
    rm -rf "$path"
    git update-index --add --cacheinfo 160000,"$sha","$path"
done
# BRANDING: public repo's first commit message
if [ -n "$AUTHOR" ]; then git -c user.name="${AUTHOR% <*}" -c user.email="$(echo "$AUTHOR" | sed -E 's/.*<(.*)>.*/\1/')" commit -q -m "New Blitz 2027: initial public release"
else git commit -q -m "New Blitz 2027: initial public release"; fi
echo "public tree: $OUT ($(git ls-files | wc -l | xargs) files, one commit). Review it, then: git remote add origin <url> && git push -u origin main"
