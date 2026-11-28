# State of the project after 1.0.6

Internal (tools/public_exclude.txt leaves it out). Written 2026-11-28, while 1.0.6 was prepared and not yet shipped,
as the hand-off for a quiet stretch: "this will likely be our last release for a bit". Start here, then follow the
links. Anything marked "(unverified)" was taken from notes, not checked against the tree.

Reading order for a cold start: this file, then docs/RELEASE.md (how to ship), docs/LAUNCH.md (what happened each
release), docs/HUMAN_TESTS.md (what still needs a device), docs/design/SEASON_PAK.md (paks), CHANGELOG.md.

## What's shipped

- **1.0.0 (2026-11-15):** the first public release. One app per computer (Mac, Windows, Linux) that is also Blitz
  Builder: checks the player's ROM, sets the game up, sends it to a PS5 or Android device, saves an unsigned iPhone
  app, or exports a `.z64`. Original 1999 and New Blitz (with the 2026 Season pak). Soft launch to the public repo,
  site, Discord, videos.
- **1.0.1 (11-15):** on-screen keyboard Up/Down, Settings → General speed on the RG DS, PS5 DualSense reconnect loop.
- **1.0.2 (11-17):** frozen games can be picked up again (freeze rescue), Season pre-game screen, PS5 build tested on
  a PS5 (build 805, the pinned "stable" kit), Instant Replay on PS5.
- **1.0.3 (11-24):** quit crash (Windows, from 13 opt-in crash reports), phone menus after the app switcher, quitting
  saves the game and the next start offers it.
- **1.0.4 (11-26):** pink screens after the app switcher, false freeze message, Instant Replay shows the last play,
  Controller Pak guard, history and Settings → Controller Pak Backups. Root cause of the 11-26 pak wipe: a save
  state's stale libultra page-table cache (fixed). PS5 store zip `PPSA99961.zip`.
- **1.0.5 (11-27, build 942):** Cloud Sync (6-digit pairing, encrypted saves, Worker `newblitz-saves`), Sync over
  Wi-Fi, Export/Import saves, Surface Duo and wide two-screen layouts, Settings → DS, Play Art, jersey numbers, end
  boards, per-controller buttons, Settings re-tabbed. PS5 1.0.5 is pinned but untested on a PS5.
- **1.0.6 (prepared, not shipped):** see CHANGELOG.md's top section. Two league paks (CFL 2026, Pac-12 2026) and the
  redone NFL uniforms (a 2026 Season pak update that reaches every version); every posted pak loads by itself, Add-Ons
  Check, Open Folder on phones, Android picker exit; the USA v1.0 cartridge updated to v1.1 at import; freeze rescue
  between quarters; League selector (◀ ▶ look, × chooses, restart prompts); Lineups Current or All Starters; "Standard
  or Advanced season?"; Restart in place on phones; Play Art in Blitz style; START in one place on every controller;
  the Uniform Picker starts on Auto. Per-run status: docs/LAUNCH.md.

## How the pieces fit

