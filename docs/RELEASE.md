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
| `PPSA99961.zip` | the PS5 title folder for ProsperoStore (docs/PROSPEROSTORE.md; tools/release/make_ps5_store_zip.sh): the kit Blitz Builder sends by default (`build/devices/ps5`, the pinned stable one), `PPSA99961/` at the top, never `launch.elf` or `STABLE.txt`, `sce_sys/param.json`'s `contentVersion` set from the version (1.0.4 -> `01.000.004`). The same name every release; never replace it on a published release (the catalog pins its sha256) | Unzip, FTP `PPSA99961/` to `/data/homebrew`; the ROM by Blitz Builder → PS5, or FTP to `/data/blitzrecomp` |

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

**Publishing.** Nothing is uploaded by the script; its last lines print the command, with every download,
`PPSA99961.zip` and `SHA256SUMS`:

```
gh release create v<v> --repo blitzrecomp/newblitz --title "New Blitz <v>" --notes-file build/release/<v>/RELEASE_NOTES.md \
  build/release/<v>/NewBlitz-* build/release/<v>/PPSA99961.zip build/release/<v>/SHA256SUMS
```

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

## The PS5: one stable build, then betas (2026-11-16)

The team's PS5 went back to stock after the last 1.0.2 test (2026-11-17), so no later PS5 build can be run on a PS5 by us.

- **Stable:** the build that passed on the console, pinned in `build/ps5_stable/current` (a link to e.g.
  `1.0.2-build805/`: `PPSA99961/`, `launch.elf`, and `STABLE.txt` = its version and build, its eboot's sha256, the commit
  and where the test is written up). `make device-kits` puts it in every Mac app as `devices/ps5`, and Blitz Builder's
  PS5 step sends it ("Sends New Blitz 1.0.2 (build 805), the PS5 build tested on a PS5").
- **Beta:** a later version's own PS5 build goes in as `devices/ps5-beta`; the Builder offers **Send Beta (untested)**
  and the live command `SEND ps5-beta <address>`. A version whose stable kit is its own (1.0.2: `STABLE.txt`'s first
  word is `VERSION`) has no beta: its release rebuild would only be an untested copy.
- **The release refuses** a Mac app whose stable kit is missing or whose eboot no longer matches `STABLE.txt`'s sha256.
- **A stable kit must be a release build** (`make ps5-modern BLITZ_CONTENT=public RELEASE=1 VERSION=x.y.z`, as `make
  release` builds it): a dev build carries this Mac's paths and fails `tools/release_scan.sh`. `make_release.sh` checks
  the pinned eboot for them before building (2026-11-17: 803 and 789 were dev builds; 805 is 803's code built for release).
- **An untested default (2026-11-26, the team's call):** from 1.0.4 the pinned kit is that version's own PS5 release build,
  never run on a PS5, because players need its fixes. Its STABLE.txt says `tested: no (...)`, and the Builder then says
  only "Sends New Blitz 1.0.4 (build N)." (not "tested on a PS5"). 1.0.2 build 805 stays in build/ps5_stable to go back to.
- **Keep the stable build working:** later Mac apps still send it its first setup (rules, season, general settings) and
  the season server still serves it rosters. Change those formats only in ways an older build reads (add fields, never
  rename or remove), and keep `paks/catalog.json` and the rosters manifest readable by it.
