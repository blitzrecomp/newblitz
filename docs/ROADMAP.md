# Roadmap

Updated 2026-10-23. The project stays internal. "Production" means a build the team can hand to someone: it installs,
survives a full season, updates, and leaves a log that explains any crash. docs/PLATFORMS.md explains the shared core;
docs/PROVENANCE.md covers what may be shipped.

## Where each platform stands

| Platform | Builds | Played on real hardware | Biggest open item |
|---|---|---|---|
| macOS | ✅ | ✅ daily | Stutter / pacing numbers (the Frame Counter now exists) |
| iPhone | ✅ | ✅ a full Modern season (controller) | Touch play and save states untried; stutter |
| Android | ✅ | emulator only (no game graphics there) | First real device: the Retroid Pocket Duo |
| PS5 | ✅ | ✅ both games boot, play, save; save states and Pick Your Crew work | No network: rosters and faces only from the bundled snapshot |
| Windows | ✅ | ✅ Classic through play | Crash after a window resize |
| Linux | ✅ (cross-built) | never run | A first run on a Steam Deck or Linux PC |

Every platform now builds save states and Redo Play, the Frame Counter and the rosters hub; phones get the shared
touch controls. `make all-platforms` builds all 12 targets, and the last run passed all 12.

## One app: New Blitz (2026-10-25)

Classic is now **Original 1999**, a mode of the one app (Settings → Season → Game): the game's own 31 teams, art and
Season on Modern's engine. **Blitz Builder** runs at the first start (Which Blitz? → Recommended or Customize →
Here, or N64/Emulator) and from the launcher afterwards. `make all-platforms` builds New Blitz only (`CLASSIC=1` adds
the old apps; tools/regress keeps its own reference builds). Next for Builder, in order:
1. ~~**Send to a device**~~ done 2026-10-28: Builder → Somewhere else → PS5 (FTP + loader), iPhone (devicectl) or
   Android (adb), with the ROM and the Season and Rules settings (src/main/device_send.cpp; kits from
   `make device-kits`). Tested: a stand-in PS5 (every file, the payload), the Android emulator (installed, ROM
   imported). Not yet: a real PS5 or iPhone from Builder; Windows (no shell commands yet; the PS5 works there).
2. **Builder as the hub:** rosters, crews and new versions pushed to every device it knows.
3. **The menus audit** (team: easier to navigate), and the Original mode hiding 2026-only settings.

## Next, in order

1. **Prove the phones by touch** (iPhone now, Android when the Retroid arrives). Play a full game by touch with Save,
   Load and Redo, then tune the buttons' size and position from the team's hands. *First because* it decides whether phones
   are a real way to play or a controller-only port.
2. **Stutter, with numbers.** Turn on the Frame Counter on the iPhone and the Mac and compare the worst gaps. Then try
   a 60 Hz cap on ProMotion phones (IOS_FEASIBILITY.md "Next pass: stutter"). The same work carries over to Android.
3. **Android on the Retroid.** Graphics through a full match, audio, the built-in pad, 4:3 and 16:9. Apply the known
   Adreno fallbacks only if something draws wrong (ANDROID.md).
4. **Crews across devices.** Pick an option and build it:
   - lock in on the Mac and let crews ride the hub (the smallest step);
   - two-way sync;
   - a QR code that opens the Roster Room from a phone.
5. **The Windows resize crash.** Reproduce it with the crash logger's minidump and fix it. Then add mouse in the menus
   and arrow keys at play select.
6. **First Linux run** on a Steam Deck or any Linux PC, then package it (AppImage) if it works.
7. ~~**"Send logs"**~~ done 2026-10-23: Settings → General → Save Logs zips the logs, crash reports, debug info and
   settings into Downloads (a phone's Files folder, the PS5's /data/blitzrecomp). Tested on the Mac.
8. **Rosters that update themselves on every device** (team, 2026-10-23). Each device checks at launch and every
   3 hours (by the wall clock, so a phone that slept for days catches up), with no button to press.
   - Done 2026-10-23: the periodic check; a build can carry a read-only token (`build/rosters_token`) or a hub
     address to every device it goes to; the PS5 links its network libraries the way the SDK's http2 sample does
     (its log had said `[http] no sceNetInit`; untested on the console since).
   - **The team's call:** create the fine-grained read-only token for `blitz-rosters` (ROSTER_UPDATES.md "Access"). Until
     then nothing downloads, not even the packaged Mac app; every device plays the snapshot its build carries
     (players, lineups and 2,550 headshots).
9. **Updates that reach every device.** The hub's "Update available" notice carries a link per platform (zip, APK,
   PS5 folder), and the desktop app can serve its own rosters as a hub (opt-in), with no script needed.

## Later

- **The in-game menu (Settings) for touch**: its controls are too small for fingers on a phone (team, 2026-10-22).
- **The Retroid's second screen**: stats or a playbook (ANDROID_FEASIBILITY.md §7).
- **Season saves and settings that follow you** between devices through the hub.
- **Before any build leaves the team's hands**: a NOTICES file, a credits page listing the libraries, and a source offer
  (PROVENANCE.md).
- **Signing**: notarized Mac zips, a release key for Android, a signed Windows installer.
