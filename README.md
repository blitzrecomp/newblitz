# New Blitz 2027

![New Blitz 2027](docs/media/readme_banner.png)

**NFL Blitz 2000, running natively on almost any device you own.** A Blitz Recomp project.

New Blitz is the 1999 Nintendo 64 game, recompiled to run as a native app: no emulator. It plays the game as it was,
adds what a modern setup wants (widescreen, save states, Redo Play, phones as controllers, tournaments, a modern
season mode), and can bring the game up to date with a season pak.

- **Bring your own cartridge.** New Blitz needs your own dump of **NFL Blitz 2000 (USA) v1.1**. No game assets are
  included, and nobody here can send you one.
- **Free and non-commercial.** Not affiliated with, endorsed or sponsored by the NFL, the NFL Players Association,
  any NFL club, Midway or its successors.

## Get it

Download **New Blitz** for your computer from [Releases](../../releases). The computer app is also the **Builder**:
it sets the game up and sends it to your other devices.

| Device | How | State |
|---|---|---|
| **Mac** (Apple Silicon) | Download from Releases | Tested |
| **Windows** (x64) | Download from Releases | Preview |
| **Linux** (x86_64) | Download from Releases | Untested |
| **iPhone / iPad** | Saved by the Builder as an `.ipa`, signed with **your own** Apple ID (AltStore, SideStore or Sideloadly; [docs/IOS_SIDELOAD.md](docs/IOS_SIDELOAD.md)) | Preview: built from source, tested with a paid account |
| **Android** | Sent from the Builder, or install the APK yourself ([docs/ANDROID.md](docs/ANDROID.md)) | Tested |
| **Dual-screen Android** | The same APK, on two-screen handhelds ([docs/BLITZ_DS.md](docs/BLITZ_DS.md)) | Beta |
| **PS5** (jailbroken, etaHEN) | Sent from the Builder over your network ([docs/PS5_HOMEBREW.md](docs/PS5_HOMEBREW.md)) | Preview |
| **N64 or an emulator** | Export ROM in the Builder writes a `.z64` | Tested |

### Opening it the first time

The downloads aren't signed with a developer certificate, so your computer asks first:

