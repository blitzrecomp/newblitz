# Junk-label A/B through nano itself (the kick build: no sleep/wake), upper screen's bottom 96 rows as raw RGBA
boot_state() { D shell "dumpsys activity processes | grep -E 'mBooted=' | head -1; dumpsys input | grep -m1 DispatchEnabled" | tr -d '\r' | xargs; }
ab3_arm() {  # ab3_arm <name> <serial 0|1> <strips>
  local name=$1 v=$2 n=${3:-150}
  python3 - "$RG/../rgds_restore/general.json" "$RG/general.run.json" <<'PY'
import json,sys
g=json.load(open(sys.argv[1])); g["ds_screens"]="Companion"; g["fps_counter"]=False; g["send_logs"]=False
json.dump(g,open(sys.argv[2],"w"),indent=4)
PY
  D shell am force-stop $P
  D push "$RG/general.run.json" $F/general.json >/dev/null
  D shell "chown \$(stat -c %U /data/data/$P):ext_data_rw $F/general.json; chmod 660 $F/general.json"
  envfile "BLITZ_UPLOAD_SERIAL=$v BLITZ_ROOT_INPUT=0"
  fresh; prep $name; dark
  echo "== $name at nano's menu: $(boot_state)"
  nano_launch || return 1
  sleep 20; echo "== $name 20 s after launch: $(boot_state), pid $(D shell pidof $P | tr -d '\r')"
  D shell "grep -a 'input\] \|uploads serial\|uploads on a' $F/blitz.log | head -5"
  sleep 25
  mkdir -p $RUN/strips
  local i; for i in $(seq -w 1 $n); do
    timeout 20 adb -s $SER exec-out "screencap -d 1 | tail -c 393216" > $RUN/strips/s$i.raw 2>/dev/null
  done
  echo "== $name done: pid $(D shell pidof $P | tr -d '\r')"
  collect >/dev/null 2>&1
}

# Both Screens (the team's), the lower screen's bottom strip (display 0), where the demo's label bar is in a two-screen play
ab5_arm() {  # ab5_arm <name> <serial 0|1> <strips>
  local name=$1 v=$2 n=${3:-200}
  restore >/dev/null 2>&1
  envfile "BLITZ_UPLOAD_SERIAL=$v BLITZ_ROOT_INPUT=0"
  fresh; prep $name; dark
  nano_launch || return 1
  sleep 45
  mkdir -p $RUN/strips
  local i; for i in $(seq -w 1 $n); do
    timeout 20 adb -s $SER exec-out "screencap -d 0 | tail -c 393216" > $RUN/strips/s$i.raw 2>/dev/null
  done
  echo "== $name done: pid $(D shell pidof $P | tr -d '\r')"
  collect >/dev/null 2>&1
}
