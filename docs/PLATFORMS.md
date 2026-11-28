# One core, every device

How BlitzRecomp keeps a single game core and gets each change to every platform: Mac, Windows, Linux, iPhone,
Android and PS5. Also covers how rosters and app updates fan out from one hub, with each device pruning what it
doesn't take. Written 2026-10-22; the per-platform docs have the details (WINDOWS_VULKAN.md, IOS_FEASIBILITY.md,
ANDROID.md, PS5_HOMEBREW.md).

## The shape

```
                    ┌─────────────────────────────── the core (same source on every device) ───────────────────────────────┐
  ROM ─ N64Recomp ─▶│ build/RecompiledFuncs, engine32/   src/game/*  (rules, teams, seasons, rosters, packs, UI tabs)       │
                    │ (generated C, never committed)     src/main/*  (launcher, controllers, touch, hints, toasts, logs)    │
                    │                                    lib/* + platform/*-patches (runtime, frontend, RT64, Plume)        │
                    └──────────────┬────────────────────────────────────────────────────────────────────────────────────────┘
                                   │ asks
         ┌─────────────────────────┴──────────────────────────┐
         │ include/blitz_platform.h   what this device has    │  one column per platform
         │ include/blitz_host.h       how it does the OS bits │  one file per platform: src/main/platform_<os>
         └─────────────────────────┬──────────────────────────┘
       ┌──────────┬──────────┬─────┴────┬──────────┬──────────┐
     macOS     Windows     Linux       iOS     Android      PS5        platform/<os>/: packaging (bundle, APK, title folder)
```

A change to the core reaches every platform at its next build. Platform code is limited to:

- **A column in `include/blitz_platform.h`**: which features this device has. These are the only `#if`s on platform
  that the core's own code should need.
- **A host-services file**: `src/main/macos_support.mm`, `platform_windows.cpp`, `platform_linux.cpp`,
  `platform_ios.mm`, `platform_android.cpp` and `platform_ps5.cpp`. Each implements `include/blitz_host.h`: HTTP, the
  ROM drop folder, the window hook, early start-up and the Mac-only menu bits.
- **Packaging** in `platform/<os>/`: the bundle, APK, title folder or exe staging. Every platform stages the same
  resources through `BLITZ_STAGE_RESOURCES` (root `CMakeLists.txt`) and adds only its own extras.
- **Library patches** in `platform/<library>-patches/NNNN-<os>.patch`. These are small, one per platform per library.
  `tools/new_patch.sh` writes a library's new edits as the next patch; `make patches` checks the whole stack.

### What each device has (`include/blitz_platform.h`)

| | macOS | Windows | Linux | iOS | Android | PS5 |
|---|:-:|:-:|:-:|:-:|:-:|:-:|
| Desktop file dialog (NFD) | ✓ | ✓ | ✓ | | | |
| System document picker | | | | ✓ | ✓ | |
| ROM drop folder (imported at startup) | | | | ✓ Documents | ✓ app files | ✓ /data/blitzrecomp |
| Local browser (Roster Room opens here) | ✓ | ✓ | ✓ | LAN address | LAN address | LAN address |
| Open Folder / Reveal | ✓ | ✓ | ✓ | Open Folder: Files app (Documents) | Open Folder: Files app (BlitzFolder) | (FTP: hidden) |
| Export ROM | ✓ | ✓ | ✓ | | | |
| Restart | ✓ | ✓ | ✓ | In place (iPhone) | In place (Android) | |
| Classic ⇄ Modern in one install | ✓ | ✓ (combined folder) | | | | ✓ (two titles) |
| F12 screenshots and clips | ✓ | screenshots | | system | system | Create button |
| Touch controls | | | | ✓ | ✓ | |
| Save states / Redo Play | ✓ | ✓ | ✓ | ✓ + touch pills | ✓ + touch pills | ✓ |

Save states need the resumable copy of the recompiled code (`SAVESTATES=1`, the default), which every platform's
targets build since 2026-10-22. Keys (F5 save, F7 load, F6 slot, F8 Redo) and controllers (hold R3: L1 save, R1 load,
D-pad left/right slot, D-pad down Redo) work everywhere; a phone played by touch has SAVE, LOAD and REDO pills, shown
when Settings → Saves → Save State Buttons and Rules → Redo Play are on. Tested on the Mac; the others build.

### Adding a platform

1. Give it a column in `include/blitz_platform.h`. The header stops the build (`#error`) until it has one.
2. Write `src/main/platform_<os>.cpp` with the functions in `include/blitz_host.h`, and add it to the root
   `CMakeLists.txt`'s host-services switch.
