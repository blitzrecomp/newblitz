#!/bin/bash
# The PS5 store zip (docs/PROSPEROSTORE.md): the PS5 kit's PPSA99961/ folder at the zip's top, for ProsperoStore's
# catalog, which installs it to /data/homebrew/PPSA99961.
#
#   tools/release/make_ps5_store_zip.sh <kit folder> <version> <out .zip>
#   e.g. tools/release/make_ps5_store_zip.sh build/devices/ps5 1.0.4 build/release/1.0.4/PPSA99961.zip
#
# <kit folder> holds PPSA99961/ (build/devices/ps5, from the pinned build/ps5_stable/current; it is only read). In the
# zip: PPSA99961/eboot.bin, sce_module/, sce_sys/ (param.json, icon0.png, pic0/pic1.dds) and the app's resources. Never
# launch.elf or STABLE.txt (they sit beside PPSA99961/, not in it): the catalog takes no ELF payloads, and its release
# scan flags any. sce_sys/param.json's contentVersion is set from <version> (1.0.4 -> 01.000.004) in the staged copy,
# so every release's zip is higher than the last, whatever build the kit is. Refused: a kit with an .elf, a symlink,
# a titleId other than PPSA99961, or no 512x512 icon0.png. make_release.sh calls this; it can run on its own too.
set -euo pipefail
KIT=${1:?kit folder (holds PPSA99961/)}
V=${2:?version}
OUT=${3:?out .zip}
TITLE=PPSA99961
SRC="$KIT/$TITLE"
fail() { echo "ps5 store zip: $*" >&2; exit 1; }
[ -f "$SRC/eboot.bin" ] || fail "no $SRC/eboot.bin"
[ -f "$SRC/sce_sys/param.json" ] || fail "no $SRC/sce_sys/param.json"
[ -f "$SRC/sce_sys/icon0.png" ] || fail "no $SRC/sce_sys/icon0.png"

# contentVersion from the version, as platform/ps5/ps5_stage.cmake makes it: NN.NNN.NNN.
CV=$(python3 -I -c '
import re, sys
m = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)", sys.argv[1])
if not m: sys.exit("version %s is not major.minor.patch" % sys.argv[1])
a, b, c = (int(x) for x in m.groups())
if a > 99 or b > 999 or c > 999: sys.exit("version %s does not fit NN.NNN.NNN" % sys.argv[1])
print("%02d.%03d.%03d" % (a, b, c))' "$V") || fail "no contentVersion"

STAGE=$(mktemp -d "${TMPDIR:-/tmp}/ps5_store_zip.XXXXXX")
trap 'rm -rf "$STAGE"' EXIT
# A copy without the Finder's extras; symlinks kept as links so they are caught below, not followed.
ditto --norsrc --noextattr --noqtn "$SRC" "$STAGE/$TITLE"
find "$STAGE/$TITLE" \( -name '.DS_Store' -o -name '._*' \) -delete
bad=$(find "$STAGE/$TITLE" -type l -o -iname '*.elf' -o -name 'STABLE.txt' -o -name '__MACOSX')
[ -z "$bad" ] || fail "the kit holds what a store zip must not (links, .elf, STABLE.txt): ${bad//$STAGE\//}"

python3 -I - "$STAGE/$TITLE/sce_sys/param.json" "$CV" "$TITLE" <<'EOF'
import json, re, sys
path, cv, title = sys.argv[1:]
text = open(path, encoding="utf-8").read()
p = json.loads(text)
if p.get("titleId") != title:
    sys.exit("param.json titleId is %r, not %s" % (p.get("titleId"), title))
old = p.get("contentVersion")
# Only that one value changes; the rest of the file stays as the build wrote it.
text, n = re.subn(r'("contentVersion"\s*:\s*")[^"]*(")', lambda m: m.group(1) + cv + m.group(2), text, count=1)
if n != 1:
    sys.exit("param.json has no contentVersion")
assert json.loads(text)["contentVersion"] == cv
open(path, "w", encoding="utf-8").write(text)
print("ps5 store zip: contentVersion %s%s" % (cv, "" if old == cv else " (the kit's param.json said %s)" % old))
EOF

w=$(sips -g pixelWidth "$STAGE/$TITLE/sce_sys/icon0.png" | awk '/pixelWidth/{print $2}')
h=$(sips -g pixelHeight "$STAGE/$TITLE/sce_sys/icon0.png" | awk '/pixelHeight/{print $2}')
[ "$w" = 512 ] && [ "$h" = 512 ] || fail "sce_sys/icon0.png is ${w}x${h}, not 512x512"

chmod -R u+rwX,go+rX "$STAGE/$TITLE"
mkdir -p "$(dirname "$OUT")"
OUT_ABS="$(cd "$(dirname "$OUT")" && pwd)/$(basename "$OUT")"
rm -f "$OUT_ABS"
# zip -X: no extra attributes (uid/gid, macOS's); -r with no -y never stores a link (there are none, checked above).
(cd "$STAGE" && zip -qr -X "$OUT_ABS" "$TITLE")
# The zip itself, checked: PPSA99961/ is the only top-level entry, and nothing banned slipped in.
listing=$(zipinfo -1 "$OUT_ABS")
top=$(sed 's#/.*##' <<< "$listing" | sort -u)
[ "$top" = "$TITLE" ] || fail "the zip's top level is not just $TITLE/: $top"
! grep -qiE '\.elf$|(^|/)STABLE\.txt$|__MACOSX|(^|/)\._' <<< "$listing" || fail "the zip holds a banned file"
grep -qx "$TITLE/eboot.bin" <<< "$listing" && grep -qx "$TITLE/sce_sys/param.json" <<< "$listing" \
    && grep -qx "$TITLE/sce_sys/icon0.png" <<< "$listing" || fail "the zip lacks eboot.bin, param.json or icon0.png"
echo "ps5 store zip: $OUT ($(du -h "$OUT_ABS" | cut -f1 | xargs), $(wc -l <<< "$listing" | xargs) entries)"
