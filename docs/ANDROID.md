# Android

Native Classic and Modern Blitz for Android: arm64, Vulkan, sideloaded APKs built on the Mac. Started 2026-10-22.
docs/ANDROID_FEASIBILITY.md has the original research; this doc is how the port actually works and where it stands.

## Status (2026-10-22, emulator only)

Tested in the Android emulator (Android 16 / API 36, arm64 on Apple Silicon):

| | Result |
|---|---|
| Build, APK, install | ✓ Classic 34 MB, Modern 43 MB. Signed, with native libraries aligned for 16 KB pages |
| Launch, resources, app folder | ✓ Resources unpacked on first launch (124 files in ~20 ms) |
| ROM import from the drop folder | ✓ "ok, stored" |
| Launcher (RmlUi on Vulkan) | ✓ Draws fully; touch drives it as a mouse |
| Game code, overlays, saves | ✓ Runs; the Controller Pak save is created and reloaded |
| Touch controls | ✓ A and Start reach the game (it advanced to controller select); hidden in menus |
| Home, then back (window lost and replaced) | ✓ The renderer moves to the new window; no errors |
| Weekly rosters via a hub | ✓ Downloaded over the LAN; per-platform pruning and the update notice work |
| Game graphics | ✗ in the emulator (see below). Needs a real device |
| Audio | not heard (the emulator ran with `-no-audio`; a phone without audio now plays on silently) |
| Controllers | the emulator's keyboard shows up as a controller ("qwerty2"); Bluetooth pads untested |
| File picker (Load ROM, Add-Ons) | built (the system picker via `BlitzActivity.pickFiles`), not tried yet |

### The emulator can't show the game's graphics

The launcher and touch overlay draw correctly. The game's frames don't, for two different reasons:

