# Lid close / open on the RG DS (gpio-keys SW_LID on event6), mid-game.
LIDDEV=/dev/input/event6
lid() { D shell "sendevent $LIDDEV 5 0 $1; sendevent $LIDDEV 0 0 0"; }  # 1 closed, 0 open
state() {
  echo "-- $1: pid $(D shell "pidof $P" | tr -d '\r'), $(D shell "dumpsys input | grep -m1 DispatchEnabled" | tr -d '\r' | xargs), nano $(D shell getprop init.svc.gammaos-nano | tr -d '\r'), overlay $(D shell getprop init.svc.gammaos-nano-overlay | tr -d '\r'), awake $(D shell "dumpsys power | grep -m1 mWakefulness=" | tr -d '\r' | xargs)"
}
tlid_setup() {
  envfile "BLITZ_INPUT_LIVE=$F/remote_input.txt BLITZ_VIRTUAL_PADS=1 BLITZ_ROOT_INPUT=0"
  fresh; prep ${1:-tlid}; dark
  nano_launch || return 1
  sleep 3; D shell "service call window 15 i32 1" >/dev/null; echo "-- input: $(D shell "dumpsys input | grep -m1 DispatchEnabled" | tr -d '\r' | xargs)"
  sleep 32
  to_field_ds; lg "kickoff" 1
  sleep 8
}
tlid_cycle() {  # tlid_cycle <closed seconds> <name>
  state "before $2"
  local n0=$(D shell "grep -c . $F/blitz.log" | tr -d '\r')
  D shell "logcat -c"
  lid 1; sleep $1; state "closed $2"
  lid 0; sleep 10; dark; state "opened $2"
  scap $2 >/dev/null
  D shell "tail -n +$n0 $F/blitz.log | grep -aE 'savestate|\[ds\]|second screen|surface|pause|resume|status|stall|crash|quit|replay\] ring'" | cut -c1-170 | head -30
  D shell "logcat -d -v time -b all | grep -vE 'avc:|audit|RTW|cw2015|allocator' | grep -iE 'GammaOSNano|nano|Killing|blitzrecomp|lid|PowerManager|goToSleep|wakeUp|InputDispatcher' | head -25" | cut -c1-190
}
tlid_ingame() {  # from the attract: into a game, then wait until a play is live (the field on top), then the lid
  to_field_ds; lg "kickoff" 1
  local i; for i in $(seq 1 40); do D shell "grep -a 'second screen: ' $F/blitz.log | tail -1" | grep -q "the picture" && break; sleep 2; done
  sleep 3; D shell "grep -a 'second screen: ' $F/blitz.log | tail -1"
  tlid_cycle ${1:-15} ${2:-lid_play}
  sleep 4; D shell "grep -a 'savestate\]\|Auto slot\|status\]' $F/blitz.log | tail -5"; D shell "ls -l $F/savestates/"
}
tlid_dispatch() {  # fresh nano launch, no sleep/wake: is input off, and does a lid close/open bring it back?
  fresh; prep ${1:-tlid_d}; dark
  nano_launch || return 1
  sleep 15; state "after launch"
  lid 1; sleep 4; lid 0; sleep 8; dark; state "after lid"
}
