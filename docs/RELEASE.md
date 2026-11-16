# Releasing

## Checklist

1. **Permissions.** Classic Blitz and Modern Blitz (on the original game since 2026-10-19) contain nothing of Zoinkity's. The optional Team Extension build compiles his code in, so it isn't released without his blessing (docs/ZOINKITY.md). Team names, logos and uniforms are the NFL's and its clubs': the credits page states we're unaffiliated, which doesn't settle whether to share widely.
2. **Libraries for macOS 13+.** Run `tools/build_deps.sh` once. Then check that every dylib in `Contents/Frameworks` says `minos 13.0`:
   `otool -l <dylib> | grep -A3 LC_BUILD_VERSION`.
3. **Version.** Set `BLITZ_VERSION` in CMakeLists.txt and add a CHANGELOG.md entry.
4. **Sign and notarize.** This needs a Developer ID Application certificate in the keychain (Xcode → Settings → Accounts → Manage Certificates → + → Developer ID Application). Then:
   ```
   xcrun notarytool store-credentials blitz --apple-id <id> --team-id <TEAMID>   # once; asks for an app-specific password
   make notarize SIGN_ID="Developer ID Application: <Your Name> (TEAMID)" NOTARY_PROFILE=blitz
   ```
   The last line of `make notarize` should read `accepted, source=Notarized Developer ID`.
5. **Test as a downloader would.** Mark the zip as downloaded and unpack it:
   ```
   xattr -w com.apple.quarantine "0083;$(printf %x $(date +%s));Safari;" ClassicBlitz-macos-arm64.zip
   ditto -x -k --qtn ClassicBlitz-macos-arm64.zip .
   ```
   Then check `spctl -a -vv -t exec "Classic Blitz.app"` says accepted, and `syspolicy_check distribution "Classic Blitz.app"` passes.
6. **First run elsewhere.** Use another Mac, or a new macOS user: Load ROM, play a game, then quit and relaunch.
7. **Public tree.** Run `tools/make_public_tree.sh --author "Name <email>"`. Review `build/public/blitzrecomp`, push it, and attach the zip and its SHA-256 to a GitHub release.

## The public downloads: `make release`

`make release VERSION=0.9.1` (tools/release/make_release.sh) builds every app public (`BLITZ_CONTENT=public`) with
`RELEASE=1` (no build-machine paths inside: `-ffile-prefix-map`, a Windows `.pdb` named without its folder) and writes
`build/release/<version>/`:

| File | What |
|---|---|
| `NewBlitz-v<v>-macOS-arm64.zip` | `New Blitz.app`, ad hoc signed with the distribution entitlements (checklist step 4 would replace that with a Developer ID) |
| `NewBlitz-v<v>-Windows-x64.zip` | the `New Blitz` folder: `New Blitz.exe`, its DLLs and resources |
| `NewBlitz-v<v>-Linux-x64.tar.gz` | the `New Blitz` folder: the executable (debug info stripped) and its resources |
| `SHA256SUMS`, `RELEASE_NOTES.md` | the checksums of every `NewBlitz-*` file; the notes are CHANGELOG.md's top section plus how to open each download |

Then one obvious file per device (tools/release/make_downloads.sh, from the same staged folders). These are what the
site's download page and the README's "Get it" table point people to; the zips and the tarball stay for RecompLauncher
and for anyone who'd rather unpack than install.

| File | What | How a player opens it |
|---|---|---|
| `NewBlitz-v<v>-Windows-Setup.exe` | NSIS installer (platform/windows/installer.nsi) of the Windows folder: per user, into `%LOCALAPPDATA%\Programs\New Blitz`, Start menu shortcut, optional desktop shortcut, uninstaller in Settings → Apps (HKCU), the app's icon. Installs over an older version in place. The uninstaller removes the program only, never the game data in `%LOCALAPPDATA%\Blitz2026Recompiled` or `BlitzRecompiled` | Run it; SmartScreen: More info → Run anyway. No administrator prompt |
| `NewBlitz-v<v>-macOS.dmg` | the same `New Blitz.app` as the zip (ad hoc signed; checked with `codesign --verify --deep --strict` inside the mounted image) and an Applications alias, in a styled window: navy background with the NEW BLITZ 2027 logo, a yellow arrow from the app to Applications, a caption, the app's icon as the volume's (dmgbuild from `.venv`: platform/macos/dmg_settings.py; the background is drawn at build time by tools/make_dmg_background.py) | Open it, drag New Blitz to Applications, then Open Anyway once |
| `NewBlitz-v<v>-x86_64.AppImage` | the Linux folder, with Debian 12's SDL2 and FreeType and what they need that a desktop may lack (libpng, brotli, libsamplerate, libdecor, libXss) in `usr/lib`, found through the executable's RUNPATH. Vulkan, glibc, X11/Wayland, ALSA/PulseAudio and Mesa come from the system. **Untested on Linux** | `chmod +x`, run |
| `NewBlitz-v<v>-Android.apk` | the Android device kit, as the build signed it (the release key when `BLITZ_ANDROID_KEYSTORE` is set: the script checks the APK's certificate against it). None with `RELEASE_NO_ANDROID=1` | Open it on the device, allow installs from that source, open New Blitz, pick your ROM |
| `NewBlitz-v<v>-iPhone.ipa` | the iPhone device kit, **unsigned** (the script refuses one with a provisioning profile or a team) | Sideload with AltStore, SideStore or Sideloadly, open New Blitz, pick your ROM |

Tools for these, all from Homebrew: `brew install makensis squashfs patchelf p7zip` (p7zip is for the scan). Set
`HOMEBREW_NO_INSTALLED_DEPENDENTS_CHECK=1 HOMEBREW_NO_INSTALL_CLEANUP=1` when installing them: on 2026-11-15 a plain
`brew install` upgraded other formulae and its cleanup removed the old real `sdl2` keg (Homebrew's `sdl2` is now
sdl2-compat), which CMakeLists.txt falls back to when `build/deps/prefix` is missing; run `tools/build_deps.sh` in a
checkout without it.

**The AppImage runtime.** An AppImage is the AppImage project's type 2 runtime (a small static x86_64 ELF) with the
app's squashfs appended. Homebrew has `mksquashfs` but neither the runtime nor appimagetool, so the release takes the
runtime from `build/appimage/runtime-x86_64` (or `APPIMAGE_RUNTIME=<file>`), and its SHA-256 from a line
`<sha256>  appimage-runtime-x86_64` in tools/deps.sha256 once that line exists. Without the file, the AppImage is
skipped with a message, the AppDir and its squashfs are left in `build/release/stage/downloads/`, and the rest of the
release goes on. The runtime is the `runtime-x86_64` asset of https://github.com/AppImage/type2-runtime/releases (about
1 MB); it isn't downloaded by any script here.

