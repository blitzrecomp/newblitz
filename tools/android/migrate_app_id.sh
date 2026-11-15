#!/bin/bash
# New Blitz's saves on an Android device, from the app's old id to its new one (2026-11-04: com.blitzrecomp.newblitz,
# Classic com.blitzrecomp.newblitz.classic; docs/ANDROID.md). Android keeps each app's files under its package name, so
# the new app starts empty. This copies the old app's saves into it over adb:
#   1. finds the old app (the package ending in .blitzrecomp.modern, or .classic with --classic; --from names it);
#   2. pulls its saves to this Mac (build/app_id_migration/android-<serial>-<time>/files: the backup, kept), checking
#      each file against the device's own sha256 (sizes where the device has no sha256sum);
#   3. pushes them into the new app's folder, which the new app makes at its first launch (install it, open it once,
#      quit it, then run this), and checks them there the same way;
#   4. with a root shell (the RG DS's adb is root), gives the pushed files to the new app's user and carries over the
#      Swap Screens / Potato Mode switches (shared_prefs/blitz_ds.xml), which live in the app's private data;
#   5. prints what the device's own settings need for the new id (GammaOS DualStack list, the overlay permission) and
#      sets them only when asked (--device-settings, or yes at the prompt) and the device lets it.
# It never deletes anything of the old app, and never starts an app (on GammaOS, start New Blitz from its own menu).
#
#   tools/android/migrate_app_id.sh [options]
#     -s <serial>          the device (default: the only one connected; or ANDROID_SERIAL)
#     --classic            the Classic app instead of New Blitz
#     --from <package>     the old app's package, when there's more than one candidate
#     --to <package>       the new app's package (default com.blitzrecomp.newblitz[.classic])
#     --no-settings        saves only: no settings files (*.json) and no shared_prefs
#     --no-rom             leave the stored ROM out (the new app asks for one again)
#     --everything         the whole app folder but logs and test switches (replays, screenshots, dumps ...)
#     --backup-only        pull to the Mac and stop
#     --device-settings    apply the DualStack list / overlay permission without asking
#     --dry-run            pull only (the backup); print every change it would make on the device
# Saves are <app folder>/saves (Controller Pak images saves/**/blitz2000_pak*.mpk, Season saves saves/advanced/**/*.adv,
# the runtime's saves/*.bin), savestates/ (slots, Auto, Save & Quit), rules/ (house-rule presets), tournament.bin,
# checkpoint.bin and rosters/ (the saved crews, rosters/crews.json, with the downloaded rosters); the Add-Ons (packs/,
# uniforms/, art/, announcer/, probowl/, houston/) go too. Settings are the *.json files and gfx_limits.txt; the stored ROM
# is the *.z64 at the top. Left out unless --everything (and named when it runs): replays, screenshots, crash dumps.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

SERIAL=${ANDROID_SERIAL:-}
FLAVOUR=modern OLD="" NEW="" SETTINGS=1 ROM=1 EVERYTHING=0 BACKUP_ONLY=0 DEVICE_SETTINGS=0 DRY=0
while [ $# -gt 0 ]; do
    case "$1" in
        -s) SERIAL=${2:?-s needs a serial}; shift ;;
        --classic) FLAVOUR=classic ;;
        --from) OLD=${2:?--from needs a package}; shift ;;
        --to) NEW=${2:?--to needs a package}; shift ;;
        --no-settings) SETTINGS=0 ;;
        --no-rom) ROM=0 ;;
        --everything) EVERYTHING=1 ;;
        --backup-only) BACKUP_ONLY=1 ;;
        --device-settings) DEVICE_SETTINGS=1 ;;
        --dry-run) DRY=1 ;;
        -h|--help) sed -n '2,/^set -euo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option $1 (--help)" >&2; exit 2 ;;
    esac
    shift
done
if [ -z "$NEW" ]; then
    if [ "$FLAVOUR" = classic ]; then NEW=com.blitzrecomp.newblitz.classic; else NEW=com.blitzrecomp.newblitz; fi
fi

