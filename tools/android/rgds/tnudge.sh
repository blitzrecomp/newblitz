tnudge() {
  restore >/dev/null 2>&1; envfile
  fresh; prep ${1:-tnudge}; dark
  echo "-- at nano: $(D shell "dumpsys input | grep -m1 DispatchEnabled" | tr -d '\r' | xargs)"
  nano_launch || return 1
  sleep 15; echo "-- 15 s after launch: $(D shell "dumpsys input | grep -m1 DispatchEnabled" | tr -d '\r' | xargs), $(D shell "dumpsys activity processes | grep -m1 -o 'mBooted=[a-z]*'" | tr -d '\r'), pid $(D shell pidof $P | tr -d '\r')"
  D pull $F/blitz.log $RUN/blitz.log >/dev/null 2>&1; grep -a "\[input\]\|\[background\]\|android\] " $RUN/blitz.log | head
  D shell "magisk --sqlite 'SELECT * FROM policies'"
}
