#!/bin/bash
# New Blitz's saves on an iPhone, from the app's old bundle id to its new one (2026-11-04: com.blitzrecomp.newblitz,
# Classic com.blitzrecomp.newblitz.classic; platform/ios/ios_bundle.cmake). iOS gives each bundle id its own data
# container, so the new app starts empty. This copies the old app's saves into it with Xcode's devicectl:
#   1. finds the old app on the phone (the bundle id ending in .blitzrecomp.modern, or .classic; --from names it);
#   2. copies its saves to this Mac (build/app_id_migration/ios-<device>-<time>/files: the backup, kept);
#   3. copies them into the new app's container (install it with tools/ios/install.sh modern --no-launch, open it once,
#      close it from the app switcher, then run this), and checks each one by copying it back and comparing sha256.
# It never deletes anything of the old app.
#
#   tools/ios/migrate_app_id.sh [--classic] [--from <bundle id>] [--to <bundle id>] [--no-settings] [--no-rom]
#                               [--everything] [--backup-only] [--dry-run]       (BLITZ_IOS_DEVICE picks the phone)
# The app folder is "Library/Application Support/Blitz2026Recompiled" (Classic: .../BlitzRecompiled) in the container:
# saves/ (Controller Pak images, Season .adv saves, the runtime's .bin), savestates/, rules/, tournament.bin,
# checkpoint.bin, rosters/ (the saved crews, crews.json, with the downloaded rosters) and the Add-Ons (packs/, uniforms/,
# art/, announcer/, probowl/, houston/); settings are its *.json; the stored ROM the *.z64 there. Replays, screenshots
# and crash dumps only with --everything.
#
# If devicectl can't do it (an older Xcode, or the listing's JSON isn't understood): with the phone connected, Xcode >
# Window > Devices and Simulators > the phone > the old app > the gear > Download Container... saves an .xcappdata;
# its AppData/Library/Application Support/<folder> holds the files above. Install and open the new app once, then in
# the same window download its container, put the files into the same folder in it, and Replace Container... with it.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

FLAVOUR=modern OLD="" NEW="" SETTINGS=1 ROM=1 EVERYTHING=0 BACKUP_ONLY=0 DRY=0
while [ $# -gt 0 ]; do
    case "$1" in
        --classic) FLAVOUR=classic ;;
        --from) OLD=${2:?--from needs a bundle id}; shift ;;
        --to) NEW=${2:?--to needs a bundle id}; shift ;;
        --no-settings) SETTINGS=0 ;;
        --no-rom) ROM=0 ;;
        --everything) EVERYTHING=1 ;;
        --backup-only) BACKUP_ONLY=1 ;;
        --dry-run) DRY=1 ;;
        -h|--help) sed -n '2,/^set -euo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option $1 (--help)" >&2; exit 2 ;;
    esac
    shift
done
if [ "$FLAVOUR" = classic ]; then FOLDER=BlitzRecompiled; DEF_NEW=com.blitzrecomp.newblitz.classic
else FOLDER=Blitz2026Recompiled; DEF_NEW=com.blitzrecomp.newblitz; fi
NEW=${NEW:-$DEF_NEW}
BASE="Library/Application Support/$FOLDER"
XCRUN=${XCRUN:-xcrun}

say() { printf '%s\n' "$*"; }
step() { printf '\n==> %s\n' "$*"; }
die() { printf 'migrate: %s\n' "$1" >&2; exit "${2:-1}"; }
change() { if [ "$DRY" = 1 ]; then say "   (dry run) would: $*"; return 1; fi; return 0; }

"$XCRUN" devicectl --version >/dev/null 2>&1 || die "no devicectl: this needs Xcode 15 or later (or see the Xcode steps in this script's header)" 2
DEVICE=${BLITZ_IOS_DEVICE:-$("$XCRUN" devicectl list devices 2>/dev/null | grep -v simulated | grep -v unavailable | grep -i iphone | grep -oE '[0-9A-F]{8}-[0-9A-F]{16}' | head -1 || true)}
[ -n "$DEVICE" ] || die "no iPhone available: connect it (or pair it in Xcode) and unlock it" 2
W=$(mktemp -d "${TMPDIR:-/tmp}/blitz-ios-migrate.XXXXXX")
trap 'rm -rf "$W"' EXIT
dctl() { "$XCRUN" devicectl "$@" --device "$DEVICE"; }

# --- The two apps -----------------------------------------------------------------------------------------------------
dctl device info apps --json-output "$W/apps.json" >/dev/null 2>&1 || die "couldn't list the phone's apps (is it unlocked?)"
if [ -z "$OLD" ]; then
    cands=()
    while IFS= read -r c; do cands+=("$c"); done < <(grep -oE "\"[A-Za-z0-9.-]+\.blitzrecomp\.$FLAVOUR\"" "$W/apps.json" | tr -d '"' | grep -vxF "$NEW" | sort -u)
    [ "${#cands[@]}" -gt 0 ] || die "no old app on the phone (no bundle id ending in .blitzrecomp.$FLAVOUR)" 3
    [ "${#cands[@]}" -eq 1 ] || die "more than one old app (${cands[*]}): pick one with --from" 2
    OLD=${cands[0]}
fi
[ "$OLD" != "$NEW" ] || die "--from and --to are the same app" 2
say "iPhone $DEVICE"
say "old app $OLD -> new app $NEW (folder $BASE)"

