#!/bin/bash
# Regression suite for Modern on the original game (engine32) and what it shares with Classic.
#
#   tools/regress/engine32.sh record [CASE...]   # keep today's results as the baselines
#   tools/regress/engine32.sh check  [CASE...]   # compare against them (exit 1 if anything differs)
#
# Cases (all by default):
#   std, adv, league  the moved team tables, rosters and title boxes at team select: Standard, Advanced, a league pak
#   kickoff           the same tables at a kickoff (crew panel, Go Play), less the uniform tables: the game picks
#                     who's home at random, and the side's worn set goes into its team's away row
#   export            Export ROM from the dev data, byte for byte
#   cart              the cart season against the app's tiebreakers (tools/cart: 20 random seasons) and the cart code
#                     rebuilt from source matching what's committed (cart_season.inc, rom_picker_code.inc)
#   classic           Classic's tables through the team store (BLITZ_ENGINE_ROUNDTRIP=1) equal to the game's own
#
# Runs the dev builds (build/app_modern_dev, build/app_classic_dev) with tools/live.sh on the test data folders
# (build/hrtest_modern, build/hrtest_classic). Baselines and results live in build/regress: they hold game data,
# never git. Opens game windows: not while the team's app is running.
cd "$(dirname "$0")/../.."
MODE=$1; shift
[ "$MODE" = record ] || [ "$MODE" = check ] || { sed -n 2,19p "$0"; exit 2; }
CASES=${*:-std adv league kickoff export cart classic}
if pgrep -f "build/app_modern/Blitz|build/app/Blitz|BlitzRecomp.app" >/dev/null; then echo "the app is running: not now"; exit 2; fi
R=build/regress; BASE=$R/base; NEW=$R/new; mkdir -p $BASE $NEW
OUT=$NEW; [ "$MODE" = record ] && OUT=$BASE
FAIL=0
result() {  # NAME FILE: identical to the baseline?
    if [ "$MODE" = record ]; then echo "  $1: recorded"; return; fi
    if [ ! -f $BASE/$2 ]; then echo "  $1: no baseline (record first)"; FAIL=1
    elif cmp -s $BASE/$2 $NEW/$2; then echo "  $1: identical"
    else echo "  $1: DIFFERS ($BASE/$2, $NEW/$2)"; FAIL=1; fi
}
build() {
    if cmake --build $1 -j8 2>&1 | grep -q " error"; then echo "  build $1: FAILED"; exit 1; fi
}

# The moved tables (engine32_layout.h: 0x80720000-0x80722280), rosters (32 x 0x3C0) and title boxes (32 x 8).
TABLES="80720000:2280 80710000:7800 80718000:100"
KICKOFF="80720000:E80 80721880:A00 80710000:7800 80718000:100"  # the same, less uni_home and uni_away
modern() { export BLITZ_FLAVOUR=modern BLITZ_APP=build/app_modern_dev BLITZ_DATA=build/hrtest_modern; }
last_dump() { ls -t $BLITZ_DATA/dumps/rdram_*.bin | head -1; }
L=tools/live.sh
to_team_select() {  # ENV...: the first team picked, the CPU's list open
    $L stop >/dev/null 2>&1; $L start BLITZ_QUIET_NOTICES=1 BLITZ_VIRTUAL_PADS=2 BLITZ_OPP_START=5 "$@" >/dev/null
    $L wait 9; $L press START; $L wait 3; $L press START A; $L wait 3
    $L press A; $L wait 2.5; $L press A; $L wait 2.5; $L press "STICK -1,0:0.4" A; $L wait 3
}
dump() {  # FILE RANGES...
    local f=$1; shift
    $L dump >/dev/null; $L wait 2; python3 tools/regress/extract.py "$(last_dump)" $OUT/$f "$@"
}

