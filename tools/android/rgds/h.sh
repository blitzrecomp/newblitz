# RG DS bench helpers over USB adb (root shell). Source it from bash: . ./h.sh
export PATH=/opt/homebrew/bin:$PATH
SER=b810ac42991369f4
RG=<repo>/build/rgds
P=com.blitzrecomp.newblitz
F=/sdcard/Android/data/$P/files
D() { timeout ${T:-40} adb -s $SER "$@"; }

# cfg two(0|1) sharp copy waits: the team's general.json (build/rgds_restore) with test changes
cfg() {
  local two=${1:-0} sharp=${2:-S150} copy=${3:-WhenNeeded} waits=${4:-Full}
  python3 - "$RG/../rgds_restore/general.json" "$RG/general.run.json" "$two" "$sharp" "$copy" "$waits" <<'PY'
import json,sys
g=json.load(open(sys.argv[1]))
g["potato_two_screens"]=sys.argv[3]=="1"; g["potato_two_sharpness"]=sys.argv[4]; g["potato_frame_copy"]=sys.argv[5]; g["potato_waits"]=sys.argv[6]
g["fps_counter"]=True; g["send_logs"]=False
json.dump(g,open(sys.argv[2],"w"),indent=4)
PY
  D shell am force-stop $P
  D push "$RG/general.run.json" $F/general.json >/dev/null
  local tw=false; [ "$two" = 1 ] && tw=true
  sed "s/name=\"potato_two_screens\" value=\"[a-z]*\"/name=\"potato_two_screens\" value=\"$tw\"/" $RG/../rgds_restore/blitz_ds.xml > $RG/blitz_ds.run.xml
  D push $RG/blitz_ds.run.xml /data/data/$P/shared_prefs/blitz_ds.xml >/dev/null
  D shell "chown \$(stat -c %U /data/data/$P):ext_data_rw $F/general.json; chown \$(stat -c %U:%G /data/data/$P) /data/data/$P/shared_prefs/blitz_ds.xml; chmod 660 /data/data/$P/shared_prefs/blitz_ds.xml $F/general.json"
}
launch() {
  RUN=$RG/runs/$1; mkdir -p $RUN
  D push $RG/sampler.sh /data/local/tmp/sampler.sh >/dev/null
  D shell "am force-stop $P; touch /data/local/tmp/sampler.stop; sleep 2; chmod 755 /data/local/tmp/sampler.sh; : > $F/remote_input.txt; chown \$(stat -c %U /data/data/$P):ext_data_rw $F/remote_input.txt; chmod 666 $F/remote_input.txt; logcat -c"
  D shell "nohup /data/local/tmp/sampler.sh /data/local/tmp/sampler.csv > /dev/null 2>&1 &"
  sleep 1
  D shell "am start --display ${DISP:-0} -n $P/com.blitzrecomp.newblitz.BlitzActivity --es blitz_env 'BLITZ_INPUT_LIVE=$F/remote_input.txt;BLITZ_VIRTUAL_PADS=1'" >/dev/null
  date +%s > $RUN/start
}
press() {
  local first=1 what hold
  while [ $# -gt 0 ]; do
    what=$1; shift; hold=0.15
    if [[ "$what" == *:* && "${what##*:}" =~ ^[0-9.]+$ ]]; then hold=${what##*:}; what=${what%:*}; fi
    [ $first = 1 ] || sleep 0.4; first=0
    D shell "echo '$what $hold' >> $F/remote_input.txt"
    sleep $hold
  done
}
live() { D shell "echo '$*' >> $F/remote_input.txt"; }
collect() {
  D shell "touch /data/local/tmp/sampler.stop"; sleep 3
  D pull /data/local/tmp/sampler.csv $RUN/sampler.csv >/dev/null 2>&1
  D pull $F/blitz.log $RUN/blitz.log >/dev/null 2>&1
  D shell "am force-stop $P"
}
gshot() {
  D shell "am broadcast -a $P.REMOTE_SHOT -p $P --es name $(basename $RUN)_$1" >/dev/null
  <repo>/tools/android/remote.sh res $(basename $RUN)_$1 $RUN/$1.jpg >/dev/null 2>&1 && echo "$RUN/$1.jpg" || echo "gshot $1: no picture"
}
envfile() {
  if [ -n "$1" ]; then printf '%s\n' $1 > $RG/test_env.txt; D push $RG/test_env.txt $F/test_env.txt >/dev/null; D shell "chown \$(stat -c %U /data/data/$P):ext_data_rw $F/test_env.txt; chmod 660 $F/test_env.txt"
  else D shell "rm -f $F/test_env.txt"; fi
}
fresh() {
  D reboot; sleep 50
  for i in $(seq 1 60); do
    st=$(timeout 5 adb -s $SER get-state 2>&1)
    if [[ "$st" == *offline* ]]; then adb reconnect offline >/dev/null 2>&1; sleep 5; fi
    if [[ "$st" != device ]] && [ $((i % 6)) = 0 ]; then adb kill-server; sleep 2; adb start-server >/dev/null 2>&1; sleep 3; fi
    [ "$(T=5 D shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ] && break; sleep 3
  done
  sleep ${SETTLE:-60}
  [ "$(timeout 5 adb -s $SER get-state 2>&1)" = device ] || { adb kill-server; sleep 2; adb start-server >/dev/null 2>&1; sleep 4; }
}
dark() { D shell "touch /data/local/tmp/dark; echo 25 > /sys/class/backlight/backlight/brightness; echo 25 > /sys/class/backlight/backlight1/brightness"; }  # the team: dim (about 10%), not off, so he can see work going on
light() { D shell "rm -f /data/local/tmp/dark; echo 1 > /sys/class/backlight/backlight/brightness; echo 1 > /sys/class/backlight/backlight1/brightness"; }
# restore: the team's settings back, no test file, no quit save, lights at 1
restore() {
  D shell "am force-stop $P; touch /data/local/tmp/sampler.stop; rm -f $F/test_env.txt $F/savestates/quit.state $F/savestates/quit.txt"
  D push $RG/../rgds_restore/general.json $F/general.json >/dev/null; D push $RG/../rgds_restore/blitz_ds.xml /data/data/$P/shared_prefs/blitz_ds.xml >/dev/null
  D shell "chown \$(stat -c %U /data/data/$P):ext_data_rw $F/general.json; chown \$(stat -c %U:%G /data/data/$P) /data/data/$P/shared_prefs/blitz_ds.xml; chmod 660 /data/data/$P/shared_prefs/blitz_ds.xml $F/general.json"
  light
}
# btn NAME[:hold] ...: the built-in pad itself (root sendevent). S E N W L1 R1 L2 R2 SELECT START MODE UP DOWN LEFT RIGHT
PADEV=${PADEV:-/dev/input/event8}
btn() {
  local first=1 what hold code ax val
  while [ $# -gt 0 ]; do
    what=$1; shift; hold=0.12
    if [[ "$what" == *:* ]]; then hold=${what##*:}; what=${what%:*}; fi
    [ $first = 1 ] || sleep 0.35; first=0
    case $what in
      S) code=304;; E) code=305;; N) code=307;; W) code=308;; L1) code=310;; R1) code=311;; L2) code=312;; R2) code=313;;
      SELECT) code=314;; START) code=315;; MODE) code=316;;
      UP) ax=17; val=-1;; DOWN) ax=17; val=1;; LEFT) ax=16; val=-1;; RIGHT) ax=16; val=1;;
      *) echo "btn: unknown $what"; continue;;
    esac
    if [ -n "$ax" ]; then
      D shell "sendevent $PADEV 3 $ax $val; sendevent $PADEV 0 0 0; sleep $hold; sendevent $PADEV 3 $ax 0; sendevent $PADEV 0 0 0"
    else
      D shell "sendevent $PADEV 1 $code 1; sendevent $PADEV 0 0 0; sleep $hold; sendevent $PADEV 1 $code 0; sendevent $PADEV 0 0 0"
    fi
    ax=
  done
}
nano_off() { D shell "stop gammaos-nano"; sleep 2; }
nano_on() { D shell "start gammaos-nano"; }
# tap X Y: a real finger on the lower panel (gt9xx, 1024x768), as root
TOUCHDEV=/dev/input/event1
tap() { D shell "sendevent $TOUCHDEV 3 57 0; sendevent $TOUCHDEV 3 53 $1; sendevent $TOUCHDEV 3 54 $2; sendevent $TOUCHDEV 3 48 10; sendevent $TOUCHDEV 3 50 10; sendevent $TOUCHDEV 1 330 1; sendevent $TOUCHDEV 0 0 0; sleep 0.08; sendevent $TOUCHDEV 3 57 4294967295; sendevent $TOUCHDEV 1 330 0; sendevent $TOUCHDEV 0 0 0"; }
# to_field: title -> Arcade -> crew screen (GO PLAY by the virtual pad's cursor) -> kickoff
to_field() {
  press START; sleep 4; press START; sleep 4; press A; sleep 4; press A; sleep 4
  press LEFT A; sleep 6; press A; sleep 6; press A; sleep 10
  # Crew screen: a tap on GO PLAY (real input, after handoff), then the virtual pad's cursor as a fallback
  tap 860 660; sleep 8
  if D shell "grep -a '\\[crew\\]' $F/blitz.log | tail -1 | grep -q 'team select'"; then
    press P2:DOWN P2:DOWN P2:DOWN P2:DOWN P2:DOWN; sleep 0.5; press P2:RIGHT P2:RIGHT P2:RIGHT P2:RIGHT P2:RIGHT; sleep 0.5; press P2:A; sleep 8
  fi
  sleep 8
}
# bench2 <name> <field seconds>: restart, nano off, screens dark, menus to the field, A every 3 s, then collect and sum up.
# Settings from cfg/envfile beforehand (they're files: a restart keeps them).
bench2() {
  local name=$1 secs=${2:-150}
  fresh; handoff; dark
  launch $name; sleep 40
  [ "${CC:-off}" = off ] && cc_off
  to_field; gshot field_start >/dev/null; scap field_start >/dev/null
  D shell "grep -c . $F/blitz.log" | tr -d '\r' > $RUN/field_from_line
  local t0=$(date +%s) n=0
  while [ $(( $(date +%s) - t0 )) -lt $secs ]; do
    D shell "pidof $P" >/dev/null || { echo "GAME GONE at $(( $(date +%s) - t0 )) s"; break; }
    press A; sleep 2.6
    if [ $(( ($(date +%s) - t0) / 50 )) -gt $n ]; then n=$(( ($(date +%s) - t0) / 50 )); gshot g_$((n*50))s >/dev/null; fi
  done
  smaps field > $RUN/smaps_summary.txt 2>&1
  collect
  echo "== $name"; python3 $RG/summ.py $RUN
}
# nshot <name>: nano's own screenshot of its menu (it draws outside Android, so screencap misses it), flipped the right way
nshot() { D shell "rm -f /data/local/tmp/nano_shot.ppm; setprop sys.gammaos.nano.shot 1"; sleep 3; D pull /data/local/tmp/nano_shot.ppm $RG/runs/nano_$1.ppm >/dev/null 2>&1 && sips -f horizontal -s format png $RG/runs/nano_$1.ppm --out $RG/runs/nano_$1.png >/dev/null 2>&1 && echo $RG/runs/nano_$1.png; }
# handoff: what nano does when it launches an app, without nano. system_server waits at boot for
# service.bootanim.nano_retroarch=1, then enables the screen: input dispatch comes on, nano (run as the boot animation)
# exits, SurfaceFlinger owns the panels. Stopping the overlay takes GammaOS's Control Center off the lower screen.
handoff() {
  # The property also starts RetroArch (system_server's "user selected RetroArch"). It stays, under the game: stopping it makes
  # the overlay put nano's home or Quick Menu back over the game. The launch props make the overlay treat the game as running.
  D shell "setprop service.bootanim.nano_retroarch 1"; sleep 6
  D shell "setprop sys.gammaos.nano.launched_pkg $P; setprop sys.gammaos.nano.app_launched 1; setprop sys.gammaos.nano.menu_active 0; setprop sys.gammaos.nano.drm_active 0"
}
# cc_off: after the game is up, GammaOS's Control Center off the lower screen (what DualStack or the setting off would give)
cc_off() { D shell "stop gammaos-nano-overlay"; sleep 3; }
# scap <name>: what's on the glass now (screencap works once SurfaceFlinger owns the panels): <name>_bottom/_top.png
scap() { D shell "screencap -d 0 -p /sdcard/s0.png; screencap -d 1 -p /sdcard/s1.png"; D pull /sdcard/s0.png $RUN/$1_bottom.png >/dev/null; D pull /sdcard/s1.png $RUN/$1_top.png >/dev/null; sips -Z 512 $RUN/$1_bottom.png >/dev/null; sips -Z 512 $RUN/$1_top.png >/dev/null; echo $RUN/$1_top.png; }
# side: the side button (adc-keys KEY_BACK), GammaOS's Quick Menu toggle
side() { D shell "sendevent /dev/input/event5 1 158 1; sendevent /dev/input/event5 0 0 0; sleep 0.1; sendevent /dev/input/event5 1 158 0; sendevent /dev/input/event5 0 0 0"; }
# smaps <name>: the running game's memory by mapping (build/rgds/smaps.py)
smaps() { D shell "cat /proc/\$(pidof $P)/smaps" > $RUN/smaps_$1.txt; python3 $RG/smaps.py $RUN/smaps_$1.txt; }

