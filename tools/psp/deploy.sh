#!/bin/bash
# Puts the PSP kit on a Memory Stick (docs/PSP_PORT.md §5.3, §13 step 5): copies build/psp/kit/PSP/GAME/{NEWBLITZ,NBCLASSIC,NBPROBE,NBREMOTE}
# (make psp-kit first), then writes each game's data/net.cfg from the Mac's build/ secret files. Nothing secret is printed.
#
#   tools/psp/deploy.sh --ms /Volumes/<stick>    the stick in USB mode (Settings > USB Connection on the PSP)
#   tools/psp/deploy.sh --link                   build/psp/host0 (what PSPLink's usbhostfs_pc build/psp/host0 serves)
#   tools/psp/deploy.sh --to <folder>            any folder laid out as a stick (tests)
#   --no-net                                     copy the games but write no net.cfg (a build that leaves the team's hands)
#   --plugin on|off                              NB Remote's VSH plugin line in <stick>/SEPLUGINS/PLUGINS.TXT
#                                                ("vsh, ms0:/SEPLUGINS/nbremote.prx, on|off"); without it, PLUGINS.TXT is
#                                                left alone (the plugin file is copied either way, so off by default)
#   --home xmb                                   the games exit to the XMB, not to NB Remote (default: home=remote when
#                                                NBREMOTE is on the stick)
# NB Remote (docs/PSP_PORT.md "NB Remote"): PSP/GAME/NBREMOTE gets its own data/net.cfg (logs_base, cmd_key, wifi_profile,
# device, plugin_autostart=on, plugin_delay_s=15, plugin_dry_run=$PLUGIN_DRY_RUN (default 0), plugin_net=off). WIFI_PROFILE=
# sets every net.cfg's first slot to try (default 1; each program tries every saved slot anyway).
#
# net.cfg (never compiled in, never printed; umask 077):
#   logs_base=http://<the logs Worker's host>/<PSP_KEY>   host from build/blitz_logs_hub's UPLOAD, key from build/psp_key
#   hub_base=http://<the rosters hub's host>/<HUB_KEY>    build/rosters_hub, switched to http:// (the PSP has no TLS)
#   cmd_key=<hex>                                          build/psp_cmd_key (signs Worker commands, §5.3)
#   wifi_profile=1, device=psp1001
# Without build/psp_key (the Worker's PSP scope isn't deployed yet, open question 6) there is no logs_base: the network log sink
# and the Worker commands stay off, and the rosters hub still works. Removing net.cfg turns the network off altogether.
# The ROMs go only to the stick: they're the team's game data, internal use only.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KIT="$ROOT/build/psp/kit/PSP/GAME"
DEST="" NET=1 PLUGIN="" HOME_TO=remote
while [ $# -gt 0 ]; do
    case "$1" in
        --plugin) PLUGIN="${2:?--plugin on|off}"; shift 2; [ "$PLUGIN" = on ] || [ "$PLUGIN" = off ] || { echo "--plugin on|off" >&2; exit 2; } ;;
        --home) HOME_TO="${2:?--home remote|xmb}"; shift 2 ;;
        --ms) DEST="${2:?--ms needs the mount point of the stick}"; shift 2 ;;
        --link) DEST="$ROOT/build/psp/host0"; shift ;;
        --to) DEST="${2:?--to needs a folder}"; shift 2 ;;
        --no-net) NET=0; shift ;;
        *) echo "usage: $0 --ms /Volumes/<stick> | --link | --to <folder> [--no-net] [--plugin on|off] [--home xmb]" >&2; exit 2 ;;
    esac
