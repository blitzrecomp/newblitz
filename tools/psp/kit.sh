#!/bin/bash
# Lays out the PSP games (docs/PSP_PORT.md §4.3) as they go on a Memory Stick:
#   <out>/PSP/GAME/NEWBLITZ    New Blitz (Modern): EBOOT.PBP, rom/blitz.z64 (Modern's art merged on the Mac) + .sha1, rom/kit.txt,
#                              rom/artfs.bin, rosters/pack.txt + manifest.json (the bundled roster snapshot), houston/houston.json,
#                              packs/blitz2026, data/ (no net.cfg: tools/psp/deploy.sh writes it)
#   <out>/PSP/GAME/NBCLASSIC   New Blitz Classic: EBOOT.PBP, rom/blitz.z64 (the plain ROM) + .sha1, data/
#   <out>/PSP/GAME/NBPROBE     the probe: EBOOT.PBP (it reads ../NBCLASSIC/rom/blitz.z64; nothing copied twice)
#   <out>/PSP/GAME/NBREMOTE    NB Remote (docs/PSP_PORT.md "NB Remote"): EBOOT.PBP, data/
#   <out>/SEPLUGINS            nbremote.prx (+ nbremote_net.prx), NB Remote's ARK VSH plugin: copied, never enabled, by deploy.sh
#   <out>/README.txt           what is where, and the team's checklist pointer (docs/PSP_PORT.md §13)
#
#   tools/psp/kit.sh             build/psp/kit/...          (the folder tools/psp/deploy.sh copies)
#   tools/psp/kit.sh --ppsspp    PPSSPP's memory stick, ~/.config/ppsspp/PSP/GAME/... (MEMSTICK= for another)
#
# The base ROM is the normalized Classic ROM the desktop build uses (build/baserom.z64, big-endian v1.1, SHA-1 3754c8fd...;
# ROM= for another). It is checked here, then copied only if the copy there differs. ROMs and art are game data: they never
# leave build/, ~/.config/ppsspp or the Memory Stick, and nothing here sends them anywhere.
#
# Modern's ROM is merged by build/psp/host_modern/newblitz-kit (make psp-host FLAVOUR=modern), which runs the desktop's own
# art_dropin.cpp and game_init.cpp on the base ROM with the same content the desktop's make app-modern puts in the app:
# art/blitz2026/files and logo27 (the '27 logo), <APP_FOLDER>/art/files, art/vs_right, art/credits.wms, uniforms, announcer
# (APP_FOLDER defaults to build/appdata2026, as for make app-modern), build/engine32/houston, assets/pad_glyphs, packs/blitz2026.
# The rosters snapshot is tools/psp/make_pack.py over ROSTERS (default: the newer of build/appdata2026/rosters and
# build/blitz-rosters).
# The sidecar rom/blitz.z64.sha1 is what the PSP trusts instead of hashing the ROM: "<sha1>  blitz.z64", then size, the header
# CRCs (0x10-0x17) and the XXH3-64 the runtime expects (blitz_config.h's rom_hash; Modern: its base ROM's, plus merged_xxh3).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PY="$ROOT/.venv/bin/python"
OUT="$ROOT/build/psp/kit"
if [ "${1:-}" = "--ppsspp" ]; then OUT="${MEMSTICK:-$HOME/.config/ppsspp}"; fi
ROM="${ROM:-$ROOT/build/baserom.z64}"
APP_FOLDER="${APP_FOLDER:-$ROOT/build/appdata2026}"
WANT_SHA1=3754c8fdf5cdce5de62685ce9be73e06ba5e3577
WANT_XXH3=45E521F9499A7BA5

[ -f "$ROM" ] || { echo "no ROM at $ROM" >&2; exit 2; }
SHA1="$(shasum "$ROM" | cut -c1-40)"
[ "$SHA1" = "$WANT_SHA1" ] || { echo "$ROM is not the Classic ROM (SHA-1 $SHA1)" >&2; exit 2; }

GAME="$OUT/PSP/GAME"
mkdir -p "$GAME/NBCLASSIC/rom" "$GAME/NBCLASSIC/data" "$GAME/NBPROBE" "$GAME/NEWBLITZ/rom" "$GAME/NEWBLITZ/data"
copy_if_changed() { if ! cmp -s "$1" "$2" 2>/dev/null; then cp "$1" "$2"; fi; }

# ---- NBCLASSIC: the plain ROM ----
DST="$GAME/NBCLASSIC/rom/blitz.z64"
copy_if_changed "$ROM" "$DST"
ROM_FILE="$DST" WANT="$WANT_XXH3" SHA1="$SHA1" "$PY" -I -c '
import os, xxhash
p, want, sha1 = os.environ["ROM_FILE"], os.environ["WANT"], os.environ["SHA1"]
data = open(p, "rb").read()
x = "%016X" % xxhash.xxh3_64_intdigest(data)
if x != want:
    raise SystemExit(f"{p}: XXH3 {x}, expected {want}")