# --- Real launches: nano starts the game itself (as the team does), so DualStack, nano's exit and no RetroArch are all as in use.
# The built-in pad is a virtual Xbox pad: nano's OK ("A") is South (btn S). From a fresh boot nano sits on Game;
# Game > Applications > New Blitz is S, RIGHT, S, RIGHT x8. nshot shows nano's own top screen to check.
prep() {  # what launch does before am start: logs, sampler, live input file
  RUN=$RG/runs/$1; mkdir -p $RUN
  D push $RG/sampler.sh /data/local/tmp/sampler.sh >/dev/null
  D shell "touch /data/local/tmp/sampler.stop; sleep 2; chmod 755 /data/local/tmp/sampler.sh; : > $F/remote_input.txt; chown \$(stat -c %U /data/data/$P):ext_data_rw $F/remote_input.txt; chmod 666 $F/remote_input.txt; logcat -c"
  D shell "nohup /data/local/tmp/sampler.sh /data/local/tmp/sampler.csv > /dev/null 2>&1 &"
  date +%s > $RUN/start
}
nano_is() {  # nano_is <ref name>: nano's screen now matches build/rgds/ref/<name>.ppm (closest different screens: 3.4)
  local k d
  for k in 1 2; do
    D shell "rm -f /data/local/tmp/nano_shot.ppm; setprop sys.gammaos.nano.shot 1"; sleep 2
    D pull /data/local/tmp/nano_shot.ppm $RG/runs/nano_now.ppm >/dev/null 2>&1 || return 1
    d=$(python3 $RG/ppmdiff.py $RG/ref/$1.ppm $RG/runs/nano_now.ppm)
    echo "$(date +%T) $1 $d" >> $RG/runs/nano_nav.log
    python3 -c "import sys; sys.exit(0 if $d < 2.8 else 1)" && return 0
    python3 -c "import sys; sys.exit(0 if $d < 4.5 else 1)" || return 1   # clearly another screen
    sleep 1.5                                                              # maybe mid-slide: look again
  done
  return 1
}
nano_launch() {
  # nano keeps its place at every level across launches and restarts, so: out to the top, then in by sight.
  # OK is South (btn S), back is East (btn E). References in build/rgds/ref.
  local i
  for i in 1 2 3; do nano_is splash || break; btn S; sleep 3; done   # after a restart it waits on its logo for A
  if ! nano_is newblitz; then
    for i in 1 2 3 4; do btn E; sleep 1.2; done                         # back out to the top row
    for i in 1 2 3 4 5 6 7; do nano_is game && break; btn LEFT; sleep 1.2; done
    nano_is game || { echo "NANO: no Game category"; return 1; }
    btn S; sleep 2.5                                                    # into Game, wherever it was last
    for i in $(seq 1 30); do nano_is applications && break; btn LEFT; sleep 1; done   # its list wraps: search
    nano_is applications || { echo "NANO: no Applications"; return 1; }
    btn S; sleep 2.5
    for i in $(seq 1 18); do nano_is newblitz && break; btn RIGHT; sleep 1; done
    nano_is newblitz || for i in $(seq 1 30); do nano_is newblitz && break; btn LEFT; sleep 1; done
  fi
  nano_is newblitz || { echo "NANO: New Blitz not found"; return 1; }
  btn S
  for i in $(seq 1 25); do D shell "pidof $P" >/dev/null && break; sleep 1; done
  D shell "logcat -d -s GammaOSNano | grep -q 'launching app $P'" && echo "nano launched it" || echo "NANO LAUNCH FAILED"
}
# to_field_ds: title -> Arcade -> crew (the pad's START, as the team does) -> kickoff
to_field_ds() {
  # Injected pad keys don't reach a nano-launched game (Android gets them, the game doesn't; the team's real presses do), so the
  # crew screen goes by the virtual pad (BLITZ_VIRTUAL_PADS=1 in test_env.txt): its cursor to GO PLAY.
  press START; sleep 4; press START; sleep 4; press A; sleep 4; press A; sleep 4
  press LEFT A; sleep 6; press A; sleep 6; press A; sleep 14
  local k; for k in 1 2 3; do
    press P2:DOWN P2:DOWN P2:DOWN P2:DOWN P2:DOWN; sleep 0.5; press P2:RIGHT P2:RIGHT P2:RIGHT P2:RIGHT P2:RIGHT; sleep 0.5; press P2:A; sleep 10
    D shell "grep -aq '\\[crew\\] kickoff' $F/blitz.log" && break
  done
  sleep 6
}
# bench3 <name> <secs>: restart, nano launches the game (DualStack if it's on the list), field play, sum up
bench3() {
  local name=$1 secs=${2:-150}
  fresh; prep $name; dark
  nano_launch || { echo "== $name: no launch"; return 1; }
  D shell "pidof $P" >/dev/null || { echo "== $name: no game"; return 1; }
  sleep 35
  to_field_ds; scap field_start >/dev/null
  D shell "grep -c . $F/blitz.log" | tr -d '\r' > $RUN/field_from_line
  local t0=$(date +%s)
  while [ $(( $(date +%s) - t0 )) -lt $secs ]; do
    D shell "pidof $P" >/dev/null || { echo "GAME GONE at $(( $(date +%s) - t0 )) s"; break; }
    press A; sleep 2.6
  done
  smaps field > $RUN/smaps_summary.txt 2>&1
  D shell "top -b -n 1 -m 12" > $RUN/top.txt 2>&1
  collect
  echo "== $name"; python3 $RG/summ.py $RUN
}