ADB=${ADB:-}
if [ -z "$ADB" ]; then
    ADB="${ANDROID_HOME:-$HOME/Library/Android/sdk}/platform-tools/adb"
    [ -x "$ADB" ] || ADB=adb
fi
command -v "$ADB" >/dev/null || { echo "no adb (the Android SDK's platform-tools)" >&2; exit 2; }

say() { printf '%s\n' "$*"; }
step() { printf '\n==> %s\n' "$*"; }
die() { printf 'migrate: %s\n' "$1" >&2; exit "${2:-1}"; }
# A change on the device: done, or only printed with --dry-run.
change() { if [ "$DRY" = 1 ]; then say "   (dry run) would: $*"; return 1; fi; return 0; }

# --- The device -------------------------------------------------------------------------------------------------------
if [ -z "$SERIAL" ]; then
    devs=()
    while IFS= read -r d; do devs+=("$d"); done < <("$ADB" devices | awk 'NR > 1 && $2 == "device" { print $1 }')
    [ "${#devs[@]}" -gt 0 ] || die "no device: connect it with USB debugging on (adb devices)" 2
    [ "${#devs[@]}" -eq 1 ] || die "more than one device (${devs[*]}): pick one with -s <serial>" 2
    SERIAL=${devs[0]}
fi
A=("$ADB" -s "$SERIAL")
dsh() { "${A[@]}" shell "$*" | tr -d '\r'; }   # a command on the device (its shell joins the words)

# A root shell, if there is one: the shell itself (GammaOS userdebug), or su.
ROOTSH=""
if [ "$(dsh 'id -u' 2>/dev/null)" = 0 ]; then ROOTSH=shell
elif [ "$(dsh 'su 0 id -u' 2>/dev/null)" = 0 ]; then ROOTSH=su0
elif [ "$(dsh "su -c 'id -u'" 2>/dev/null)" = 0 ]; then ROOTSH=suc
fi
rsh() {  # a command as root (only called when ROOTSH is set; the command has no single quotes)
    case "$ROOTSH" in
        shell) dsh "$*" ;;
        su0) dsh "su 0 sh -c '$*'" ;;
        suc) dsh "su -c '$*'" ;;
        *) return 1 ;;
    esac
}

# --- The two apps -----------------------------------------------------------------------------------------------------
installed() { dsh 'pm list packages' | sed -n 's/^package://p'; }
if [ -z "$OLD" ]; then
    cands=()
    while IFS= read -r c; do cands+=("$c"); done < <({ installed; dsh 'ls -1 /sdcard/Android/data' 2>/dev/null; } |
        grep -E "\.blitzrecomp\.$FLAVOUR\$" | grep -vxF "$NEW" | sort -u)
    [ "${#cands[@]}" -gt 0 ] || die "no old app on $SERIAL (nothing ending in .blitzrecomp.$FLAVOUR, installed or with a folder)" 3
    [ "${#cands[@]}" -eq 1 ] || die "more than one old app (${cands[*]}): pick one with --from" 2
    OLD=${cands[0]}
fi
[ "$OLD" != "$NEW" ] || die "--from and --to are the same app" 2
OLD_DIR=/sdcard/Android/data/$OLD/files
NEW_DIR=/sdcard/Android/data/$NEW/files
if [ -n "$ROOTSH" ]; then say "device $SERIAL (root shell)"; else say "device $SERIAL"; fi
say "old app $OLD -> new app $NEW"
[ "$(dsh "[ -d '$OLD_DIR' ] && echo yes")" = yes ] || die "the old app has no folder ($OLD_DIR): nothing to copy" 3

