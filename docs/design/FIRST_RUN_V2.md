# First run v2: one file per device, all the guidance on first launch

> **Status:** built for the 0.9.5 test on branch `first-run-v2` (2026-11-15; `src/main/firstrun.cpp`, `firstrun_screen.cpp`,
> `crash_report.cpp`), with §12's answers and the team's later notes: Back and Exit on every screen, Main Menu first in Settings
> → General, no Export ROM row (Blitz Builder → N64 or emulator), season pak updates following Roster Updates.
> Device checks waiting: docs/HUMAN_TESTS.md. Was: spec for approval (branch `launch`). Supersedes the first-run
> section of `README.md` and the Builder questions in `src/main/builder.cpp`. Not related to `FIRST_RUN.md`, which is
> about in-game hints (those stay as they are).
> Test build: **0.9.5** on Mac, Windows, Android and iPhone (free Apple ID). If it's good, it becomes **1.0**.

## 1. Goals and non-goals

**Goals**
1. A player downloads one obvious file for their device, opens it, and the app tells them everything else.
2. Every path ends with the player playing, or holding the file they need, and plain next steps.
3. First run can never silently not start. It logs every step (`[firstrun]`), has a fallback row in the launcher, and
   can be resumed after any interruption.
4. The player always knows which ROM is in use.
5. Updates and crash reports are two clear, optional choices, made once, changeable in Settings.

**Non-goals**
- Packaging (Setup.exe, .dmg, AppImage, .apk, .ipa): another agent builds it. This spec is what the app does after it opens.
- In-game hints (`FIRST_RUN.md`), the Settings layout, the ROM validation itself, the pak format, the roster format.
- Any download containing the ROM. The player always supplies their own NFL Blitz 2000 (USA) v1.1.
- Diagnosing the Windows 0.9.2 bug. The design routes around it and logs enough to find it (section 7).

## 2. Flow per platform

Words in **bold** are screens (section 3). `[T]` marks a step skipped on that path.