crc = data[0x10:0x14].hex().upper() + " " + data[0x14:0x18].hex().upper()
open(p + ".sha1", "w").write(f"{sha1}  blitz.z64\nsize {len(data)}\ncrc {crc}\nxxh3 {x}\n")
'
if [ -f "$ROOT/build/psp/app_classic/EBOOT.PBP" ]; then
    cp "$ROOT/build/psp/app_classic/EBOOT.PBP" "$GAME/NBCLASSIC/EBOOT.PBP"
else
    echo "no Classic EBOOT yet (make psp-classic)" >&2
fi
[ -f "$ROOT/build/psp/probe/EBOOT.PBP" ] && cp "$ROOT/build/psp/probe/EBOOT.PBP" "$GAME/NBPROBE/EBOOT.PBP"

# ---- NEWBLITZ: Modern's content staged as the desktop app's Resources, merged into the ROM by newblitz-kit ----
KIT_BIN="$ROOT/build/psp/host_modern/newblitz-kit"
NB="$GAME/NEWBLITZ"
if [ -x "$KIT_BIN" ]; then
    STAGE="$ROOT/build/psp/kit_stage"
    rm -rf "$STAGE"
    P="$STAGE/program"
    mkdir -p "$P/art" "$P/assets" "$P/packs" "$STAGE/app"
    cp -R "$ROOT/art/blitz2026/files" "$P/art/files"
    cp -R "$ROOT/art/blitz2026/logo27" "$P/art/logo27"
    for d in art/files art/vs_right uniforms announcer; do
        if [ -d "$APP_FOLDER/$d" ]; then mkdir -p "$P/$d" && cp -R "$APP_FOLDER/$d/." "$P/$d/";
        else echo "no $APP_FOLDER/$d: the kit goes without it (as make app-modern would)"; fi
    done
    mkdir -p "$P/art" && cp "$ROOT/art/credits/credits.wms" "$P/art/credits.wms"
    if [ -d "$ROOT/build/engine32/houston" ]; then cp -R "$ROOT/build/engine32/houston" "$P/houston";
    else echo "no build/engine32/houston: Houston wears the Titans' art (make houston)"; fi
    cp -R "$ROOT/assets/pad_glyphs" "$P/assets/pad_glyphs"
    [ -d "$ROOT/packs/blitz2026" ] && cp -R "$ROOT/packs/blitz2026" "$P/packs/blitz2026"
    "$KIT_BIN" --app "$STAGE/app" --program "$P" --rom "$ROM" --make-kit "$STAGE/out" > "$STAGE/kit.log" 2>&1 \
        || { tail -20 "$STAGE/kit.log" >&2; echo "newblitz-kit failed (log: $STAGE/kit.log)" >&2; exit 1; }
    grep -E '^\[ *[0-9.]+\] \[(kit|fs|uniform|e32|glyphs)\]' "$STAGE/kit.log" | sed 's/^\[ *[0-9.]*\] /  /'
    copy_if_changed "$STAGE/out/blitz.z64" "$NB/rom/blitz.z64"
    cp "$STAGE/out/blitz.z64.sha1" "$STAGE/out/kit.txt" "$STAGE/out/artfs.bin" "$NB/rom/"
    # What the EBOOT reads at run time besides the ROM: Houston's record, the Blitz 2026 pack (no images: they're in the ROM)
    rm -rf "$NB/houston" "$NB/packs"
    if [ -f "$P/houston/houston.json" ]; then mkdir -p "$NB/houston" && cp "$P/houston/houston.json" "$NB/houston/"; fi
    if [ -d "$P/packs/blitz2026" ]; then mkdir -p "$NB/packs" && cp -R "$P/packs/blitz2026" "$NB/packs/blitz2026" && rm -rf "$NB/packs/blitz2026/files"; fi
else
    echo "no $KIT_BIN: NEWBLITZ has no ROM (make psp-host FLAVOUR=modern)" >&2
fi
# The team's Season settings (Standard or Advanced, Lineups) from the desktop's app folder, as the stick's starting point (deploy.sh
# copies it only to a stick that has none, so what the PSP saved wins).
if [ -f "$APP_FOLDER/season.json" ] && [ "${1:-}" != "--ppsspp" ]; then cp "$APP_FOLDER/season.json" "$NB/season.json"; fi
# The bundled roster snapshot (§6): offline play has this week's lineups; a check on the hub replaces it.
ROSTERS="${ROSTERS:-}"
if [ -z "$ROSTERS" ]; then
    for d in "$APP_FOLDER/rosters" "$ROOT/build/blitz-rosters"; do
        [ -f "$d/players.json" ] || continue
        if [ -z "$ROSTERS" ] || [ "$(grep -o '"updated": *"[^"]*"' "$d/manifest.json")" \> "$(grep -o '"updated": *"[^"]*"' "$ROSTERS/manifest.json")" ]; then ROSTERS="$d"; fi
    done