# --- What to copy -----------------------------------------------------------------------------------------------------
items=() left=()
while IFS= read -r n; do
    case "$n" in
        # saves; rosters/ holds the saved crews (crews.json) beside the downloaded rosters; the Add-Ons (Settings >
        # Add-Ons: packs, uniforms, art, announcer; probowl and houston by hand), which nothing downloads again
        saves|savestates|rules|tournament.bin|checkpoint.bin|rosters|packs|uniforms|art|announcer|probowl|houston) items+=("$n") ;;
        *.json|*.json.bak|gfx_limits.txt) if [ "$SETTINGS" = 1 ]; then items+=("$n"); else left+=("$n"); fi ;;  # gfx_limits: what a handheld stepped down to
        *.z64) if [ "$ROM" = 1 ]; then items+=("$n"); else left+=("$n"); fi ;;
        *.log|*.log.*|remote_input.txt|test_env.txt|ds_stack|ds_stack_test|[Bb]"litz logs"*) ;;  # logs, test switches
        *) if [ "$EVERYTHING" = 1 ]; then items+=("$n"); else left+=("$n"); fi ;;
    esac
done < <(dsh "ls -1a '$OLD_DIR'" | grep -vxE '\.|\.\.')
[ "${#items[@]}" -gt 0 ] || die "the old app's folder has no saves or settings" 3
say "copying: ${items[*]}"
[ "${#left[@]}" -eq 0 ] || say "left out: ${left[*]} (see --help: --everything, --no-settings, --no-rom)"

# sha256 (or size) of every file under the named items of a device folder: "<sum> <relative path>" lines, sorted.
HASH="sha256sum"
dsh 'command -v sha256sum' >/dev/null 2>&1 || { HASH=""; say "(no sha256sum on the device: checking sizes)"; }
quoted() { local q="" i; for i in "$@"; do q+=" '$i'"; done; printf '%s' "$q"; }
remote_sums() {  # folder, items...
    local dir=$1; shift
    if [ -n "$HASH" ]; then
        dsh "cd '$dir' && find$(quoted "$@") -type f -exec sha256sum {} + 2>/dev/null"
    else
        dsh "cd '$dir' && find$(quoted "$@") -type f -exec wc -c {} + 2>/dev/null" | grep -v ' total$'
    fi | awk '{ s = $1; $1 = ""; sub(/^ +/, ""); sub(/^\.\//, ""); print s, $0 }' | sort -k2
}
local_sums() {  # folder, items...
    local dir=$1; shift
    (cd "$dir" && if [ -n "$HASH" ]; then find "$@" -type f -exec shasum -a 256 {} +; else find "$@" -type f -exec wc -c {} +; fi) |
        grep -v ' total$' | awk '{ s = $1; $1 = ""; sub(/^ +/, ""); sub(/^\.\//, ""); print s, $0 }' | sort -k2
}

# --- 1. Back up the old app's files to the Mac --------------------------------------------------------------------------
STAMP=$(date +%Y%m%d-%H%M%S)
BK=${BLITZ_MIGRATE_DIR:-$ROOT/build/app_id_migration}/android-$SERIAL-$STAMP
mkdir -p "$BK/files"
step "1. backing up $OLD's files to $BK"
if change "am force-stop $OLD (so nothing is mid-write)"; then dsh "am force-stop $OLD" >/dev/null; fi
remote_sums "$OLD_DIR" "${items[@]}" > "$BK/old.sums"
for it in "${items[@]}"; do
    "${A[@]}" pull "$OLD_DIR/$it" "$BK/files/" >/dev/null || die "couldn't pull $it"
done
local_sums "$BK/files" "${items[@]}" > "$BK/backup.sums"
cmp -s "$BK/old.sums" "$BK/backup.sums" || { diff "$BK/old.sums" "$BK/backup.sums" | head -20; die "the backup doesn't match the device"; }
say "   $(wc -l < "$BK/backup.sums" | tr -d ' ') files, all matching the device ($( [ -n "$HASH" ] && echo sha256 || echo sizes))"
PREFS_SRC=/data/data/$OLD/shared_prefs/blitz_ds.xml
if [ "$SETTINGS" = 1 ] && [ -n "$ROOTSH" ] && [ "$(rsh "[ -f $PREFS_SRC ] && echo yes")" = yes ]; then
    mkdir -p "$BK/shared_prefs"
    rsh "cat $PREFS_SRC" > "$BK/shared_prefs/blitz_ds.xml"
    say "   + shared_prefs/blitz_ds.xml (Swap Screens, Potato Mode)"