done
[ -n "$DEST" ] || { echo "usage: $0 --ms /Volumes/<stick> | --link | --to <folder> [--no-net]" >&2; exit 2; }
[ -d "$KIT" ] || { echo "no kit at $KIT (make psp-kit)" >&2; exit 2; }
case "$DEST" in /Volumes/*) [ -d "$DEST" ] || { echo "$DEST isn't mounted (USB mode on the PSP?)" >&2; exit 2; } ;; esac
mkdir -p "$DEST/PSP/GAME"

for g in NEWBLITZ NBCLASSIC NBPROBE NBREMOTE; do
    [ -d "$KIT/$g" ] || { echo "$g: not in the kit, skipped"; continue; }
    mkdir -p "$DEST/PSP/GAME/$g"
    # rsync keeps what the PSP wrote there (logs/, saves/, configs, rosters it downloaded) and copies only what changed;
    # the bundled rosters/ never replaces a newer download, and nothing is deleted.
    rsync -rt --exclude 'rosters/' --exclude 'data/net.cfg' --exclude '/season.json' "$KIT/$g/" "$DEST/PSP/GAME/$g/"
    if [ -f "$KIT/$g/season.json" ] && [ ! -f "$DEST/PSP/GAME/$g/season.json" ]; then cp "$KIT/$g/season.json" "$DEST/PSP/GAME/$g/"; fi
    if [ -d "$KIT/$g/rosters" ] && [ ! -f "$DEST/PSP/GAME/$g/rosters/pack.txt" ]; then
        mkdir -p "$DEST/PSP/GAME/$g/rosters" && cp "$KIT/$g/rosters/"* "$DEST/PSP/GAME/$g/rosters/"
    fi
    echo "$g: copied"
done
[ -f "$ROOT/build/psp/kit/README.txt" ] && cp "$ROOT/build/psp/kit/README.txt" "$DEST/NEWBLITZ-README.txt"
# NB Remote's VSH plugin: the file always, the PLUGINS.TXT line only when asked (--plugin on|off). ARK reads both
# PSP/SAVEDATA/ARK_01234/PLUGINS.TXT (ARK's own, never touched here) and SEPLUGINS/PLUGINS.TXT (this one).
if ls "$ROOT/build/psp/kit/SEPLUGINS/"nbremote*.prx >/dev/null 2>&1; then
    mkdir -p "$DEST/SEPLUGINS" && cp "$ROOT/build/psp/kit/SEPLUGINS/"nbremote*.prx "$DEST/SEPLUGINS/"
    PT="$DEST/SEPLUGINS/PLUGINS.TXT"
    if [ -n "$PLUGIN" ]; then
        # the other lines kept as they are (grep's "no other lines" status is fine); a write that fails (a full stick) leaves
        # PLUGINS.TXT untouched rather than cut short
        if { if [ -f "$PT" ]; then grep -vi 'nbremote' "$PT" || [ $? = 1 ]; fi; printf 'vsh, ms0:/SEPLUGINS/nbremote.prx, %s\n' "$PLUGIN"; } > "$PT.part"; then
            mv "$PT.part" "$PT"
        else
            rm -f "$PT.part"; echo "couldn't write $PT.part: PLUGINS.TXT left as it was" >&2; exit 1
        fi
        echo "SEPLUGINS: nbremote.prx copied, PLUGINS.TXT line set to $PLUGIN"
    elif [ -f "$PT" ] && grep -qiE 'nbremote\.prx, *(on|1|true|enabled)' "$PT"; then
        echo "SEPLUGINS: nbremote.prx copied (its PLUGINS.TXT line is on)"
    else
        echo "SEPLUGINS: nbremote.prx copied, not enabled (make psp-deploy PLUGIN=on to enable it)"
    fi
    ARKPT="$DEST/PSP/SAVEDATA/ARK_01234/PLUGINS.TXT"
    [ -f "$ARKPT" ] && grep -qi nbremote "$ARKPT" && echo "note: ARK's own PLUGINS.TXT ($ARKPT) also names nbremote: that line counts too"
fi
if [ -d "$ROOT/build/psp/psplink/psplink" ]; then
    mkdir -p "$DEST/PSP/GAME/PSPLINK" && rsync -rt "$ROOT/build/psp/psplink/psplink/" "$DEST/PSP/GAME/PSPLINK/" && echo "PSPLINK: copied"
fi

if [ "$NET" = 1 ]; then
    # Each value is read into a variable and written with printf: never echoed, never on a command line (ps can't show it).
    logs_base="" hub_base="" cmd_key=""
    if [ -s "$ROOT/build/psp_key" ]; then
        if [ -f "$ROOT/build/blitz_logs_hub" ]; then
            host="$( . "$ROOT/build/blitz_logs_hub"; u="${UPLOAD#*://}"; printf '%s' "${u%%/*}")"
            key="$(tr -d ' \r\n' < "$ROOT/build/psp_key")"
            [ -n "$host" ] && [ -n "$key" ] && logs_base="http://$host/$key"
        else
            echo "no build/blitz_logs_hub: no logs_base (the network log sink stays off)"
        fi
    else
        echo "the Worker key isn't set up yet (build/psp_key): no logs_base, so the network log sink and Worker commands stay off"
    fi
    if [ -s "$ROOT/build/rosters_hub" ]; then
        h="$(tr -d ' \r\n' < "$ROOT/build/rosters_hub")"
        h="${h%/}"
        hub_base="http://${h#*://}"
    else
        echo "no build/rosters_hub: no hub_base (the bundled rosters only)"
    fi
    [ -s "$ROOT/build/psp_cmd_key" ] && cmd_key="$(tr -d ' \r\n' < "$ROOT/build/psp_cmd_key")"
    [ -n "$logs_base" ] && [ -z "$cmd_key" ] && echo "no build/psp_cmd_key: Worker commands can't be signed, so the PSP takes none"
    home=""
    [ "$HOME_TO" = remote ] && [ -f "$DEST/PSP/GAME/NBREMOTE/EBOOT.PBP" ] && home=remote
    for g in NEWBLITZ NBCLASSIC NBPROBE NBREMOTE; do
        [ -d "$DEST/PSP/GAME/$g" ] || continue
        mkdir -p "$DEST/PSP/GAME/$g/data"
        cfg="$DEST/PSP/GAME/$g/data/net.cfg"
        (
            umask 077
            {
                [ -n "$logs_base" ] && printf 'logs_base=%s\n' "$logs_base"
                [ -n "$hub_base" ] && [ "$g" != NBREMOTE ] && printf 'hub_base=%s\n' "$hub_base"
                [ -n "$cmd_key" ] && printf 'cmd_key=%s\n' "$cmd_key"
                printf 'wifi_profile=%s\ndevice=psp1001\n' "${WIFI_PROFILE:-1}"
                if [ "$g" = NBREMOTE ]; then
                    printf 'plugin_autostart=on\nplugin_delay_s=15\nplugin_dry_run=%s\nplugin_net=off\n' "${PLUGIN_DRY_RUN:-0}"
                elif [ -n "$home" ]; then
                    printf 'home=remote\n'
                fi
            } > "$cfg.part"
            mv "$cfg.part" "$cfg"
        )
        echo "$g: net.cfg written$( [ "$g" != NBREMOTE ] && [ -n "$home" ] && echo ' (home=remote: exits go to NB Remote)')"
    done
else
    for g in NEWBLITZ NBCLASSIC NBPROBE NBREMOTE; do rm -f "$DEST/PSP/GAME/$g/data/net.cfg"; done
    echo "no net.cfg (--no-net): the network is off on every game"
fi
case "$DEST" in /Volumes/*) sync ;; esac
echo "deploy done: $DEST"