fi
if [ -n "$ROSTERS" ]; then
    echo -n "  rosters from ${ROSTERS#$ROOT/}: "
    "$PY" -I "$ROOT/tools/psp/make_pack.py" --src "$ROSTERS" --snapshot "$NB/rosters"
else
    echo "no roster data (build/blitz-rosters or <app folder>/rosters): the pack's own players" >&2
fi
if [ -f "$ROOT/build/psp/app_modern/EBOOT.PBP" ]; then
    cp "$ROOT/build/psp/app_modern/EBOOT.PBP" "$NB/EBOOT.PBP"
else
    echo "no Modern EBOOT yet (make psp-modern)" >&2
fi
# ---- NBREMOTE: NB Remote, the Wi-Fi remote control (docs/PSP_PORT.md "NB Remote"); its VSH plugin goes in SEPLUGINS/, where
# deploy.sh copies it to the stick without enabling it (deploy.sh --plugin on adds the PLUGINS.TXT line) ----
mkdir -p "$GAME/NBREMOTE/data"
if [ -f "$ROOT/build/psp/nbremote/EBOOT.PBP" ]; then cp "$ROOT/build/psp/nbremote/EBOOT.PBP" "$GAME/NBREMOTE/EBOOT.PBP"
else echo "no NB Remote yet (make psp-remote)" >&2; fi
if [ "${1:-}" != "--ppsspp" ]; then
    mkdir -p "$OUT/SEPLUGINS"
    for prx in launch/nbremote.prx net/nbremote_net.prx; do
        [ -f "$ROOT/build/psp/nbremote_plugin/$prx" ] && cp "$ROOT/build/psp/nbremote_plugin/$prx" "$OUT/SEPLUGINS/"
    done
fi
# No net.cfg in a kit: deploy.sh writes it on the stick (removing it turns the network sink off). PPSSPP's memory stick keeps
# whatever a test put there (the mock Worker's).
if [ "${1:-}" != "--ppsspp" ]; then rm -f "$GAME"/*/data/net.cfg; fi

# BRANDING: kit README names the app
cat > "$OUT/README.txt" <<EOF
New Blitz for the PSP-1000, kit made $(date -u +%Y-%m-%dT%H:%MZ) from commit $(git -C "$ROOT" rev-parse --short=7 HEAD) (build $(git -C "$ROOT" rev-list --count HEAD)).
Internal use only: the ROMs in here are the team's own, for his PSP. Don't share this folder.

PSP/GAME/NEWBLITZ   New Blitz (Modern: 32 teams, the Blitz '27 art, this week's lineups)
PSP/GAME/NBCLASSIC  New Blitz Classic (the 1999 game)
PSP/GAME/NBPROBE    New Blitz Probe (checks the PSP and sends a report; reads NBCLASSIC's ROM)
PSP/GAME/NBREMOTE   New Blitz Remote (Wi-Fi remote control: the Mac runs, updates and restarts the games over the Worker)
SEPLUGINS/          nbremote.prx, NB Remote's ARK plugin (starts NB Remote from the XMB); not enabled until
                    make psp-deploy PLUGIN=on (nbremote_net.prx: an experimental variant, leave it off)

To put it on the Memory Stick: Settings > USB Connection on the PSP, then on the Mac:
    make psp-deploy MS=/Volumes/<stick name>
That copies these folders and writes each game's data/net.cfg (Wi-Fi logs and commands) from the Mac's build/ keys.
Then follow docs/PSP_PORT.md section 13 (the hardware-day checklist), one numbered step at a time.

Switch files (an empty file in a game's folder turns a test option on): logsync, nogfx, audio (sound on: costs CPU for now), noaudio, cpu222, romhash,
present ("30", "auto" or "20"), overlay ("off", "hud" or "log"), vsync, stackpool ("<slots> <KB>").
Logs: <game>/logs/blitz.log; a run that died leaves logs/pending/.
EOF
echo "kit: $GAME"
for g in NEWBLITZ NBCLASSIC NBPROBE NBREMOTE; do
    printf '  %-10s EBOOT %s' "$g" "$( [ -f "$GAME/$g/EBOOT.PBP" ] && echo yes || echo no)"
    [ -f "$GAME/$g/rom/blitz.z64" ] && printf ', rom %s MB + sidecar' "$(( $(stat -f %z "$GAME/$g/rom/blitz.z64") / 1048576 ))"
    [ -f "$GAME/$g/rosters/pack.txt" ] && printf ', rosters %s' "$(head -1 "$GAME/$g/rosters/pack.txt" | cut -d' ' -f2)"
    echo
done