fi
[ "$BACKUP_ONLY" = 0 ] || { say "backup only: done"; exit 0; }

# --- 2. Into the new app -----------------------------------------------------------------------------------------------
step "2. copying into $NEW"
if [ -z "$(dsh "pm path $NEW" 2>/dev/null)" ]; then
    die "$NEW isn't installed: install the new build (make android-modern android-install, or Blitz Builder), open it once, quit it, and run this again (the backup is in $BK)" 4
fi
if [ "$(dsh "[ -d '$NEW_DIR' ] && echo yes")" != yes ]; then
    die "$NEW has no folder yet: open it once (on GammaOS from its own menu), quit it, and run this again (the backup is in $BK)" 4
fi
if change "am force-stop $NEW (it mustn't write its own saves over these)"; then dsh "am force-stop $NEW" >/dev/null; fi
# Pushed world-readable and -writable: a file adb creates in another app's folder isn't that app's, and the game
# rewrites its saves in place. With a root shell they're given to the app below.
STAGE=$(mktemp -d "${TMPDIR:-/tmp}/blitz-migrate.XXXXXX")
trap 'rm -rf "$STAGE"' EXIT
cp -R "$BK/files/." "$STAGE/"
find "$STAGE" -type d -exec chmod 0777 {} +
find "$STAGE" -type f -exec chmod 0666 {} +
for it in "${items[@]}"; do
    if change "adb push $it -> $NEW_DIR/"; then "${A[@]}" push "$STAGE/$it" "$NEW_DIR/" >/dev/null || die "couldn't push $it"; fi
done
if [ -n "$ROOTSH" ]; then
    LOWER=/data/media/0/Android/data/$NEW/files   # the folder under FUSE; else the /sdcard path itself
    [ "$(rsh "[ -d $LOWER ] && echo yes")" = yes ] || LOWER=$NEW_DIR
    OWNER=$(rsh "stat -c %u:%g $LOWER")
    for it in "${items[@]}"; do
        if change "chown -R $OWNER $LOWER/$it"; then rsh "chown -R $OWNER \"$LOWER/$it\"" || say "   (couldn't chown $it: the app may only read it)"; fi
    done
    if [ -f "$BK/shared_prefs/blitz_ds.xml" ]; then
        APPID=$(rsh "stat -c %u:%g /data/data/$NEW")
        if change "shared_prefs/blitz_ds.xml -> /data/data/$NEW/shared_prefs (owner $APPID)"; then
            "${A[@]}" push "$BK/shared_prefs/blitz_ds.xml" /data/local/tmp/blitz_ds.xml >/dev/null
            rsh "mkdir -p /data/data/$NEW/shared_prefs && cp /data/local/tmp/blitz_ds.xml /data/data/$NEW/shared_prefs/blitz_ds.xml && chown -R $APPID /data/data/$NEW/shared_prefs && chmod 771 /data/data/$NEW/shared_prefs && chmod 660 /data/data/$NEW/shared_prefs/blitz_ds.xml && restorecon -R /data/data/$NEW/shared_prefs; rm -f /data/local/tmp/blitz_ds.xml"
        fi
    fi
else
    say "   no root shell: the files are the shell's, world-writable. If the game says it can't write a Controller Pak"
    say "   ('Saving is off this session'), copy them in with a file manager on the device instead (from $BK/files)."
fi

# --- 3. Check -----------------------------------------------------------------------------------------------------------
step "3. checking"
if [ "$DRY" = 0 ]; then
    remote_sums "$NEW_DIR" "${items[@]}" > "$BK/new.sums"
    # Every backed-up file, the same in the new app (which may have more of its own, e.g. a blank pak it made)
    bad=$(grep -vxFf "$BK/new.sums" "$BK/backup.sums" || true)
    if [ -z "$bad" ]; then
        say "   $(wc -l < "$BK/backup.sums" | tr -d ' ') files in $NEW, every one matching the old app's"
    else
        printf '%s\n' "$bad" | head -20
        die "those files in the new app don't match the backup (in $BK): run this again"
    fi
else
    say "   (dry run) nothing copied"
fi