### 2.1 Computer (Mac, Windows, Linux)
1. **A. Where** ("Where do you want to play?"). Default: This computer.
2. By choice:
   - **This computer:** B. ROM → C. 2026 → D. Updates and reports → Play (first game starts).
   - **Phone or handheld:** **P. QR** (no ROM needed). "Done" ends setup. Or "No camera? Make the file on this computer":
     B. ROM → C. 2026 → **E. Which device** (Android or iPhone) →
     - Android: **F. Android** (send over USB with adb, or save the APK) → **G. Install steps**.
     - iPhone: **H. iPhone** (save the `.ipa` with the ROM and setup inside) → **G. Install steps**.
     - Both then → D. Updates and reports? **No.** The phone asks its own (section 2.2). The computer skips D on this path.
   - **PS5:** B. ROM → C. 2026 → **I. PS5 address** → send and start → **J. Sent**.
   - **N64 or emulator:** B. ROM → C. 2026 → **K. Lite or Full** → save the `.z64` → **L. Here it is**. No D (no internet needed, and the file isn't a running app).
3. Finishing any path sets `welcomed` (section 5). Back out of the launcher any time; **Finish setup** brings it back.

### 2.2 Android phone or handheld, and iPhone
Never see A.
1. **B. ROM** (system file picker).
2. **C. 2026.**
3. **D. Updates and reports.**
4. Play (first game starts).

When the device was set up from a computer (send over adb, saved APK/IPA with setup inside, PS5), `builder_done` is
already in the app folder (`device_send.cpp`, `config.cpp`): the app skips B, C, D and plays. The ROM and the season
answer were given on the computer. D is the exception: it is never answered on the computer for a device, so the device
**still asks D once** on its first launch (a `setup_updates_asked` mark, section 5, is not in the handoff).

Android with about 1 GB of RAM: `BlitzRomFirst.java` already shows the system picker before the game loads. Keep it.
Once the ROM is in, the app continues at C. Log both halves under the same `[firstrun]` lines (section 7).

### 2.3 PS5 as a target
The PS5 never runs onboarding: the computer sends the ROM, the season, the setup and `builder_done`
(`device_send.cpp`, FTP port 1337, then `launch.elf` on 9021). D on the PS5: not asked; Roster Updates follows the
computer's choice sent in the setup (`season.json`); crash reports stay off (no upload support on PS5 in 0.9.5).

## 3. Screens

Common rules for every screen:
- **Header:** small eyebrow `SETUP · step n of N` (N counts the screens on this path), title, body, buttons.
- **Back** is always a button and always the cancel key (Esc, B/○, Android Back, a left-edge swipe on iPhone). It goes to
  the previous screen. On the first screen it leaves setup for the launcher (`welcomed` stays false, **Finish setup** shows).
- The default button is focused when the screen opens and is the big one.
- Copy is kept to what each existing prompt says now. Style: Blitz '27 (`ui_kit.cpp`).
- Built as one `FirstRunScreen` (title, body, N buttons, optional toggles, optional image, step line), since the framework's
  two-button choice prompt has no Back and no toggles.

### A. Where do you want to play? (computers only)
Body: *Pick one. You can run this again any time from Blitz Builder in the launcher.*

```
 SETUP
 Where do you want to play?

 Pick one. You can run this again any time from Blitz Builder.

 [        This computer         ]   <- default, big
 [    Phone or handheld         ]
 [          PS5                 ]
 [   N64 or emulator            ]
                      Skip setup
```
- **This computer** (default) → B. **Phone or handheld** → P. **PS5** → B (with `where=ps5`). **N64 or emulator** → B (`where=export`).
- **Skip setup:** to the launcher. `welcomed` stays false, **Finish setup** shows. (Text button, not a Back: there is nothing before it.)
- Back/Esc = Skip setup. Logs `answer: where=...`.

### P. Phone or handheld: the QR code
Body: *Scan this with your phone. The page gives you the Android or iPhone app, and the app asks for your ROM itself.*

```
 SETUP
 Get it on your phone

 Scan this with your phone's camera.
 The page gives you the app, and the app asks for your ROM.

        ########  ####
        ##    ##  #  #       newblitz.com/download
        ## ## ##  ####
        ########  #  #

 [          Done           ]   <- default
 [ No camera? Make the file on this computer ]
 Back
```
- The QR code encodes `https://newblitz.com/download`. The address is also printed under it, for a handheld with a browser.
- **Done** (default): sets `welcomed`, goes to the launcher. **No camera?** → B (`where=device`). **Back** → A.
- No network is needed on the computer to show the code.

### B. Load your ROM (computers)
Shown when there is no valid stored ROM, or a stored ROM exists and was saved before this setup began (a leftover).

No ROM yet:
```
 SETUP
 Load your NFL Blitz 2000 ROM

 The USA cartridge, v1.1 (.z64, .n64, .v64 or a .zip).
 New Blitz keeps its own copy. It never leaves this computer.

 [        Load ROM          ]   <- default (opens the file picker)
 Back
```
Leftover stored ROM:
```
 Using your saved ROM
 NFL Blitz 2000 (USA) v1.1 · saved 3 Nov 2026

 [      Use It      ]   <- default
 [   Change ROM     ]
 Back
```
- **Load ROM / Change ROM:** the system file picker (a sheet on the game window; Android and iOS use the system picker, iOS the Files sheet from `0016-ios.patch`). Cancelled picker: back to this screen.
- **Use It:** continue. The ROM shown is the stored one: name from the ROM table (`launcher.cpp` RomNameLabel) or the file name, plus the file date.
- **Wrong or bad ROM** (replaces the screen, same buttons):
  `That's not the right game. New Blitz needs NFL Blitz 2000 for the N64, USA, v1.1. A different region or version won't work.` Buttons: **Try Another** (default), **Back**. A ROM that is the game but a bad dump says: `That file looks damaged. Try dumping it again.`
- Phones and handhelds: the same screen, but without Back (it is the first screen) and with **Skip** instead (to the launcher).

### C. Add the 2026 season?
Exists today (`builder.cpp` AskGame). Text and buttons unchanged.

```
 SETUP
 Add the 2026 season?

 Today's 32 teams, rosters, uniforms and team art, as a small
 download from the New Blitz server.

 Or play NFL Blitz 2000 exactly as it shipped in 1999.

 Either way you get widescreen, save states and the rest. Add the
 season any time from Add-Ons.

 [          Add It            ]   <- default
 [   Just the 1999 Game        ]
 Back
```
- **Add It:** sets `season.game = New Blitz`, starts the download (`pak_download_start`), shows `Getting the 2026 Season…` (non-dismissable, **Cancel** after 10 s). Success → next screen. Failure → **C-fail**.
- **Just the 1999 Game:** `season.game = Original 1999`, sets `setup_season_asked`. Next screen.
- **C-fail** ("No 2026 Season for now"): why in one line (offline, the server unreachable, a bad checksum: the existing messages), then **Try Again** (default) / **Play the 1999 Game for Now** (sets `game = Original 1999`, `setup_season_asked`; the season stays addable from Add-Ons).
- Already installed (pak present): this screen is skipped, logged `skipped: season already installed`.

### D. Updates and reports (devices with internet: computers on the "This computer" path, phones, handhelds, iPhone)
```
 SETUP
 Updates and reports

 Two choices. Change them any time in Settings.

 [x] Keep rosters up to date
     New lineups download in the background when the
     app starts, on Thursdays and Saturdays in season.

 [ ] Send crash reports to the team
     If the app crashes, send a small report the next time
     you open it. No ROM, no files, no name. See what is sent.

 [         Continue          ]   <- default
 Back
```
- Two switches. Rosters **on** by default (`live_rosters`), crash reports **off** by default (`crash_reports`). Continue writes both, sets `setup_updates_asked`.
- The rosters row is hidden when the player chose **Just the 1999 Game** (rosters need the season). The screen then shows only the reports row.
- **See what is sent** opens a short text (section 6.2). Back goes to the previous screen and does not write the switches.
- Offline when D is shown: shown anyway, with the line `You're offline. Rosters will update when you're connected.`
- Not shown for the N64/emulator export and the PS5/phone-from-computer paths (the target device asks, section 2.2).
- Changing them later: Settings → Teams → Roster Updates (exists), Settings → General → Send Crash Reports (new).

### Play
Computer, phone, handheld: the game starts (`start_game()`), `welcomed` is set, log `done`. No extra screen: the in-game hints take over. Customize is no longer asked here (Settings is where the rules live, and the existing hints say how to open it); the Blitz Builder row still offers a Customize pass for sending a customized setup (section 4).

### E. Which device? (computer, after "No camera?")
```
 SETUP
 Which phone or handheld?

 [        Android          ]   <- default
 [        iPhone           ]
 Back
```
Android → F. iPhone → H.

### F. Android
```
 SETUP
 Send to your Android device

 Connect it with USB, with USB debugging on.
 (Settings → About → tap Build number 7 times,
  then Developer options → USB debugging.)

 [      Send over USB       ]   <- default
 [      Save the APK        ]
 Back
```
- **Send over USB:** `send_to_android()` (adb install, then the ROM, the setup and the pak into the app folder). Toasts say how it went (existing). Success → G (Android, USB).
- **Save the APK:** writes the app into Downloads → New Blitz for Android (`NewBlitz.apk`, plus the ROM and setup in a folder), shows the folder → G (Android, file).
- adb missing: see section 8. No device: `No device found. Plug it in, unlock it, and allow USB debugging on its screen.` with **Try Again**.

### G. Install steps (the last screen of a device path)
Android, USB: `It's on your device. Open New Blitz on it: it starts ready to play.`
Android, file: `Copy NewBlitz.apk to the device (USB, a cloud folder, or a card) and open it. Android will ask you to allow installs from that source. Then copy the "Copy to the device" folder into New Blitz's folder (Android/data/com.blitzrecomp.newblitz/files).` Plain buttons: **Open Folder** (default), **Done** (sets `welcomed`, to launcher).

### H. iPhone
```
 SETUP
 Make the iPhone app

 Apple only installs apps you sign yourself. New Blitz makes
 the app with your ROM and setup already inside. You sign it
 and install it with your own Apple ID.

 [   Save the iPhone app (.ipa)   ]   <- default
 Back
```
Then G (iPhone), shown after the save:
```
 Saved in Downloads → New Blitz for iPhone

 1. Install NewBlitz.ipa with AltStore, SideStore or Sideloadly,
    signed with your own Apple ID.
    A free Apple ID works for 7 days at a time, then re-sign.
    You can have 3 free apps at once.
 2. On the iPhone: Settings → Privacy & Security → Developer
    Mode (on), then trust your Apple ID under General → VPN &
    Device Management.
 3. Open New Blitz. It starts ready to play.

 [ Open Folder ]  [ Done ]   <- Done default
```
- The `.ipa` contains the ROM, `season.json`, `rules.json`, the pak and `builder_done` inside the bundle (new: the iPhone app imports them from its own bundle at first start, `main.cpp` drop-folder code, so there's no separate "copy to the iPhone" step). Because the ROM is inside, the screen says: `This file holds your ROM. Don't share it.`
- When a Mac has the private device kit (`iphone_kit_available`): unchanged (Xcode install). Not part of the public flow.
- Free Apple ID limits are stated here, not hidden (section 8).

### I. PS5 address
The existing keyboard prompt: `The PS5's address`, prefilled with `ps5_last_address()`, plus a line above it: `On the PS5: etaHEN running, same network. Settings → Network shows the address.` **Send and Start** (default) / Back. Bad address format: `That doesn't look like an address (like 192.168.1.20).` Not reachable: section 8.

### J. Sent (PS5)
`Sent. New Blitz is starting on your PS5.` (or the existing failure toast text and **Try Again**). **Done** → launcher, `welcomed` set.

### K. Lite or Full (N64 / emulator)
The existing Export screen (`export_screen.cpp`) is used as is, so a player who knows it sees no change. The only new thing is the lead-in: `Full needs an Expansion Pak (a flash cart can't add memory). Lite fits a plain N64.` Default **Lite**. **Back** goes to C.

### L. Here it is (export done)
```
 Your ROM is saved
 Downloads/NewBlitz-Lite.z64

 Next steps
 N64 with a flash cart: copy it to the card and load it.
 Emulator: open it like any game. RetroArch and other mupen64plus
 emulators freeze at team select with Choose CPU Opponent: use the Lite.

 [ Open Folder ]  [ Done ]   <- Done default
```

### Launcher changes (not a screen of the flow)
- **ROM line:** under the title, always, once a ROM exists: `Using your saved ROM · Change ROM` (the second part is the existing Change ROM row, focusable; and the line's hover/long text shows the ROM name and saved date). Replaces the "New here? Play to start" tip only when the stored ROM predates this install (a leftover): `Using a ROM saved earlier (3 Nov 2026) · Change ROM`. "Earlier" = the ROM file's date is before this app's install/first-run mark (`setup_started` in the log, section 5).
- **Finish setup** row: the first row, shown only while `welcomed` is false (section 7).

## 4. Controller, keyboard and touch

| Screen type | Controller | Keyboard / mouse | Touch (phone, iPhone, handheld screens) |
|---|---|---|---|
| Any screen with buttons | D-pad or left stick up/down moves focus; ✕/A or the face button the app uses for "OK" activates; ○/B = Back; Start = default button | Up/Down or Tab/Shift-Tab; Enter = activate; Esc = Back; mouse click | Tap; Android Back / iPhone left-edge swipe = Back |
| Toggles (D) | Left/right or ✕/A flips the focused row; Down to Continue | Space or Enter flips; Tab to Continue | Tap the switch |
| QR (P) | No input needed; Down to the buttons | Same | Same |
| ROM picker | The system file picker. On a handheld the Android picker is controller-driven by the system | Native file dialog | The Files / Android picker |
| PS5 address | The existing on-screen keyboard (`open_keyboard`) | Typing | The on-screen keyboard |

Rules: focus is always on the default button when a screen opens; no screen requires a pointer; a screen never shows only
a touch instruction on a device with no touchscreen (the app knows which, `blitz_platform.h`).

## 5. What persists, and where

| Thing | Where | Set when |
|---|---|---|
| `welcomed` (bool, exists) | `general.json` in the app folder | Setup finished (Play, Done, Sent, Here it is, or `builder_done` seen at start). **Never** set at start, at ROM load, or by backing out |
| `builder_done` (file, exists) | app folder (or the drop folder, moved in at start, `main.cpp`) | Written by a computer that set the device up; read once at start (`config.cpp`), then removed |
| `setup_season_asked` (bool, **new**, hidden) | `general.json` | C answered (Add It succeeded, Just 1999, or Play the 1999 Game for Now) |
| `setup_updates_asked` (bool, **new**, hidden) | `general.json` | D's Continue |
| `live_rosters` (exists, Settings → Teams) | `season.json` | D's Continue (writes the player's choice, default ON) |
| `crash_reports` (bool, **new**, default OFF, Settings → General) | `general.json` | D's Continue |
| `season.game` (exists) | `season.json` | C |
| the stored ROM | the app folder (existing store) | B |
| the season pak | `paks/season2026/` (existing) | C |

Resume rule: **Finish setup** (and the launcher-ready trigger) start at A (computers) or B (devices) and **skip any question
already answered**: B when a valid ROM is stored (shown as "Using your saved ROM" only if it's a leftover), C when `setup_season_asked`
or the pak is installed, D when `setup_updates_asked`. The player is never asked the same thing twice.

### Interrupted first run
| What happened | Result next start |
|---|---|
| Quit during A, P, B, E to I | `welcomed` false. Setup starts again at A (computers) or B (devices). Finish setup row shows |
| Quit during the season download | The partial pak isn't installed (`season_pak.cpp` checks everything before swapping). C asks again |
| Download fails (offline, server, checksum) | C-fail: Try Again or play 1999. Season addable later from Add-Ons |
| Quit during D | D asked again (nothing written until Continue) |
| Quit after Play started the game | `welcomed` already true. Normal |
| Offline from the start | A, B work. C fails with C-fail. D shows with the offline line |
| Wrong ROM picked | B error screen. Nothing stored (the app removes a stored ROM that turns out bad) |
| ROM stored, `welcomed` false (the Windows 0.9.2 state) | Setup starts at A, B shows "Using your saved ROM" |

## 6. New pieces in detail

### 6.1 Trigger and fallback (replaces the timing-dependent open in `builder_update()`)

Known: on Windows 0.9.2 `welcomed` is false, a valid ROM is stored, the launcher shows, and `builder_open()` never runs; the
same build on the Mac asks. **Cause not diagnosed**: the code that gates the open
(`builder_update()`: the launcher-context check, the single `checked` latch that is set only when `is_rom_valid` is true on a tick,
the 50 ms poll in `update_gfx`) is where to look, nothing is claimed. The design does not depend on any of it:

1. **State, not an event.** Setup is needed when `!welcomed && !autostarted && !BLITZ_LEAN && !ultramodern::is_game_started()`.
   Evaluated every tick, with **no latch**. Needed-ness is derived from state each time, so a missed tick or a late ROM can't lose it.
2. **Start it from the launcher's own ready hook** (`on_launcher_update`, main.cpp, its frame counter), not from `update_gfx`'s
   poll: when the launcher has existed for 30 frames and setup is needed, call `firstrun_start("launcher-ready")`.
3. **Watchdog.** If setup is needed, no screen is up, and 3 seconds have passed since the launcher first showed, log
   `[firstrun] WARNING needed but not started` (with the state, section 7) and start it (`trigger=watchdog`). Re-armed every 3 s, at most 3 times.
4. **Fallback row.** The launcher's first row is **Finish setup** while `!welcomed`. It calls `firstrun_start("finish-setup-row")`. It is the player's
   way out whatever else fails, and a test can tell "auto trigger failed" from "setup is broken" by whether the row works.
5. **Single owner.** `firstrun_start` is idempotent: if a screen is already up it does nothing and logs `already running`.

### 6.2 Crash reports (setting and upload: neither exists in the public app)

Checked: crash logging exists locally (`app_log.cpp`: `blitz.log`, the previous run in `blitz.prev.log`, `[crash]` lines with the call stack,
Windows minidumps in `dumps/`). Upload exists only in **private test builds**: Android reads `res/logs_hub` (an asset written by
`tools/android/build_apk.sh` from a file outside the source), and the public build strips it (`tools/release_scan.sh`). So there is no
setting and no public upload. Smallest version:

- **Setting:** `crash_reports`, General tab, off by default. The first-run screen D and Settings both write it.
- **When:** at the next start after a crash, only if on. A crash = `blitz.prev.log` ends with a `[crash]` block (all platforms), or on Android the
  `running` marker file is still there. One report per crash. If it can't connect it tries again at the next start, at most 3 times.
- **What:** one gzipped text body of at most 256 KB:
  - the app version/build, platform and OS version, device model (phones), the graphics name the game printed;
  - the `[crash]` block and call stack from the log (offsets, no paths beyond the app's own file names);
  - the last 200 lines of `blitz.prev.log`, with the user's home folder and user name replaced by `~`, and ROM and pak file names removed;
  - the `[firstrun]` lines.
- **Never sent:** the ROM, any save or file content, Windows minidumps (they hold memory, which can hold ROM data), the account or device name, a stored address.
- **Where:** a public endpoint on the season server's Worker (a new route, kept apart from the private `blitz-logs` Worker, `tools/blitz_logs_worker`, which is excluded from the public tree). Its address goes in a file in Resources written at release time from outside the source, like the pak server (`Resources/paks/server`), not in code. The server stores the body without the sender's IP and keeps it 30 days.
- **Player-visible:** a toast on send: `Crash report sent. Thanks.` **See what is sent** (screen D, and the Settings row) shows the list above.
- Not in 0.9.5 if short of time: the setting and screen ship with the switch off and no upload (honest text: `Reports aren't sent in this build yet`), so the flow doesn't change later. Decide in the open questions.

## 7. Logging

Every first-run event prints one line with the `[firstrun]` prefix to stderr, which `app_log.cpp` mirrors to `blitz.log`. Replaces `[builder]` lines
for the same events. A platform where setup doesn't start must still show lines 1 to 4.

| # | Line (key=value after the tag) | When |
|---|---|---|
| 1 | `[firstrun] app up platform=windows build=0.9.5 appfolder=<path>` | start (also tells the tester which folder to clear) |
| 2 | `[firstrun] state welcomed=0 rom=valid\|none\|bad romsaved=<date> pak=1 season_asked=0 updates_asked=0 builder_done=0` | start, and again when the launcher shows |
| 3 | `[firstrun] waiting for launcher` | until it exists |
| 4 | `[firstrun] launcher ready ms=<since start>` | the launcher exists and shows |
| 5 | `[firstrun] needed=yes\|no reason=<welcomed\|builder_done\|autostart\|lean\|game>` | each time the decision is made |
| 6 | `[firstrun] trigger=launcher-ready\|watchdog\|finish-setup-row\|builder-row` | setup begins |
| 7 | `[firstrun] WARNING needed but not started after 3000 ms <state>` | watchdog |
| 8 | `[firstrun] rom ready source=stored-earlier\|picked\|drop name=<name>` | B done |
| 9 | `[firstrun] rom rejected reason=wrong-game\|damaged` | wrong ROM |
| 10 | `[firstrun] asked <screen>` and `[firstrun] answered <screen> <button>` | every screen (A, P, B, C, D, E, F, H, I, K) |
| 11 | `[firstrun] season download start`, `season download ok\|failed <why>` | C |
| 12 | `[firstrun] updates rosters=on\|off reports=on\|off` | D's Continue |
| 13 | `[firstrun] send start\|ok\|failed target=android-usb\|android-file\|ipa\|ps5 <why>` | F, H, I |
| 14 | `[firstrun] export start\|ok\|failed kind=lite\|full` | K |
| 15 | `[firstrun] abandoned at=<screen>` | Back/Skip out of setup |
| 16 | `[firstrun] done target=computer\|device\|ps5\|export welcomed=1` | the end |

Screen names: `where`, `qr`, `rom`, `season`, `season-failed`, `updates`, `device`, `android`, `iphone`, `ps5`, `export`, `export-done`, `install-steps`.

## 8. Edge cases

| Case | What the player sees |
|---|---|
| **No internet** | A and B work. C: `Add It` fails to C-fail with `Couldn't reach the New Blitz server. Check your connection.`: Try Again or the 1999 game. D shows with the offline line. Nothing blocks. |
| **Wrong or bad ROM** | B's error text (section 3). No ROM stored. Four wrong tries in a row add a line: `The right file is NFL Blitz 2000 for the N64, USA, version 1.1.` |
| **Stored ROM from an earlier install** | B shows "Using your saved ROM" (name and date) with Use It / Change ROM. The launcher line says the same. Phones and handhelds: the same line on the launcher; on first launch with a valid stored ROM B is skipped and the line shows in the launcher. |
| **Android, no camera** | They never see the QR (it's on the computer path). On the handheld itself: it just asks for the ROM (B). The handheld's own browser can open `newblitz.com/download`: the web page also prints the address. On the computer path, "No camera? Make the file on this computer" is the answer. |
| **adb missing** (F) | `This computer doesn't have adb (Android's USB tool).` **Save the APK instead** (default) / **How to get adb** (opens the platform-tools download page, `device_send.cpp` already searches `ANDROID_HOME`, the SDK folders and the PATH) / **Try Again**. The APK path never needs adb. |
| **PS5 not found** (I) | `Couldn't reach the PS5 at <address>. Is etaHEN running, and is it on the same network?` **Try Again** / **Change Address** / **Back**. The address field keeps what was typed. The FTP port is 1337; a dead address times out in 8 s, not the OS default. |
| **iPhone free Apple ID** (H, G) | Said up front on G: 7 days, then re-sign; 3 free apps at a time; Developer Mode and trust steps; an app ID limit per week. **Open risk to test in 0.9.5:** whether the free account keeps the app's increased-memory entitlement (`platform/ios/BlitzRecomp.entitlements`). If it strips it, the game may not have the memory it needs on small phones; if so the tested wording must say so before 1.0. |
| **Phone/handheld, Android picker closed without a file** | Back on B with the same buttons. A skipped B leaves the launcher with Load ROM; `welcomed` stays false. |
| **Quit mid-download on a handheld** | Next start: C again (`setup_season_asked` false). |
| **ROM picked is a `.zip`** | Accepted when it holds the right dump (`.z64`, `.v64`, `.n64`). |

## 9. What exists today vs what's new

| Piece | Today | New | Files to change |
|---|---|---|---|
| Trigger | `builder_update()` polls; latch `checked`; Windows never opens | State-based trigger from the launcher hook, watchdog, no latch | `src/main/builder.cpp` (rename to `firstrun.cpp`), `src/main/main.cpp` (`on_launcher_update`, `update_gfx`) |
| **Finish setup** row | none | first launcher row while `!welcomed` | `src/main/launcher.cpp` |
| Step 0 "Where" | asked late (after the setup question), as two-button prompts | first screen, four choices | `builder.cpp` |
| Screens | `open_choice_prompt`, `open_info_prompt`, `open_keyboard`; no Back | `FirstRunScreen` (Back, step line, toggles, buttons, optional QR) | `src/main/ui_kit.cpp/.h`, new `src/main/firstrun_screen.cpp` |
| QR | none | QR drawn in the UI (a small QR encoder, no network) | new `src/main/qr.cpp` + `firstrun_screen.cpp` |
| ROM step | not part of Builder (Load ROM is a launcher row) | B, with the leftover-ROM confirm and wrong-ROM messages | `firstrun_screen.cpp`, `src/main/main.cpp` (stored-ROM import), `src/main/launcher.cpp` |
| ROM line in launcher | `RomNeededNote` shows "Load your ROM" or the new-player tip | "Using your saved ROM · Change ROM" with name and date, leftover wording | `launcher.cpp` (`RomNeededNote`, `RomNameLabel`) |
| 2026 question | exists (`AskGame`), follows with "keep rosters up to date?" | stays; follow-up moves into D; Try Again on failure | `builder.cpp`, `src/game/season_pak.cpp` (retry, 10 s cancel) |
| Updates and reports | rosters asked alone (`AskLive`), off unless you turn it on (README) | one screen, rosters ON, crash reports OFF | `builder.cpp`, `src/game/rosters_live.cpp` (`set_live_rosters`), `src/game/config.cpp` (`crash_reports`, `setup_*` options) |
| Crash upload | none in public; private Android only (`res/logs_hub`) | opt-in upload at next start (section 6.2) | `src/main/app_log.cpp`, new `src/main/crash_report.cpp`, the season Worker route (outside this repo's app), `tools/release_scan.sh` (allow the public address, keep the private one banned) |
| Logging | `[builder] done: ...` only | `[firstrun]` lines (section 7) | `builder.cpp`/`firstrun.cpp` |
| Phone / handheld: ROM first | `BlitzRomFirst.java` for about 1 GB RAM; others via the launcher | same; continues into C and D | `platform/android/java/.../BlitzRomFirst.java`, `firstrun.cpp` |
| iPhone: ROM | Files picker (`0016-ios.patch`), or copy via Finder | unchanged; plus ROM bundled in the `.ipa` | `platform/recompfrontend-patches/0016-ios.patch`, `src/main/main.cpp` (import from bundle), `src/main/device_send.cpp` (`save_iphone_ipa`: put ROM, `season.json`, pak, `builder_done` inside the bundle) |
| Android: send / save APK | `send_to_android()` (adb); no "Save the APK" | adds Save the APK with ROM and setup beside it, adb-missing path | `src/main/device_send.cpp` |
| PS5 | `send_to_ps5()`, address keyboard | prompt text, timeout, error text, J | `device_send.cpp` |
| N64 / emulator | `export_rom_dialog()` | reached from A; L "Here it is" with next steps | `src/game/export_screen.cpp`, `src/game/export_rom.cpp` |
| `builder_done` | skips Builder on a device | unchanged; D is still asked on the device | `src/game/config.cpp` |
| README | "First run" lists steps | rewrite to match | `README.md` |

## 10. Test plan for 0.9.5

Run each path from a **clean state**, and also from the **stale state** (the Windows 0.9.2 bug): the app folder kept with its ROM, `general.json` deleted.
Every `[firstrun]` line below is read from `blitz.log` in the app folder (its path is line 1).

**Clear to reset** (quit the app first; uninstalling the app does **not** delete the player's folder):
- **Mac:** `~/Library/Application Support/Blitz2026Recompiled`
- **Windows:** `%LOCALAPPDATA%\Blitz2026Recompiled`
- **Linux:** not in the 0.9.5 test; the folder is printed in line 1 (confirm before 1.0)
- **Android:** `adb shell pm clear com.blitzrecomp.newblitz` (clears the files folder with the ROM too); or Settings → Apps → Storage → Clear data
- **iPhone:** delete the app, then install again (removes its Documents)
- **Stale state only:** delete just `general.json` in the folder above and leave the rest (ROM stays)

| # | Platform and path | Do | Look for | Log lines that prove it |
|---|---|---|---|---|
| 1 | Mac, This computer (clean) | Open the app. A → This computer → Load ROM → Add It → defaults on D → Continue | Game starts. Launcher has no Finish setup after a restart | 1, 2 (`welcomed=0 rom=none`), 4, 5 (yes), 6 (`launcher-ready`), 10 for each screen, 8, 11 ok, 12, 16 |
| 2 | **Windows, stale state** | Delete `general.json` only; open | A appears on its own. If it doesn't within 3 s: line 7, then screen A comes up by itself | 2 (`welcomed=0 rom=valid`), 5, 6 (`launcher-ready` or `watchdog`), 10 asked `where` |
| 3 | Windows, clean | Same as 1; the Setup.exe's shortcut | Same as 1, on Windows | same as 1 |
| 4 | Windows, Finish setup | Back out at A; look at the launcher | **Finish setup** is the first row; it reopens A | 15 `abandoned`, 6 `finish-setup-row` |
| 5 | Mac or Windows, Phone → QR | A → Phone or handheld | QR and `newblitz.com/download` text shown; Done goes to the launcher with no Finish setup | 10 `qr`, 16 |
| 6 | Mac or Windows, "No camera?" → Android | → Load ROM → 2026 → Android → Save the APK (and Send over USB with a device) | Folder opens; steps are on screen; with USB the app is on the device and starts ready | 8, 10 `device`, 13 `android-file` / `android-usb` ok, 16 |
| 7 | Mac or Windows → iPhone | → iPhone → Save the .ipa; install with Sideloadly and a free Apple ID | `.ipa` installs; the iPhone app opens with the ROM and setup already in, no picker; asks D only | 13 `ipa` ok on the computer; on the iPhone 2 (`builder_done=1`), 10 `updates`, 16 |
| 8 | Computer → N64 | → Lite → save | `.z64` file named on screen with next steps; opens in an emulator | 14 ok, 10 `export-done`, 16 |
| 9 | Android, clean | Install the APK, open | B (picker) → C → D → game. No A | 1, 2, 4, 6, 8, 12, 16 (no `where`) |
| 10 | iPhone (free Apple ID) from the direct `.ipa` | Sideload the downloadable `.ipa`; open | Files picker → C → D → game. Note the 7-day and 3-app limits; whether the game has enough memory | same as 9 |
| 11 | Wrong ROM | Pick any other file on B | Error text; Try Another works | 9 |
| 12 | Offline | Wi-Fi off from B onward | C-fail: Try Again and Play the 1999 Game for Now both work; D shows the offline line | 11 failed, 10 `season-failed`, 12 |
| 13 | Quit mid-way | Quit at C, reopen | A again; B shows "Using your saved ROM"; no repeat of answered questions | 2 (`season_asked=0`), 6, 10 skipped lines |
| 14 | Crash report (if shipped) | D with reports on; force a crash (`--crash-test` or Task Manager on the game) and reopen | Toast `Crash report sent. Thanks.`; with reports off, nothing is sent | a `[firstrun]` D line `reports=on`, and a `[crash-report]` sent line |
| 15 | PS5 (not part of the first-run test) | Not tested in 0.9.5 | n/a | n/a |
| 16 | Linux AppImage | Smoke test only: first run starts, A shows | n/a | 1 to 6 |

Pass for 0.9.5: 1 to 13 pass on Mac, Windows, Android and iPhone as marked, and **line 5 and 6 appear on every platform** (a missing
line is itself the finding).

## 11. Open questions for the team

1. **Rosters ON by default** means the app contacts the season server at every start, with the player's IP visible. README says optional/off today. OK to default it on (the spec does), shown on D with the explanation?
2. **ROM inside the `.ipa`:** a change from today (ROM beside it). It means the file holds the player's ROM (warned on screen). OK, or keep ROM beside it and copy it over?
3. **Crash reports in 0.9.5, or later?** They need a new public Worker route and a settings row. Ship the setting and screen now with the upload following (off, honest text), or hold the row until the upload exists?
4. **The QR path on the computer:** should "Done" on the QR screen count the computer as set up, so it never asks again? The spec says yes (the computer can still load a ROM later from the launcher).
5. **Customize** no longer appears in first run (Settings holds the rules). OK? Blitz Builder keeps a Customize pass for sending a customized setup.
6. **Free Apple ID memory risk** (iPhone): if the game can't get enough memory with the free account's entitlements, do we say it on the page, or require a paid account for 1.0?
7. **Linux AppImage** isn't in the 0.9.5 test list. Ship it untested, or hold it back from the page until tested?

## 12. The team's answers (2026-11-15): these override anything above

1. **Roster updates:** an optional checkbox on the updates screen, **ticked by default**. The README's "off unless you
   turn it on" changes to match.
2. **ROM inside the `.ipa`:** yes when the iPhone app is made on a computer (the "No camera? Make the file here"
   path). An `.ipa` downloaded on the phone (SideStore can install one straight from Safari's downloads) has no ROM,
   so the app still asks for it with the Files picker on first launch. Both routes must work.
3. **Crash reports ship in 0.9.5:** off by default (opt-in checkbox). When on, they're sent **silently**: no prompt
   after a crash. In Settings → General, next to Save Logs, a line or button lets people **see exactly what gets
   sent** (the last report, or an example of one if none was sent).
4. **The QR screen's Done:** marks the computer as set up and goes back to the app's main screen.
5. **Recommended / Customize stays in first run** (team, after seeing it on Windows: "a nice touch"). This replaces
   the earlier "drop Customize": keep today's screen, with Recommended as the default.
6. **Free Apple ID:** undecided until it's tested. That's the next test after Windows, so keep the iPhone wording
   easy to change.
7. **Linux AppImage:** ships, labelled untested.
8. **No launcher at every start.** Once setup is done, the app starts straight into the game. The launcher's items
   (Change ROM, Add-Ons, Blitz Builder, the 1999/New Blitz switch) live in Settings, where players go when they want
   them. The first run, and "Finish setup" while setup isn't done, are the only times the setup screens show by
   themselves.
9. **The hub model** (later on 2026-11-15): "The computer app is the hub: it pushes the goods (game, ROM, setup) to the
   device. Devices skip Load ROM and the file picker and go straight to updates and reports." Blitz Builder is never
   shown on Android, iOS or the PS5 (Settings → General and the launcher). A device set up from a computer asks
   Updates and reports only (§2.2). A phone with no ROM (installed by hand) shows one screen for B, "Set up New Blitz
   from your computer" (Mac, Windows or Linux → Blitz Builder → Phone or Handheld), with **Pick a File Instead** below
   (Android's BlitzRomFirst.java says the same; an iPhone also names the Files app's Share route).
10. **Reset Data** (Settings → General, near the bottom; `src/main/reset_data.cpp`): "Reset Data?" with Keep my ROM
   (ticked); Back Up and Reset (default), Reset, Cancel. Backups: Documents/New Blitz Backups on a computer, backups/
   in the app folder on a phone or the PS5. Then the first run again (a computer restarts; a phone closes). **Restore a
   Backup** shows once a backup exists. `[reset]` lines.