- **The core:** one source tree (src/game, src/main, lib/*, platform/*-patches) compiled for every device through
  include/blitz_platform.h and include/blitz_host.h (docs/PLATFORMS.md). The game is the recompiled USA v1.1 cartridge
  (the app never ships it; v1.0 dumps are patched to v1.1 at import, 1.0.6). Patches to submodules are numbered files
  applied by tools/apply_patches.sh; after editing a patched tree, regenerate the patch (docs below, "Traps").
- **The app and Blitz Builder** (docs/design/ONE_APP.md): first run asks where you play, takes the ROM, offers the 2026
  Season, then sends the device what it needs. Device kits ship inside every computer's app (`devices/`): PS5 title +
  payload, Android APK (release-signed), unsigned iPhone IPA.
- **Season pak and league paks** (docs/design/SEASON_PAK.md): the public app carries no licensed content. The 2026
  Season pak (rosters, lineups, uniforms, art, announcer, Houston, Pro Bowl) and league paks (CFL 2026, Pac-12 2026)
  are `.nbpak` zips listed in a catalog (`blitz-paks/1`) served by the season-server Worker. 1.0.6: every catalog pak
  installs/updates itself at start under Settings → Teams → Keep Rosters and Uniforms Up to Date; Add-Ons shows each
  pak with Get/Remove and has Check. League paks play Arcade and Tournament only; League is chosen in Settings →
  Teams. Weekly roster files come from the blitz-rosters job.
- **Cloud Sync** (docs/SAVE_SYNC.md): saves encrypted on the device with a key from the pairing code; Worker +
  bucket `newblitz-saves`; never offered on a PS5. Sync over Wi-Fi and Export/Import are the offline routes.
- **Export ROM:** season carts (Lite/Full kinds were built on the parked lean-4mb branch, not in main) and, in 1.0.6,
  **league carts**: a league's 2-30 teams repeat through the cart's 31 slots (slot s = team s mod n; each team's "own"
  slot is its highest, 22-30, so its right VS helmet exists), team select steps over N teams, main menu blanks
  Season and Play Editor. A set's `cart_files` texture replaces a shoulder the cart can't draw (a cart runs the game's
  code, so `uv_overrides` can't come along). Checked in ares with the CFL and Pac-12 paks; never on a real cartridge.
- **Uniform previewer** (docs/UNIFORM_PREVIEW.md): `tools/uniform_preview.py` draws a set's PNGs through the real
  importer onto the game's own team-select player, offline; sheets plus a drag-around HTML page. Use it before
  looking in game.
- **Devices:**
  - Mac, Windows, Linux: the release downloads (dmg, Setup.exe, AppImage; Linux AppImage untested on Linux).
  - iPhone: sideload only (Sideloadly recommended); docs/IOS_SIDELOAD.md. The owner's phone upgrades in place with
    build/ios/sl_upgrade.sh.
  - Android: APK; RG DS and other two-screen handhelds (docs/BLITZ_DS.md, docs/POTATO_PERF.md); Surface Duo
    (docs/SURFACE_DUO.md); a Retroid handheld for remote tests (tools/android/remote.sh).
  - PS5: etaHEN homebrew via Blitz Builder or the store zip; docs/PS5_HOMEBREW.md, docs/PROSPEROSTORE.md.
  - PSP: runs on a PSP-1001 (ARK-5); docs/PSP_PORT.md. Not in public releases.
  - N64 / emulator: Export ROM.

## Release procedure in brief

docs/RELEASE.md is the full list. Short form: bump `BLITZ_VERSION`, write the CHANGELOG section, pin the PS5 kit
(`build/ps5_stable/current`), build the paks with `MIN_BUILD` = the release build, run `make release VERSION=x.y.z`
with the Android release key (the owner types the keystore password), run the scan, then publish each step only on the
owner's say: season-server catalog deploy, public tree push, GitHub release, update notice (blitz-rosters), Discord.
`make release` refuses to start without the Android key and without a matching PS5 stable kit.

## Parked branches and how to resume each

All local or on the private repo; nothing below is merged into 1.0.6. Check with `git branch --no-merged 1.0.6`.

- **autopilot** (9cb8d50, also on origin; docs/AUTOPILOT.md lives on the branch): CPU plays both sides, game after
  game. Passed on the Mac for 1.0.5 (HUD, game after game, stop/take over, records, capture). **Owes a LONG run** (an
  hour or more, back to back), a real controller's Start, a recording with sound, Android, and recording on the big
  display. Resume: merge the current line in, apply new patches by hand, then do the long run first.
- **model-replacement** (386706e, 45+ commits; parked 2026-11-13): HD ball, helmets, whole player, field, free camera
  (rt64 patches 0037-0047). Goal: "Madden-27 graphics in the Blitz 2000 engine" as a proof of concept. To revive,
  rebase onto the current line and renumber its rt64 patches (the Duo work took 0038-0045: start at 0046+), resolve
  src/game/config.cpp and src/main/dev_input.cpp. Its research outputs live in gitignored build/models_* folders; read
  docs/MODEL_REPLACEMENT.md and tools/models/README.md on the branch. Modelling work runs on Fable.
- **lean-4mb** (and relink-4mb, parked): the cart's Advanced season without an Expansion Pak (39.8 KB season, heap
  cut). Passed ares 4 MB and the owner's real N64 + SummerCart64. Before merging: checklist in docs/NO_EXPANSION_PAK.md
  (each kind in ares or on hardware, a fire test on Full/Arcade, picker, rosters, re-record `make regress`). Any heap
  or cart-contents change needs the fire stress test (Arizona at the Chargers, week 1, defense on fire). League carts
  in 1.0.6 are a separate path in main; whether they work without an Expansion Pak is (unverified).
- **windows-port** (hardware tests): PS5 runs both games (as of 2026-10-21); Windows Classic ran on a real PC (AMD,
  Vulkan) with a crash after a window resize (docs/WINDOWS_VULKAN.md "First run on a real PC"). The public Windows
  build has since had real users (1.0.3's crash reports), but the resize crash is (unverified) as to fixed. PS5: the
  console can't be reset to a test build any more (it went back to stock after 1.0.2), so every PS5 build after 805
  is untested; test steps in docs/PS5_HOMEBREW.md.
- **psp-port** (24887ce): runs on a PSP-1001 with NB Remote. Open: the triangle path (D-pad diagonals were fixed),
  ME audio (silent by default), mipmaps, speed (~25-28 ticks/s), Modern's heap is tight (3.1 of 3.5 MB peak). Start
  at docs/PSP_PORT.md §10/§14/§15.
- **blitz-ds / surface-duo:** merged in 1.0.5 (Surface Duo hinge layout, Settings → DS). What remains is in
  docs/SURFACE_DUO.md "Not known yet" (full-fidelity frame rate on two screens, controls with no controller, the
  launcher still locked to landscape) and docs/BLITZ_DS.md (RG DS probe findings, Mali upload race, memory budget).
  Next devices named: Thor, Retroid Pocket 5/6 (the owner's RP5 is away for weeks), cheap handhelds.
- **pak-compat** (777f868): the old-build compatibility harness (tools/tests/pak_compat: a local catalog, an
  old-build runner, drives, an ares script). Run it for every pak change; the final 2026 Season pak passed builds
  1.0.0-1.0.5 with it.
- **uniform-preview** (79f628e): superseded; its content is in 1.0.6 as 06e336f.
- **player-glyphs, skin-tones, smart-cpu** (older feature branches, per-player button icons, six skin shades, a
  smarter CPU): unmerged and unreviewed since the October pass; check `git log main..<branch>` before reviving
  (unverified whether any is still wanted).
- **rf-repin** (d5ca467): the RecompFrontend pin is upstream main (2ddbfc8) in the tree since 11-14; the branch is
  history (unverified that it was merged as a branch).
- **research/32-teams-no-teamplus:** research on a slot allocator (any franchise in any slot); not planned.
- **Follow-ups after the 1.0.6 launch:** the known issues below; PS5 1.0.6 build (untested); decide when the
  remaining parked branches should go back to main.

## Known issues and open follow-ups

- **Audio busy on reopen (Android):** reopening New Blitz too quickly after closing it can abort with the audio
  device still busy. Queued for after the 1.0.6 launch; cause not found. Waiting a moment before reopening avoids it.
- **League paks' low-detail models:** two of the six player models (the low-detail ones) have no cap table, so they
  keep the stock shoulders when a league set overrides them (cosmetic; which two models: `tools/uvo_check.py`).
- **Cart gaps (league and season carts):**
  - the attract demo's self-matchup on a league cart (a team can meet itself; unverified details);
  - the mirrored right VS helmet after a random pick: 18cb148 fixes it for league carts with team select over the
    league's teams (checked in ares, Pac-12); whether any other path still shows it is (unverified);
  - no announcer calls for a league's team names (the announcer files are the season pak's).
- **The City stadium's wide-top patch:** seen once on the City stadium's wide view; not reproduced since 1.0.5. If seen, save a mark (`MARK save`) and note the camera.
- **Pac-12 and CFL fidelity:** the art and rosters were produced by a design agent in another app; checked in game
  but not frame-by-frame. Known: the CFL's Saskatchewan alternate has stripe breaks on the pants (pants coordinates
  not done in the CFL pak), and the Pac-12 pak's final
  title is "Pac-12 2026" (it was "Blitz: Pac-12" in test builds and in SEASON_PAK.md's older examples). Rosters are
  snapshots (Pac-12: season start; CFL: preseason); they don't update weekly.
- **Tournament with a league pak** was never played to the end live (only the menus and an ares kickoff).
- **PS5 builds after 1.0.2 are untested** on a console; Blitz Builder says so ("Sends New Blitz 1.0.x (build N)").
- **Linux AppImage untested;** Intel Macs unsupported.
- **Cloud Sync's real-world use** has had one week; watch the `newblitz-saves` Worker's stats.

## Test recipes

- **Mac live tools** (private dev builds): `tools/live.sh start|stop|press|shot|CONFIG|MARK|…` drives a running game
  through the dev input file. Always `tools/live.sh stop` before `start` after a rebuild (start reuses an old
  instance). Cheat-set a stadium or weather with `CONFIG rules stadium N` (0 game decides, 1 Day, 2 Night, 3 Snow, 4 Old
  day…) instead of re-rolling games, and put it back afterward (CONFIG saves for real). `MARK save <name>` / `MARK
  load` give before/after comparisons. To reach Pick Your Crew: `press START`, `press A`, `press LEFT:0.6`, then `A`s;
  `KEY` arrows drive app UI. Never run builds or test windows over the owner's running app.
- **BLITZ_DS** (two-screen layouts on a computer): a two-window mode for DS and Controller layouts; see
  docs/BLITZ_DS.md and docs/TALL_VIEW.md. **BLITZ_SOFT_RESTART=1** exercises the phone's restart-in-place path on a
  computer. **BLITZ_HINGE=l,t,r,b** fakes a Duo's hinge.
- **Local pak catalog:** serve a folder holding `catalog.json` and the `.nbpak` files with `python3 -m http.server`
  and run the app with `BLITZ_PAK_SERVER=http://127.0.0.1:<port>/`. `BLITZ_PAK_INSTALL=<file>` acts as Add… (the
  player's own pak), `BLITZ_TEST_METERED=1` plus `BLITZ_TEST_METERED_LIMIT=<bytes>` simulate mobile data. On
  Android, put the same `BLITZ_*=value` lines in `<app folder>/test_env.txt` (read at launch). **Remove test_env.txt
  and stop the server afterwards**; the Duo's was pointed at a Mac on the LAN during 1.0.6 testing and may still be
  there (unverified).
- **adb over Wi-Fi:** after pairing once, a device that advertises itself (`adb mdns services`) connects by itself;
  don't also `adb connect` its address (two entries break plain commands). The Duo stops advertising after it sleeps:
  open Wireless debugging on it, read the IP:port, `adb connect <ip>:<port>` and use `-s`. A debug-signed dev APK
  won't install over a release-signed one (uninstall first, and ask). Remote tests from anywhere:
  `tools/android/remote.sh` (check `seen` first).
- **iPhone in-place upgrade:** a phone that got its app from Sideloadly has a different entitlement id, so
  tools/ios/install.sh is refused. Use `build/ios/sl_upgrade.sh path/to/NewBlitz.app` (unzip the release IPA first);
  it keeps the bundle id and data. Back up first. `tools/ios/adhoc.sh` + `publish.sh` make a private install link.
- **Old-build compatibility:** branch pak-compat's `tools/tests/pak_compat/` (setup.sh, make_catalog.py, run.sh,
  grid.py, drive.sh, ares.sh). Run a new pak against the oldest and latest public builds before posting it.
- **Exported carts:** run in ares (the owner's N64 + SummerCart64 is the hardware check). For the cart's heap, see
  the fire test above. `tools/rom_picker/build.sh` rebuilds the cart's added code; edit .asm, not the .inc.
- **Agents' tests:** no full games or tournaments; drive to the moment and take a shot (see "Rules").

## Traps worth knowing

- Editing a patched submodule (lib/RecompFrontend, rt64, N64ModernRuntime): `make app` applies patches first and
  stops with "FAILED: … does not apply" before compiling, so an old binary looks like "my change did nothing"; after
  editing, regenerate the patch against a clean tree and never strip trailing whitespace in patch files.
- A fresh release worktree needs the extras in docs/RELEASE.md ("Release in a fresh worktree").
- Agent worktrees start at an old commit: reset them to the current branch first.
- Roster/season-server secrets live in gitignored `build/` files (hub, tokens, install links); release_scan.sh reads
  them to fail a leak. In a worktree, point `BLITZ_SECRETS_DIR` at the main checkout's `build/`.

## People and process rules worth keeping

- **No full games in agent tests.** Drive to the screen, shoot, stop. The owner dislikes long test runs by agents.
- **Models for agents:** Sonnet by default (sweeps, builds, dry runs, docs, pak imports with a recipe, device
  installs); Opus only for hard engineering (engine/runtime, recompiled hooks, MIPS/cart patches); Fable only for
  modelling. Keep briefs tight and the agent count modest.
- **Site imagery rule:** on the public site, no legible real logos, wordmarks, logo'd helmets or player faces in
  images (UI shots and wide gameplay are fine); league and team names as plain facts in copy are fine; headings avoid
  "NFL" beyond the factual "NFL Blitz 2000". Social posts may show gameplay with logos, but keep the site to 2 such
  images and social to about 3.
- **Name scrub:** keep the owner's full name, personal addresses and the old developer handle out of every public
  artifact; `build/public_deny.txt` is the deny list and `tools/release_scan.sh` enforces it. Never echo the list.
  Public names are "New Blitz" and "Blitz Recomp".
- **Publishing** (catalog deploy, repo push, GitHub release, Discord) each wait for the owner's OK; a build that needs
  the keystore password is the owner's to run. Don't push or merge unless asked; local commits are fine.
- **Device installs cost the owner minutes:** bundle fixes per install and make unknowns Settings switches. Ask
  before touching a PS5; never leave test settings (CONFIG changes, test_env.txt) behind on a device.
- **The old Cloudflare account** still hosts one unrelated Worker that must stay where it is; everything for New Blitz
  is on the Blitz Recomp account (the old addresses are forwarders).