# --- 4. The device's own settings ---------------------------------------------------------------------------------------
step "4. device settings for the new id"
todo=()
DS=$(dsh 'getprop persist.gammaos.dualstack.pkgs')
if [ -n "$DS" ]; then
    if printf ',%s,' "$DS" | grep -q ",$OLD,"; then
        DS_NEW=$(printf '%s' "$DS" | tr ',' '\n' | awk -v o="$OLD" -v n="$NEW" '{ if ($0 == o) $0 = n } !seen[$0]++' | paste -sd, -)
        say "   GammaOS DualStack (two screens as one): persist.gammaos.dualstack.pkgs is '$DS'"
        say "   it needs '$DS_NEW' (the old id swapped for the new one)."
        say "   Never start New Blitz with am start while it's on that list (that hung the device): only from GammaOS's menu."
        todo+=(dualstack)
    elif printf ',%s,' "$DS" | grep -q ",$NEW,"; then
        say "   GammaOS DualStack: $NEW is already on the list ('$DS')"
    else
        say "   GammaOS DualStack list: '$DS' (New Blitz isn't on it: two screens the usual way; nothing to change)"
    fi
fi
# Anything else of the device's that names the old app (a launcher's or the firmware's own per-app settings): shown
# only, to change by hand.
others=$( { dsh 'getprop' | grep -F "$OLD" | grep -vF 'persist.gammaos.dualstack.pkgs' | sed 's/^/   property /'
    for ns in global secure system; do dsh "settings list $ns" 2>/dev/null | grep -F "$OLD" | sed "s/^/   setting ($ns) /"; done; } || true)
if [ -n "$others" ]; then
    say "   these name the old app too (change them by hand if they matter: the old id -> $NEW):"
    printf '%s\n' "$others"
fi
OV_OLD=$(dsh "appops get $OLD SYSTEM_ALERT_WINDOW" 2>/dev/null || true)
OV_NEW=$(dsh "appops get $NEW SYSTEM_ALERT_WINDOW" 2>/dev/null || true)
if printf '%s' "$OV_OLD" | grep -q allow && ! printf '%s' "$OV_NEW" | grep -q allow; then
    say "   'Display over other apps' (restarting the game after a crash): allowed for the old app, not the new one."
    todo+=(overlay)
fi
if [ "${#todo[@]}" -gt 0 ]; then
    apply=$DEVICE_SETTINGS
    if [ "$apply" = 0 ] && [ "$DRY" = 0 ] && [ -t 0 ]; then
        read -r -p "   Set ${todo[*]} for $NEW now? [y/N] " ans || ans=""
        case "$ans" in y|Y|yes) apply=1 ;; esac
    fi
    for t in "${todo[@]}"; do
        case "$t" in
            dualstack)
                if [ "$apply" = 1 ] && [ -n "$ROOTSH" ] && change "setprop persist.gammaos.dualstack.pkgs $DS_NEW"; then
                    rsh "setprop persist.gammaos.dualstack.pkgs $DS_NEW"
                    if [ "$(dsh 'getprop persist.gammaos.dualstack.pkgs')" = "$DS_NEW" ]; then say "   DualStack list set"; else say "   the DualStack list didn't take: set it by hand (below)"; fi
                else
                    say "   by hand, in a root shell on the device:  setprop persist.gammaos.dualstack.pkgs '$DS_NEW'"
                fi ;;
            overlay)
                if [ "$apply" = 1 ] && change "appops set $NEW SYSTEM_ALERT_WINDOW allow"; then
                    dsh "appops set $NEW SYSTEM_ALERT_WINDOW allow"; say "   overlay permission allowed"
                else
                    say "   by hand: Settings > Apps > New Blitz > Display over other apps (or: adb shell appops set $NEW SYSTEM_ALERT_WINDOW allow)"
                fi ;;
        esac
    done
else
    say "   nothing to change"
fi

step "done"
say "   The old app ($OLD) and its folder are as they were. Once the new one has your saves (open it from the launcher),"
say "   uninstall the old one in Android's Settings; that deletes its folder, and the backup stays in $BK."