- **Mac:** open the app once (it's blocked), then **System Settings → Privacy & Security → Open Anyway**.
- **Windows:** on the SmartScreen prompt, **More info → Run anyway**.
- **Linux:** `chmod +x` the app, then run it.

## First run

1. **Load ROM.** Pick your dump (`.z64`, `.v64`, `.n64`, or a `.zip` holding one). The app checks it's the right game
   and keeps a copy.
2. **Add the 2026 season?** One click downloads the 2026 Season pak (today's teams, rosters, uniforms and team art,
   about 2 MB) and plays **New Blitz** with it. Or choose **Just the 1999 game** to play the cartridge as it was
   (**Original 1999**). You can add or remove the pak later under **Add-Ons**.
3. **Keep rosters up to date?** Optional: each week's lineups download in the background. Off unless you turn it on.
4. **Where to play:** here; on an N64 or an emulator (Export ROM); or on another device (below).

The Builder runs again from the launcher's **Blitz Builder** row whenever you want another device or an export.

## Sending to your devices

The Builder carries the device builds and sends each one with your ROM and your setup, so the device starts ready to
play. Each device needs its own way in:

- **PS5:** a jailbroken console running etaHEN, on the same network. Enter the console's address; the Builder sends
  the game over FTP and starts it.
- **Android:** turn on USB debugging, connect the phone or handheld, and have
  [platform-tools](https://developer.android.com/tools/releases/platform-tools) (`adb`) installed.
- **iPhone / iPad:** Apple only installs apps you sign yourself, so the Builder doesn't send this one: **Save the
  iPhone app (.ipa)** puts the unsigned app in Downloads → New Blitz for iPhone, with your ROM and setup beside it, on
  a Mac, Windows or Linux. Install `NewBlitz.ipa` with [AltStore](https://altstore.io),
  [SideStore](https://sidestore.io) or [Sideloadly](https://sideloadly.io) and your own Apple ID (a free one works for
  7 days at a time, a paid one for a year), then copy the saved files into New Blitz's folder (Files → On My iPhone →
  New Blitz). Step by step, and building it yourself with Xcode: [docs/IOS_SIDELOAD.md](docs/IOS_SIDELOAD.md).

## Paks

Paks are add-ons the Builder installs: season rosters, uniform sets, team art, announcer calls, rules. The 2026 Season
pak is the one-click download above. Community paks are shared on our [Discord](https://discord.gg/AtcKN9dY5). To make
one, or to bring New Blitz to another device, see the [contributor guide](https://newblitz.pages.dev/contribute).

## Community

- **Discord:** [discord.gg/AtcKN9dY5](https://discord.gg/AtcKN9dY5): help, paks, bugs and showcase.
- **YouTube:** [@BlitzRecomp](https://www.youtube.com/@BlitzRecomp): the trailer, how to get it, and feature walkthroughs.
- **X:** [@blitzrecomp](https://x.com/blitzrecomp).
- **Bugs:** an issue here, with the zip from Settings → General → Save Logs.

## Playing

Gamepads work out of the box; so do phones, as controllers, by scanning a QR code (Settings → Players).

| N64 | Gamepad | Keyboard |
|---|---|---|
| Stick | left stick | W A S D |
| A (pass / switch player) | South (A / Cross) | Space |
| B (jump / tackle) | West (X / Square) | Left Shift |
| Z (turbo) | left trigger | Q |
| C buttons (targeted passes) | right stick, North/East buttons | arrow keys |
| D-pad | D-pad | I J K L |
| Start | Start | Return |

Save states: F5 saves, F7 loads, F6 picks a slot. On a controller, hold a stick pressed in (L3 or R3) as the hotkey:
+ R1 saves, + L1 loads, + □ / ○ picks the slot. Redo Play runs the last play again: hotkey + △ (or D-pad up), or F8.
Fullscreen: F11, or L3 + R3. Screenshots: F12. More, including tournaments, phones as controllers and playing on a TV:
[docs/PLAYING.md](docs/PLAYING.md).

## How it's made

The game itself is NFL Blitz 2000's own code, translated from your cartridge by N64Recomp, a deterministic tool: no AI
writes or changes the game's logic. Everything around it (the menus, seasons, save states, the phone controllers, the
ports to iPhone, Android and PS5) was written with AI coding agents (Claude), directed, reviewed and play-tested by a
person, on real hardware: Mac, iPhone, Android handhelds and PS5. Every release is checked by a test suite, a scan
that keeps game data and private details out of the downloads, and someone playing it. If something's broken, tell us
in #bugs on the [Discord](https://discord.gg/AtcKN9dY5).

## Building from source

The apps are built on a Mac (every platform cross-builds from it). You need your own dump for the build too: the
recompiler reads the game's code from it.

```
make env && make venv     # check the toolchain; the Python tools
tools/build_deps.sh       # SDL2 and FreeType for the Mac (once)
# put your NFL Blitz 2000 (USA) v1.1 dump in rom/
make rom-import           # check it and pin its SHA-1
make test                 # tool tests (synthetic data only)
make app-modern           # the Mac app
make all-platforms        # every platform set up here, with a pass/fail table
```

Per platform: [docs/PLATFORMS.md](docs/PLATFORMS.md). Contributing: [CONTRIBUTING.md](CONTRIBUTING.md).

## Credits and licences

New Blitz stands on N64Recomp and N64ModernRuntime (Wiseguy and contributors), RT64, the Zelda 64: Recompiled team's
frontend, Zoinkity's research and the NFL Blitz community: [CREDITS.md](CREDITS.md). The code is GPL-3.0
([LICENSE](LICENSE)); third-party licences are in [NOTICES.md](NOTICES.md).

NFL Blitz is a trademark of its owner. All names, logos and trademarks belong to their owners.