3. Add its dependencies branch (SDL2 and FreeType) and `platform/<os>/<os>_stage.cmake` (resources via
   `BLITZ_STAGE_RESOURCES`, plus its own packaging).
4. Patch the libraries where they need it, and keep each patch small: `tools/new_patch.sh lib/<library>
   platform/<library>-patches NNNN-<os>`.
5. Add `<os>-classic` and `<os>-modern` to the Makefile, and the platform to `tools/release/build_all.sh`.

Android was added this way on 2026-10-22. Nothing in `src/game` changed for it.

## Building everything

`make all-platforms` runs `tools/release/build_all.sh`. It builds Classic and Modern for every platform whose
toolchain is set up on this Mac, carries on past a failure, and prints a table at the end. Logs go to
`build/all/<target>.log`. Use `PLATFORMS="ios android" make all-platforms` for some only.

| Platform | Targets | Output | Gets onto the device |
|---|---|---|---|
| macOS | `app`, `app-modern`, `bundle` | `build/app*/…app`, `build/dist/BlitzRecomp.app` | run it |
| Windows | `win-classic`, `win-modern`, `win` | `build/win/BlitzRecomp.zip` | copy over, unzip |
| Linux | `linux-classic`, `linux-modern` | `build/linux/app_*` (cross-built, not run yet) | copy the folder over |
| iOS | `ios-classic`, `ios-modern` | `build/ios/app_*/BlitzRecompiled.app` | `tools/ios/install.sh classic\|modern` |
| Android | `android-classic`, `android-modern` | `build/android/app_*/<Name>.apk` | `make android-install` (adb) |
| PS5 | `ps5-classic`, `ps5-modern` | `build/ps5/app_*/PPSA9996x` | `tools/ps5/deploy.sh` (FTP) |

Every build stamps its build number (the commit count, `BLITZ_BUILD`). The update notice below compares against it.

## Content and updates: one hub, fanned out, pruned per device

```
  ESPN ─▶ blitz-rosters job (Thu/Sat, tools/roster_season.py) ─▶ the roster job's repo (private) 
                                                                     │  manifest.json, players.json, lineups/*.json
               ┌─────────────────────────────────────────────────────┤
               │ GitHub API + read-only token                        │ any base URL, no token: a hub
               ▼                                                     ▼  (tools/rosters_hub.sh on the Mac, a private host)
      Mac / Windows / Linux                              iPhone / Android / PS5 / another computer
               └──────────── each device: takes what's for it (targets), shows "Rosters updated" / "Update available" ┘
```

- **The source of truth** is the blitz-rosters repo. The job writes `manifest.json`: every file's hash, the newest
  lineup checkpoint, and two optional fields that do the fan-out.
- **`targets`** limits a file to some platforms (names as in `blitz_platform.h`) or to builds from some number up.
  A device skips files that aren't for it. Use this when a file only one platform reads (big art, a format a newer
  build needs) would otherwise go to every device. Set it with
  `tools/roster_season.py target <path> --platforms iOS,Android [--min-build N]`, or lift it with `--clear`.
- **`app`** announces a new app build. A device on an older build shows "Update available: build N" once, where the
  `platforms` list includes it. Set it with `tools/roster_season.py announce --build N [--platforms …] [--notes …]`.
  Both fields survive the job's rewrites. Since the PSP pack, the job also gives every file an explicit
  `platforms` list on each run (`psp/pack.txt` → PSP, the JSON files → `JSON_PLATFORMS` in `roster_season.py`), but
  a list set with `target --platforms` is marked `by_hand` and kept; `--clear` brings the default back next run.
- **Getting it to a device**:
  - A computer with the token (`<app folder>/rosters/token`) reads GitHub directly.
  - Anything else follows a hub: one line in `<app folder>/rosters/hub`, e.g. `http://192.168.0.84:8790/`.
    `tools/rosters_hub.sh` serves the Mac's checkout of blitz-rosters on the LAN.
  - `BLITZ_ROSTER_SOURCE` overrides both: a URL or a folder.
- **Pruned by design, not by manifest**:
  - Headshots are fetched only when Pick Your Crew shows them, so a device that never opens it never downloads one.
  - Classic takes no rosters at all.
  - League paks and Add-Ons stay per device. They come from the device's own folder, and through a document picker
    on phones.

Tested on 2026-10-22 in the Android emulator against a hub on this Mac. The Android-only update notice showed, and a
file targeted at macOS only was skipped.

### Next for the hub

ROADMAP.md tracks these:
- The desktop app could serve its own rosters folder as the hub while it runs (an opt-in "Share rosters on this
  network"), instead of a separate script.
- Hubs could be found on the LAN automatically (mDNS) instead of typing an address.
- App updates could carry a download link per platform, not just a notice.