- **`-gpu host`** (the Mac's GPU through gfxstream and MoltenVK): RT64's game shaders use large texture and sampler
  arrays. The emulator's MoltenVK can't compile those ("'texture' attribute parameter is out of bounds"), and the
  stream to the host then stalls with a black screen. Real Android GPUs (Adreno, Mali) support these arrays, and the
  three Android ports of other N64Recomp games run them.
- **`-gpu swiftshader_indirect`** (software Vulkan): everything runs, slowly, but the game's frames come out
  thresholded to pure black and white. The UI drawn over them is correct, so this is something in SwiftShader's
  handling of RT64's targets, not the swapchain.

So the emulator is for everything except how the game looks. The first real device (the Retroid Pocket Duo,
Snapdragon) decides graphics. If a real GPU shows problems, the Android ports' notes list known Adreno and Mali
fixes: dual-source blending, flat shading, descriptor pool sizes. See "What other ports learned" below.

## Build and install

One-time setup:
- Install the NDK: `sdkmanager --install "ndk;29.0.14206865"`. Android Studio's SDK is at
  `~/Library/Android/sdk`, and `tools/android/env.sh` finds everything.
- `tools/android/build_deps.sh` builds SDL2 (shared) and FreeType into `build/android/prefix`.
  `tools/android/build.sh` runs it automatically if needed.

Then:
```sh
make android-classic           # build/android/app_classic/ClassicBlitz.apk
make android-modern            # build/android/app_modern/NewBlitz.apk (Modern's content from APP_FOLDER)
make android-install           # adb install -r both (FLAVOUR=classic for one)
```

There's no Gradle. CMake builds the game as `libBlitzRecompiled.so` with the NDK, and `tools/android/build_apk.sh`
assembles the APK with the SDK's tools: javac + d8, aapt2, `zipalign -P 16`, apksigner. The signing key is
`BLITZ_ANDROID_KEYSTORE` (+ `_PASS`, `_ALIAS`), else the SDK's debug key. Keep using the same key: Android only
updates an app signed by the key that installed it.

Packages: `com.blitzrecomp.newblitz` (New Blitz) and `com.blitzrecomp.newblitz.classic` (the old Classic app, `make android-classic`; not built by default). Until 2026-11-04 they had other ids (`<old>.blitzrecomp.modern` and `.classic`): `tools/android/migrate_app_id.sh` moves a device's saves to the new app. minSdk 29 (Android 10), targetSdk 36.

### Emulator

There's an AVD for this, `Blitz_API36`, so your everyday one stays untouched:
```sh
~/Library/Android/sdk/emulator/emulator -avd Blitz_API36 -gpu swiftshader_indirect -no-audio -no-snapshot -no-window
```
`-no-window` keeps a crash-consent dialog from blocking startup after an unclean stop. Screenshots:
`adb exec-out screencap -p > shot.png`. A tap: `adb shell input tap X Y` (the screen is 2400×1080 in landscape).

## How it works

- **Java shell** (`platform/android/java/.../BlitzActivity.java`): SDL's `SDLActivity`, plus:
  - It unpacks the APK's `assets/res` (UI, fonts, packs, Modern's art) to `files/res` once per installed version.
    The game reads ordinary files.
  - It sets `BLITZ_RESOURCES`, `APP_FOLDER_PATH` (the app's external files folder) and `HOME` before SDL loads the
    game. The frontend's path code already honours the first two, so no patch was needed for paths.
  - It provides `httpGet` (HttpURLConnection: the system's TLS and certificates) and `pickFiles` (the system file
    picker; picks are copied into the cache and handed back as paths) to native code through JNI
    (`src/main/platform_android.cpp`).
  - It hides the system bars and keeps the screen on.
- **App folder**: `Android/data/<package>/files`, reachable with `adb push` and USB file transfer. A ROM put there is
  imported at startup, as on iOS (Documents) and the PS5 (`/data/blitzrecomp`). Note: a file the adb *shell* creates
  there (`echo > …`) is group-only and the app can't read it. Use `adb push` (world-readable).
- **Window and Vulkan**:
  - The window must be created with `SDL_WINDOW_VULKAN`, or SDL takes the surface for OpenGL and Vulkan fails with
    `NATIVE_WINDOW_IN_USE`.
  - Android surfaces offer RGBA, not BGRA (rt64 patch 0007).
  - Android takes the window away in the background and gives a new one back. `platform_android.cpp` reports each
    change to Plume (`plume_android_set_window`, plume patch 0004), which rebuilds its surface on the new window and
    holds a reference meanwhile.
- **Logs**: everything printed goes to logcat (`adb logcat -s BlitzRecomp`) and to `blitz.log` in the app folder.
  Crash stacks are library+offset (unwinder, `src/main/backtrace_compat.h`); symbolize them with the unstripped
  `build/android/app_*/libBlitzRecompiled.so`.
- **Library patches**: rt64 0007, plume 0004, n64modernruntime 0010 and recompfrontend 0017, all named `-android`.
  zstd's dictionary builder is left out (it needs `qsort_r`, which Android lacks).

## The new app id (2026-11-04)

The packages became `com.blitzrecomp.newblitz` and `com.blitzrecomp.newblitz.classic` (the old ids named a person).
Android treats that as a different app: the new build installs beside the old one, with an empty folder, and both show
in the launcher as New Blitz. To move a device across (the scripts never delete the old app or its data):

1. Install the new build (`make android-modern android-install`, or Blitz Builder). Open it once, quit it. (That first
   start opens Android's "Display over other apps" page and then Blitz Builder: back out of both; the script carries over
   the settings, and the permission when asked.)
2. `tools/android/migrate_app_id.sh` (`--dry-run` first to see what it does). It backs up the old app's saves to
   `build/app_id_migration/android-<serial>-<time>/` on the Mac, then copies them into the new app and checks every file
   by sha256: `saves/` (Controller Paks `*.mpk`, Season saves `*.adv`, the runtime's `.bin`), `savestates/`, `rules/`,
   `tournament.bin`, `checkpoint.bin`, `rosters/` (the saved crews, `crews.json`, live there with the downloaded rosters),
   the Add-Ons (`packs/`, `uniforms/`, `art/`, `announcer/`, `probowl/`, `houston/`), the settings (`*.json`,
   `gfx_limits.txt`) and the stored ROM. Not logs; replays, screenshots and crash dumps only with `--everything` (the
   script names what it leaves out). With a root shell it also gives the files to the
   new app's user and carries over `shared_prefs/blitz_ds.xml` (Swap Screens, Potato Mode), which live in the app's private
   data.
3. Open the new app and check a Season and the settings; then uninstall the old app in Android's Settings.

**The RG DS (GammaOS).** On top of the above, which its root adb does in full:
- `persist.gammaos.dualstack.pkgs` (GammaOS DualStack, both panels as one display) names apps by package: if it lists
  the old id, swap it for `com.blitzrecomp.newblitz` (the script shows the value and sets it, root, with
  `--device-settings` or a yes at its prompt: `setprop persist.gammaos.dualstack.pkgs 'com.dsemu.drastic,com.blitzrecomp.newblitz'`).
  If it only lists `com.dsemu.drastic`, leave it. Never `am start` Blitz while it's on that list.
- "Display over other apps" (the game restarting itself after a crash) is per app: the script copies the old app's
  allow (`appops set com.blitzrecomp.newblitz SYSTEM_ALERT_WINDOW allow`), or allow it in Settings > Apps > New Blitz.
- GammaOS's menu lists the new app as a second New Blitz under Applications until the old one is uninstalled; launch the
  new one from there (`nano_launch` in `tools/android/rgds/h.sh` picks by label: uninstall the old one first).
- `persist.gammaos.nano.ps3xmb.controlcenter` and the other nano switches aren't per app: nothing to change. The script
  lists any other system property or setting that still names the old id.
- The bench's saved copies of the team's settings (`build/rgds_restore/general.json`, `blitz_ds.xml`) go to the new app's
  folders now (`h.sh` uses `com.blitzrecomp.newblitz`).

## Touch controls

`src/main/touch_controls.cpp` is shared with iOS.
- **Layout**: a floating stick on the left half; A, B, TURBO (Z) and R on the right; START at the bottom; MENU (opens
  Settings) at the top left.
- **Input**: SDL finger events become player 1's N64 buttons and stick, merged like the keyboard. The state is
  recomputed from the fingers down on every event, so nothing sticks. A tap shorter than a frame is held until the
  game reads it.
- **When it shows**: hidden while a controller is being used (back at the next touch), while a menu is open (touches
  then work the menu) and before the game starts.
- **Debugging**: each change is logged as `[touch] buttons XXXX`.

## What other ports learned

From the Zelda64, DK64 and Goemon64 Android ports, studied 2026-10-22: ideas only, our own code.
- **Graphics drivers**: on Adreno 6xx, dual-source blending, flat shading and über-shaders can be unreliable. Size
  descriptor pools as the larger of what's asked and the layout, plus a margin (`OUT_OF_POOL_MEMORY` otherwise).
  Present only when the swapchain is valid.
- **Lifecycle**: pause the emulated machine in the background (Goemon's `set_app_paused`). We keep running. Worth
  adding if it drains batteries.
- **Turnip**: they offer the open Adreno driver (libadrenotools) as an option for devices whose stock driver
  misbehaves.

## Next, on a real device

1. Graphics on the Retroid Pocket Duo: a full match, the VS screen, uniforms, fire and replays. Log the Vulkan
   features.
2. Audio, controllers (the Duo's built-in pad, Bluetooth), and the file picker.
3. 4:3 and 16:9 screens, then the Duo's second screen (docs/ANDROID_FEASIBILITY.md §7).
4. Performance: frame times (the FPS counter planned for iOS), heat, battery.
