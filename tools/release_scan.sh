#!/bin/bash
# The last check before a release goes out (make release runs it; it fails the release on any hit):
#
#   tools/release_scan.sh <artifact or folder>...       e.g. tools/release_scan.sh build/release/0.9.1
#
# Every artifact is unpacked (zips, .tar.gz, the Mac .dmg (attached read-only, copied, detached), the Linux .AppImage
# (its squashfs read with unsquashfs: no Linux needed), the Windows *Setup*.exe (NSIS, with 7z), and the .ipa/.apk inside
# them, as deep as they go) and every file, binary or not, is searched for:
#   - this machine's home folder and user name, as paths (/Users/<name>, /home/<name>, C:\Users\<name>), in ASCII and
#     UTF-16 (Windows resources);
#   - the developer's own name and address: this checkout's git identity (family name, e-mail and the part before the
#     @), the private repo's owner (git remote origin), and every word in build/public_deny.txt (as
#     tools/make_public_tree.sh does; the list itself never ships);
#   - licensed content that only the 2026 Season pak may carry ("Houston Texans");
#   - the private addresses and keys in build/ (live rosters, the logs and remote-testing mailbox, install links, the
#     PSP keys, the season server's upload keys): read from those files at scan time and never printed. Only the season
#     server's own address (build/pak_server) may appear, and no other *.workers.dev host at all.
# Plus files a public build must not hold: a logs/remote address (assets/res/logs_hub), live-rosters secrets
# (rosters/token|hub|repo), a provisioning profile, the team-signed iPhone kit (NewBlitz.app.zip), the 2026 roster pack
# or Houston; an app bundle signed for a team; and a tar entry owned by this user.
#
# BLITZ_SECRETS_DIR=<folder>: another build/ folder whose secret files count too (a worktree's build/ may lack them).
# Hits print as "<archive>!<file>: <what>", never the secret itself. Exit 1 on any hit.
# Tools beyond macOS's: unsquashfs for an AppImage (brew install squashfs), 7z for an installer (brew install p7zip).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
[ $# -gt 0 ] || { sed -n '2,4p' "$0"; exit 2; }

WORK=$(mktemp -d "${TMPDIR:-/tmp}/release_scan.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

# The words to look for, one per line, as "label<TAB>word" (the labels are what a hit prints).
PAT="$WORK/patterns.tsv"
: > "$PAT"
add() { [ ${#2} -ge 4 ] && printf '%s\t%s\n' "$1" "$2" >> "$PAT" || true; }
me=$(id -un)
add "home folder" "${HOME%/}"
add "user path" "/Users/$me"
add "user path" "/home/$me"
add "user path" "Users\\$me"
add "licensed content" "Houston Texans"
name=$(cd "$ROOT" && git config user.name || true)
mail=$(cd "$ROOT" && git config user.email || true)
[ "${name##* }" != "$name" ] && add "developer's name" "${name##* }"
[ -n "$mail" ] && add "developer's e-mail" "$mail" && add "developer's e-mail" "${mail%%@*}"
owner=$(cd "$ROOT" && git remote get-url origin 2>/dev/null | sed -E 's#.*[:/]([^/]+)/[^/]+$#\1#' || true)
[ -n "$owner" ] && add "private repo owner" "$owner"
for d in "$ROOT/build" ${BLITZ_SECRETS_DIR:-}; do
    if [ -f "$d/public_deny.txt" ]; then
        while IFS= read -r w; do w=${w%%#*}; w=$(echo "$w" | xargs); [ -n "$w" ] && add "public_deny.txt word" "$w"; done < "$d/public_deny.txt"
    fi
done

# Unpack every archive, nested ones too (an .apk or .ipa inside the Mac app).
n=0
unpack() {  # archive dest
    mkdir -p "$2"
    case "$1" in
        *.tar.gz|*.tgz)
            if tar -tvzf "$1" | awk '{print $3, $4}' | grep -qw -- "$me"; then
                echo "${1#"$WORK"/in.*/}: tar entries owned by the user name"; echo hit >> "$WORK/hits"
            fi
            tar -xzf "$1" -C "$2" ;;
        *.dmg)
            # Read-only, never shown in the Finder; its contents copied out (links stay links), then detached.
            local mnt="$WORK/mnt.$RANDOM$RANDOM"
            mkdir -p "$mnt"
            hdiutil attach -quiet -readonly -nobrowse -noautoopen -mountpoint "$mnt" "$1" \
                || { echo "${1#"$WORK"/in.*/}: can't attach"; echo hit >> "$WORK/hits"; return 0; }
            ditto "$mnt" "$2" || { echo "${1#"$WORK"/in.*/}: can't copy out"; echo hit >> "$WORK/hits"; }
            hdiutil detach -quiet "$mnt" || hdiutil detach -quiet -force "$mnt" ;;
        *.appimage|*.AppImage)
            # An AppImage is the runtime (an ELF file) with a squashfs straight after it: the ELF's own size says where.
            command -v unsquashfs >/dev/null || { echo "unsquashfs is needed for an AppImage: brew install squashfs"; exit 2; }
            local off
            off=$(python3 -I - "$1" <<'PY'
import struct, sys
d = open(sys.argv[1], "rb").read(1 << 22)
if d[:4] != b"\x7fELF" or d[4] != 2:
    sys.exit("not a 64-bit ELF")
shoff, = struct.unpack_from("<Q", d, 0x28)
shentsize, shnum = struct.unpack_from("<HH", d, 0x3A)
off = shoff + shentsize * shnum
if d[off:off + 4] != b"hsqs":
    off = d.find(b"hsqs")
if off < 0:
    sys.exit("no squashfs")
print(off)
PY
            ) || { echo "${1#"$WORK"/in.*/}: not an AppImage"; echo hit >> "$WORK/hits"; return 0; }
            local owners
            owners=$(unsquashfs -o "$off" -lln "$1" | awk '{print $2}' | sort -u)
            if grep -q "^$(id -u)/" <<< "$owners"; then
                echo "${1#"$WORK"/in.*/}: squashfs entries owned by this user"; echo hit >> "$WORK/hits"
            fi
            unsquashfs -q -n -f -o "$off" -d "$2" "$1" > /dev/null ;;
        *.exe|*.EXE)
            command -v 7z >/dev/null || { echo "7z is needed for the installer: brew install p7zip"; exit 2; }
            local listing
            listing=$(7z l "$1" 2>&1 || true)
            grep -q '^Type = Nsis' <<< "$listing" \
                || { echo "${1#"$WORK"/in.*/}: not an NSIS installer"; echo hit >> "$WORK/hits"; return 0; }
            7z x -y -bd -o"$2" "$1" > /dev/null ;;
        *) ditto -x -k "$1" "$2" 2>/dev/null || unzip -q -o "$1" -d "$2" ;;
    esac
}
for a in "$@"; do
    if [ -d "$a" ]; then cp -R "$a" "$WORK/in.$n"; else mkdir -p "$WORK/in.$n" && cp "$a" "$WORK/in.$n/"; fi
    n=$((n + 1))
done
for depth in 1 2 3 4; do
    found=0
    while IFS= read -r -d '' f; do
        unpack "$f" "$f!" && rm -f "$f"; found=1
    done < <(find "$WORK"/in.* -type f \( -iname '*.zip' -o -iname '*.ipa' -o -iname '*.apk' -o -iname '*.tar.gz' -o -iname '*.tgz' \
        -o -iname '*.dmg' -o -iname '*.appimage' -o -iname '*setup*.exe' \) -print0)
    [ $found = 1 ] || break
done

# Files that mustn't be there at all.
while IFS= read -r f; do
    echo "${f#"$WORK"/in.*/}: must not ship in a public build"; echo hit >> "$WORK/hits"
done < <(find "$WORK"/in.* \( -path '*/assets/res/logs_hub' -o -path '*/rosters/token' -o -path '*/rosters/hub' \
    -o -path '*/rosters/repo' -o -name '*.mobileprovision' -o -name 'NewBlitz.app.zip' -o -path '*/packs/blitz2026' \
    -o -path '*/houston/houston.json' -o -name 'blitz_logs_hub' \) -print)
# App bundles: signed ad hoc (or not at all), never for a team.
while IFS= read -r -d '' app; do
    t=$(codesign -dv "$app" 2>&1 | sed -n 's/^TeamIdentifier=//p')
    if [ -n "$t" ] && [ "$t" != "not set" ]; then echo "${app#"$WORK"/in.*/}: signed for a team"; echo hit >> "$WORK/hits"; fi
done < <(find "$WORK"/in.* -type d -name '*.app' -print0)

# The search: every file's bytes, each word in ASCII and UTF-16LE, case-insensitively; the secrets from build/ files;
# any *.workers.dev host but the season server's.
python3 -I - "$WORK" "$PAT" "$ROOT/build" ${BLITZ_SECRETS_DIR:-} <<'EOF' || echo hit >> "$WORK/hits"
import os, re, sys
work, patfile, secret_dirs = sys.argv[1], sys.argv[2], sys.argv[3:]
pats = []
for line in open(patfile, encoding="utf-8"):
    label, _, word = line.rstrip("\n").partition("\t")
    if word:
        pats.append((label, word.lower().encode()))

def host(u):
    m = re.match(r"https?://([^/:]+)", u)
    return m.group(1).lower() if m else ""

allowed_host = ""
for d in secret_dirs:
    try:
        allowed_host = allowed_host or host(open(os.path.join(d, "pak_server")).read().strip())
    except OSError:
        pass
# Private files in build/: every value in them (KEY=VALUE lines: the value), plus a URL's host unless it's the season
# server's. public_author (the public identity) and pak_server (the one allowed address) aren't secrets.
SECRET_FILES = ["rosters_token", "rosters_hub", "rosters_repo", "blitz_logs_hub", "android_install_link",
                "android_short_link", "ios_install_link", "psp_key", "psp_cmd_key", "pak_upload_secret", "pak_hub_key"]
for d in secret_dirs:
    for name in SECRET_FILES:
        try:
            text = open(os.path.join(d, name), encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        for line in text.splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            m = re.match(r"^(?:export\s+)?[A-Za-z_][A-Za-z0-9_]*=(.*)$", line)
            value = (m.group(1) if m else line).strip().strip("'\"")
            for v in re.split(r"\s+", value):
                if len(v) >= 8:
                    pats.append(("secret from build/" + name, v.rstrip("/").lower().encode()))
                    h = host(v)
                    if h and h != allowed_host:
                        pats.append(("private host from build/" + name, h.encode()))
# Each word's UTF-16LE form too.
pats += [(l + " (UTF-16)", w.decode().encode("utf-16-le")) for l, w in list(pats) if w.isascii()]
workers = re.compile(rb"[a-z0-9-]+(?:\.[a-z0-9-]+)*\.workers\.dev")
hits = 0
roots = [os.path.join(work, d) for d in sorted(os.listdir(work)) if d.startswith("in.")]
for top, _, files in (w for r in roots for w in os.walk(r)):
    for f in files:
        p = os.path.join(top, f)
        if not os.path.isfile(p) or os.path.islink(p):
            continue
        try:
            data = open(p, "rb").read().lower()
        except OSError:
            continue
        rel = p[len(work) + 1:].split("/", 1)[-1]
        seen = set()
        for label, w in pats:
            if label not in seen and data.find(w) >= 0:
                seen.add(label)
                print(f"{rel}: {label}")
                hits += 1
        for h in set(workers.findall(data)):
            if h.decode() != allowed_host:
                print(f"{rel}: a workers.dev host other than the season server's")
                hits += 1
                break
sys.exit(1 if hits else 0)
EOF

if [ -s "$WORK/hits" ]; then echo "release scan: FAILED (above)"; exit 1; fi
echo "release scan: clean ($(find "$WORK"/in.* -type f | wc -l | xargs) files)"
