# RG DS bench over USB adb

Scripts for benchmarking New Blitz on the RG DS Plus (GammaOS, root adb). Copy this folder to `build/rgds/` (not tracked,
survives Mac restarts) and source `h.sh` from bash there; results go to `build/rgds/runs/`. Settings to restore after tests
live in `build/rgds_restore/` (the team's `general.json` and `blitz_ds.xml`).

- `bench3 <name> <secs>`: restart, nano launches the game itself (as the team does), menus to kickoff, field play pressing A,
  then `summ.py` (fps, shown per screen, GPU, memory, page faults, hitches), `smaps.py` and `top`.
- `nano_launch`: drives GammaOS's own menu with the pad (OK is South, back is East) and checks each step against nano's own
  screenshot (`setprop sys.gammaos.nano.shot 1`, `/data/local/tmp/nano_shot.ppm`) with `ppmdiff.py`. The references in
  `ref/` (splash, game, applications, newblitz) are nano shots of those screens, taken once by hand with `nshot`.
- `handoff`, `cc_off`, `bench2`: the older adb-launch path (`am start` after `service.bootanim.nano_retroarch=1`). It leaves
  nano's main service and RetroArch running and the overlay sometimes on top: use bench3 for numbers.
- Never `am start` Blitz while it's in `persist.gammaos.dualstack.pkgs`: that hung the whole device twice.

docs/POTATO_PERF.md has what was found with these.