Every archive also holds `recomplauncher.json` beside the app: the manifest RecompLauncher reads (platform/recomplauncher;
its spec is recomplauncher.com/sdk). It asks the player for their USA v1.1 cartridge (game code NBIE), starts the app
with `--import-rom <that file>`, and names the app folder on each system for save backups and logs. The archive names
carry the system and processor because that's how RecompLauncher picks a download. Publish the 0.9.x builds as GitHub
pre-releases: the launcher offers the latest full release by default.

Every computer's app carries the same device kits for Blitz Builder (`Resources/devices` on a Mac, `devices/` beside the
executable elsewhere): the PS5 title and launch payload, the Android APK and the iPhone app as an **unsigned**
`NewBlitz.ipa` (Save the iPhone app (.ipa); docs/IOS_SIDELOAD.md §3). A team-signed iPhone kit (`DEVICE_KITS_IOS=1`) is
refused. The APK is signed with `BLITZ_ANDROID_KEYSTORE`, else this Mac's debug key: keep the same key for every release,
or Android won't update an installed copy. The release key (2026-11-14) is `~/Keys/newblitz-release.jks`, alias `newblitz`:

```
read -rs "BLITZ_ANDROID_KEYSTORE_PASS?Keystore password: " && echo && export BLITZ_ANDROID_KEYSTORE_PASS \
  BLITZ_ANDROID_KEYSTORE=~/Keys/newblitz-release.jks BLITZ_ANDROID_KEYSTORE_ALIAS=newblitz && make release VERSION=<v>
```

Keep the `-r`: without it zsh's `read` eats backslashes, and a password with a `\` then fails ("keystore password was
incorrect").

Last, **tools/release_scan.sh** unpacks every download (the `.dmg` attached read-only and detached, the AppImage's
squashfs read with `unsquashfs` at the runtime's end, the installer extracted with `7z`, and the `.apk`/`.ipa` inside
each) and fails the release on: this
machine's home folder or user name; the developer's name and addresses (git identity, the private repo's owner,
`build/public_deny.txt`); "Houston Texans"; any private address or key from `build/` (live rosters, the logs and
remote-testing mailbox, install links, PSP and season-server keys; read at scan time, never printed) or any
`*.workers.dev` host but the season server's; and files a public build must not hold (`logs_hub`, `rosters/token|hub|repo`,
a provisioning profile, the 2026 roster pack, Houston). In a worktree whose `build/` lacks those secret files, point
`BLITZ_SECRETS_DIR` at the main checkout's `build/`. `RELEASE_SKIP_BUILD=1` repackages without building.

The dependency scripts (tools/build_deps.sh, tools/ios/build_deps.sh, tools/android/build_deps.sh, tools/ps5/setup_sdk.sh)
build SDL and FreeType with the same path mapping: libraries built before 2026-11-14 keep the build machine's paths in
SDL's asserts, so run them again before a release.

## Known issues (0.1.0)

- **Apple Silicon only** (arm64 builds); Intel Macs aren't supported.
- **Experimental features:** Resume Interrupted Games, save states and Redo Play are marked experimental.
- **Play Editor** lists 31 teams, even with the 32-team season.
- **Simulated Standard Season weeks** repeat some scores: the game reseeds from week 1's results, as it always did.
- **Real roster names:** the real-name NFL roster pack is a separate download. Without it, Modern Blitz plays the ROM's own teams.

## Open before the first public release

- ~~**RecompFrontend has no license file.**~~ Settled 2026-11-13: upstream now carries `COPYING` (GPL-3.0; GitHub's licence API for N64Recomp/RecompFrontend). Since 2026-11-14 the pin is upstream main (2ddbfc8), which has it: `lib/RecompFrontend/COPYING`.
- **The full BLITZ 2027 logo includes the NFL wordmark.** Decide whether the public apps ship the BLITZ '27 lockup (no NFL mark) instead.
- ~~**Getting a Modern Blitz ROM.**~~ Settled 2026-10-19: Modern plays the plain USA v1.1 ROM, the same one as Classic.