# Every file under a folder of an app's container, relative to it, one a line (devicectl's JSON, read leniently: any
# object with a path and not marked a directory).
list_files() {  # bundle id, json out
    dctl device info files --domain-type appDataContainer --domain-identifier "$1" --subdirectory "$BASE" \
        --json-output "$2" >/dev/null 2>&1 || return 1
    python3 -I - "$2" "$BASE" <<'PY'
import json, sys
data = json.load(open(sys.argv[1]))
base = sys.argv[2].strip("/")
out = set()
def walk(o):
    if isinstance(o, dict):
        path = next((o[k] for k in ("relativePath", "path", "name") if isinstance(o.get(k), str)), None)
        kind = " ".join(str(o.get(k, "")) for k in ("type", "fileType", "kind")).lower()
        # Xcode 26's devicectl (checked 2026-11-04): result.files[] = {relativePath, metadata: {size}, resources: {isDirectory}}
        res = o.get("resources") if isinstance(o.get("resources"), dict) else {}
        meta = o.get("metadata") if isinstance(o.get("metadata"), dict) else {}
        is_dir = o.get("isDirectory") is True or res.get("isDirectory") is True or "dir" in kind or "folder" in kind
        sized = "size" in o or "fileSize" in o or "size" in meta or "isDirectory" in res
        if path and not is_dir and (sized or "file" in kind or "isDirectory" in o):
            p = path.strip("/")
            if p.startswith(base + "/"):
                p = p[len(base) + 1:]
            if p and p != base:
                out.add(p)
        for v in o.values():
            walk(v)
    elif isinstance(o, list):
        for v in o:
            walk(v)
walk(data)
print("\n".join(sorted(out)))
PY
}

wanted() {  # relative path -> copy it?
    local top=${1%%/*}
    case "$top" in
        # saves; rosters/ for the saved crews (rosters/crews.json); the Add-Ons, which nothing downloads again
        saves|savestates|rules|tournament.bin|checkpoint.bin|rosters|packs|uniforms|art|announcer|probowl|houston) return 0 ;;
        *.json|*.json.bak) [ "$SETTINGS" = 1 ] ;;
        *.z64) [ "$ROM" = 1 ] ;;
        *.log|*.log.*|test_env.txt|remote_input.txt|[Bb]"litz logs"*) return 1 ;;
        *) [ "$EVERYTHING" = 1 ] ;;
    esac
}

# --- 1. Back up the old app's files to the Mac --------------------------------------------------------------------------
STAMP=$(date +%Y%m%d-%H%M%S)
BK=${BLITZ_MIGRATE_DIR:-$ROOT/build/app_id_migration}/ios-$DEVICE-$STAMP
mkdir -p "$BK/files"
step "1. backing up $OLD's files to $BK"
say "   (close New Blitz on the phone first: swipe it away in the app switcher)"
list_files "$OLD" "$W/old.json" > "$W/old.list" || die "couldn't list the old app's files (see the Xcode steps: $0 --help)"
files=()
while IFS= read -r f; do [ -n "$f" ] && wanted "$f" && files+=("$f"); done < "$W/old.list"
[ "${#files[@]}" -gt 0 ] || die "no saves or settings found in the old app (its listing: $W/old.json; see the Xcode steps in --help)" 3
for f in "${files[@]}"; do
    mkdir -p "$BK/files/$(dirname "$f")"
    dctl device copy from --domain-type appDataContainer --domain-identifier "$OLD" --source "$BASE/$f" \
        --destination "$BK/files/$f" >/dev/null 2>&1 || die "couldn't copy $f from the phone"
    [ -f "$BK/files/$f" ] || die "$f didn't arrive"
done
(cd "$BK/files" && find . -type f -exec shasum -a 256 {} + | sed 's#  \./#  #' | sort -k2) > "$BK/backup.sums"
say "   $(wc -l < "$BK/backup.sums" | tr -d ' ') files"
[ "$BACKUP_ONLY" = 0 ] || { say "backup only: done"; exit 0; }

# --- 2. Into the new app -----------------------------------------------------------------------------------------------
step "2. copying into $NEW"
grep -qF "\"$NEW\"" "$W/apps.json" || die "$NEW isn't on the phone: make ios-modern, tools/ios/install.sh modern --no-launch, open it once, close it, and run this again (the backup is in $BK)" 4
list_files "$NEW" "$W/new.json" > /dev/null || die "the new app has no $BASE yet: open it once, close it, and run this again (the backup is in $BK)" 4
for f in "${files[@]}"; do
    if change "copy $f -> $NEW:$BASE/$f"; then
        dctl device copy to --domain-type appDataContainer --domain-identifier "$NEW" --source "$BK/files/$f" \
            --destination "$BASE/$f" >/dev/null 2>&1 ||
            die "couldn't copy $f to the new app (a folder the new app hasn't made yet? Then the Xcode steps in $0 --help do it)"
    fi
done

# --- 3. Check: every file back from the new app, compared ----------------------------------------------------------------
step "3. checking"
if [ "$DRY" = 1 ]; then say "   (dry run) nothing copied"; else
    mkdir -p "$W/back"
    for f in "${files[@]}"; do
        mkdir -p "$W/back/$(dirname "$f")"
        dctl device copy from --domain-type appDataContainer --domain-identifier "$NEW" --source "$BASE/$f" \
            --destination "$W/back/$f" >/dev/null 2>&1 || die "couldn't read $f back from the new app"
    done
    (cd "$W/back" && find . -type f -exec shasum -a 256 {} + | sed 's#  \./#  #' | sort -k2) > "$BK/new.sums"
    cmp -s "$BK/backup.sums" "$BK/new.sums" || { diff "$BK/backup.sums" "$BK/new.sums" | head -20; die "the new app's files don't match the backup (in $BK)"; }
    say "   $(wc -l < "$BK/new.sums" | tr -d ' ') files in $NEW, every one matching the old app's"
fi

step "done"
say "   The old app ($OLD) is as it was. Once the new one shows your saves, delete the old one from the Home Screen"
say "   (that deletes its data; the backup stays in $BK)."