for c in $CASES; do
    case $c in
    std|adv|league)
        modern; build build/app_modern_dev
        env="BLITZ_ADVANCED=0"  # Standard, whatever the default (Advanced since 2026-10-21)
        [ $c = adv ] && env="BLITZ_ADVANCED=1"
        if [ $c = league ]; then
            pak=$PWD/build/appdata2026/packs/pac12
            [ -d $pak ] || { echo "  league: no $pak, skipped"; continue; }
            env="BLITZ_PACK=$pak BLITZ_ADVANCED=0"
        fi
        to_team_select $env; dump $c.bin $TABLES; $L stop >/dev/null
        result "$c (team select)" $c.bin ;;
    kickoff)
        modern; build build/app_modern_dev
        to_team_select BLITZ_ADVANCED=0
        for i in 1 2 3 4; do grep -q "\[crew\] team select" build/live/app.log && break; $L press A; $L wait 3; done
        $L wait 1
        for k in DOWN DOWN DOWN DOWN DOWN RIGHT RIGHT RIGHT RIGHT; do $L press $k; $L wait 0.35; done
        $L press A; $L wait 15; dump kickoff.bin $KICKOFF; $L shot regress_kickoff >/dev/null; $L stop >/dev/null
        result "kickoff" kickoff.bin ;;
    export)
        modern; build build/app_modern_dev
        rm -rf $R/exp; mkdir -p $R/exp
        $L stop >/dev/null 2>&1; $L start BLITZ_QUIET_NOTICES=1 BLITZ_ADVANCED=0 BLITZ_EXPORT_DIR=$PWD/$R/exp >/dev/null
        $L wait 9; echo EXPORTROM >> build/live/input.txt; $L wait 10; $L stop >/dev/null
        if ls $R/exp/*.z64 >/dev/null 2>&1; then mv $R/exp/*.z64 $OUT/export.z64; result "export" export.z64
        else echo "  export: no ROM written"; FAIL=1; fi ;;
    cart)
        f=$OUT/export.z64; [ -f $f ] || f=$BASE/export.z64
        [ -f $f ] || { echo "  cart: needs an export (run the export case)"; FAIL=1; continue; }
        tools/cart/build.sh >/dev/null && tools/rom_picker/build.sh >/dev/null || { echo "  cart: build FAILED"; FAIL=1; continue; }
        if git diff --quiet -- src/game/cart_season.inc src/game/rom_picker_code.inc; then echo "  cart code: matches source"
        else echo "  cart code: rebuilt differently from what's committed (git diff src/game/*.inc)"; FAIL=1; fi
        cc -O1 -w -DCART_HOST_TEST -Isrc/cart tools/cart/host_test.c -o build/cart/host_test 2>/dev/null &&
            c++ -std=c++20 -O1 -w tools/cart/compare.cpp src/game/adv_league.cpp -o build/cart/compare 2>/dev/null || { FAIL=1; continue; }
        bad=0
        for s in $(seq 1 20); do
            build/cart/host_test $f $s | grep -E "^(div|seeds)" | tail -10 > $R/cart_host.txt
            build/cart/compare | grep -E "^(div|seeds)" > $R/cart_app.txt
            cmp -s $R/cart_host.txt $R/cart_app.txt || { bad=$((bad + 1)); echo "    season $s differs: diff $R/cart_host.txt $R/cart_app.txt"; }
        done
        [ $bad = 0 ] && echo "  cart season: 20 of 20 seasons as the app" || { echo "  cart season: $bad of 20 differ"; FAIL=1; } ;;
    classic)
        build build/app_classic_dev
        unset BLITZ_FLAVOUR; export BLITZ_APP=build/app_classic_dev BLITZ_DATA=build/hrtest_classic
        MAIN="8028AAB0:9B0 802836AC:B8 802AAFD8:7C"; OVLB="8033800C:C1C 80330BCC:7440 80338E68:1F0 8032D5E0:F8 8032D6D8:3C4"
        for rt in 0 1; do
            to_team_select BLITZ_ENGINE_ROUNDTRIP=$rt; dump classic_rt$rt.bin $MAIN $OVLB; $L stop >/dev/null
        done
        if cmp -s $OUT/classic_rt0.bin $OUT/classic_rt1.bin; then echo "  classic round trip: identical to the game's own"
        else echo "  classic round trip: DIFFERS ($OUT/classic_rt0.bin, $OUT/classic_rt1.bin)"; FAIL=1; fi
        result "classic (team select)" classic_rt0.bin ;;
    *) echo "  $c: no such case"; FAIL=1 ;;
    esac
done
[ "$MODE" = check ] && { [ $FAIL = 0 ] && echo "all identical" || echo "SOMETHING DIFFERS"; }
exit $FAIL
