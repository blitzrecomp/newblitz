# Playing New Blitz

This guide is for people who have never set up an emulator, and for anyone who wants NFL Blitz 2000 running well on a Mac. It covers the first launch, controllers, the TV, and the settings worth knowing. Building the apps yourself is covered in the README.

## What you need

- **A Mac with Apple Silicon** (M1 or later).
- **The app:**
  - **Classic Blitz** is the original 2000 game.
  - **Modern Blitz** has 32 teams and current rosters, on the same USA v1.1 cartridge as Classic: load the ROM once and both are there (the launcher switches between them).
  - **Modern Blitz for the Team Extension** is a separate build that plays the Team Extension version of the game, or a community update built on it (such as Resurrection's Gold or Silver Edition). When you load a community update (or switch with **Change ROM** on the launcher), it asks whose rosters to play: **Blitz 2026** puts this app's current rosters on its art and uniforms, **Its Own** keeps its teams. Settings → Season → Rosters changes it later.
- **Your own copy of the game, as a ROM file.** The app doesn't include the game. You make a ROM from your own cartridge with a cartridge reader, a small USB device that copies the cartridge to a file. Classic Blitz needs the USA v1.1 cartridge, and the app checks the file is exactly that.

## First launch

1. **Open the app.** macOS may say it "can't be opened" because it isn't from the App Store. Open **System Settings → Privacy & Security**, scroll down, and click **Open Anyway** next to the app's name. You only do this once.
2. **Load ROM.** Choose your ROM file: .z64, .v64 and .n64 all work, and so does a .zip holding one. The app checks it and keeps its own copy, so you can move or delete the original afterwards. If it isn't the right file, the app says what's wrong with it (for example, that it isn't the USA v1.1 cartridge, or that the file is damaged). On a phone or the PS5, put the ROM in the app's folder (the Files app, a file manager, or `/data/blitzrecomp` over FTP) and open the app.
3. **Blitz Builder** asks three things, once:
   - **Which Blitz?** **New Blitz** (today's 32 teams, rosters and uniforms) or **Original 1999** (NFL Blitz 2000 exactly as it shipped). Change it later in Settings → Season → Game.
   - **Set it up for you?** **Recommended** plays straight away; **Customize** opens Settings on the house rules (Receiver Buttons, the play clock, realism and the rest are all off until you turn them on).
   - **Where do you want to play?** (on a computer) **Here**, or **Somewhere else**: a **PS5** (etaHEN running; type its address), an **iPhone** (connected or paired with Xcode on this Mac) or an **Android** device (USB debugging on, adb), sent with your ROM and your Season and Rules settings; or **N64 or Emulator** for a ROM with your setup built in (Export ROM). Devices need an app built with the device kits (`make device-kits app-modern`). The iPhone app isn't among them unless the Mac app was built with `DEVICE_KITS_IOS=1` (it's signed for one developer's team): otherwise build and install it with your own Xcode, `make ios-modern` then `tools/ios/install.sh modern` (set `BLITZ_IOS_TEAM` to your team id), and Blitz Builder says so when you pick iPhone.
4. From then on a computer opens on the launcher with **Play** ready, and a phone or the PS5 goes straight into the game. **Settings → General → Launcher at Start** changes that either way (it was Skip Launcher). The launcher's **Blitz Builder** asks again, say to make a ROM for an N64.

**Hints.** The first time you reach each part of the game (the menus, team select, play select, offense, defense), a small TIP card shows what to press, in your own controller's buttons. Each shows once and never blocks a button. **Settings → General → Hints** turns them off; **Show Hints Again** brings them back.

Settings and Controller Pak saves are kept in your Library folder (`~/Library/Application Support/BlitzRecompiled`, or `Blitz2026Recompiled` for Modern Blitz). To keep everything next to the app instead, for example on a USB stick, put an empty file named `portable.txt` beside it.

## Controllers

- **PlayStation controllers (DualSense, DualShock 4)** work over USB or Bluetooth. The Switch Pro Controller works too. Xbox controllers go through the same controller support, but haven't been tested yet. To pair over Bluetooth, put the controller in pairing mode, then add it in **System Settings → Bluetooth**.
  - DualSense: hold Create and PS until the light bar flashes.
  - Xbox: hold the pair button on top.
  - Switch Pro: hold the sync button on top.
- **Each controller becomes the next free player (1-4).** The light bar shows which player you are: blue 1, red 2, green 3, yellow 4. **Settings → Players** shows who is who: **Identify** buzzes a controller, **Swap** trades two players' places, and **Make Player 1** swaps a controller into player 1, who drives the game's menus.
- **Player 1 can also use the keyboard:** W A S D to move, Space to pass, Shift to jump, Q for turbo, Return for Start.
- **In the game's own screens, each player sees their own controller's buttons:** PlayStation, Xbox or Switch. Any other controller, or an empty spot, shows PlayStation. The play-select help shows each player's column in their own buttons, and Controller Setup shows the controller you're setting up. Menu footers follow whoever pressed a button last.
- **The app's menus (launcher, settings, prompts) follow player 1's controller** (PlayStation or Xbox letters). **Settings → General → Button Icons** can pick one style for everyone instead: PlayStation, Xbox, Switch or N64 (the game's original icons). This setting was reset to Each Player's Controller in the update that added it.
- **In the app's menus:** ✕ selects and ○ goes back (□ works too), L1 / R1 change tabs, and Create (Xbox: View) opens or closes settings. On a keyboard: Enter, Backspace, Page Up / Page Down and Esc.
- **Nintendo controllers use the button positions, not the letters:** on a Switch Pro the bottom button (B) passes and the left one (Y) tackles, as on every other controller. To go by the letters instead, remap in Settings → Controls.

Default buttons: pass/change player = Cross (A), jump/tackle = Square (X), turbo = L2 or R2. Remap them in **Settings → Controls**.

## Playing with friends

For a party: **Settings → General → TV Mode** (full screen with a widescreen picture), **Settings → Players** for phone controllers (everyone scans the QR code), and **Rules → Quick Setup → Party** (infinite and faster turbo, late hits, a big football, field goal odds).

### Tournaments

**Settings → Bracket** sets up a bracket for 2 to 8 people:
1. Under **Players**, type the names separated by commas (up to 9 letters each), e.g. `SAM, ALEX, JO, PAT`. Teams are picked before each game, so these are the people.
2. Choose **Each Round** (one game, or best of 3) and **Matchups** (random, or as typed: 1 v 2, 3 v 4, ...).
3. Press **Create**.
4. Close Settings, open **Tournament** from the game's main menu, and choose **PLAY**. The bracket is already there, with no name entry.

Party tournaments play the shortest quarters. The bracket is saved after every game (`tournament.bin` in the app folder) and comes back after a restart. **Show** (Show Standings) shows each player's record and points, and the champion. A tied game is played again.

## Phones as controllers

No controller for everyone? **Settings → Players → Phone Controllers → Turn on**. Phones on the same Wi-Fi scan the QR code shown there (or open the address under it), turn sideways, and join as the next free player: stick on the left half, Pass, Jump and Turbo on the right, Start at the top. Up to four phones. macOS may ask once whether the app may accept incoming connections: allow it. Turn it off when you're done, since anyone on the network can join while it's on.

- **The buttons are labelled for what's on screen:** menus, play select, and offense or defense during a play.
- **The ··· menu** at the top has save, load and the save slot, Redo Play, and **Be player 1**.
- **A phone keeps its player** if it locks or loses Wi-Fi for a moment (up to 10 seconds), and the app's notes ("Slot 1: state saved") show on it too.
- An open page updates itself after the app is updated.

## On a TV

- **Connect** with an HDMI cable, or mirror to an Apple TV with AirPlay (Control Center → Screen Mirroring).
- **Settings → General → TV Mode** switches to full screen with a widescreen picture in one go.
- **Without TV Mode,** **L3 + R3** (both sticks pressed in) toggles full screen from the controller, and **F11** does it from the keyboard.
- **Pair everyone's controllers first,** then hand them out. Settings → Players shows who is who.

## Settings worth knowing

| Where | Setting | What it does |
|---|---|---|
| General | Skip Intro Screens | Gets you to Start in a few seconds |
| General | Skip Rumble Pak Screen | Goes straight from controller select to team select |
| General | Player 1 Rumble | Player 1's controller rumbles on hits (default on) |
| General | Resume Interrupted Games (Experimental) | Off by default. Quitting mid-game asks whether to save the game, and after a crash the app offers to recover it; the next game between the same teams picks up from there |
| Rules | Choose CPU Opponent | Lets you pick who you play against in a 1-player game (the opponent list is marked 3, the other team's first player) |
| Rules | Uniform Picker | On by default. At team select, △ (Y on Xbox; UNIFORM on a phone) cycles your team's uniform: Home, Away, or back to the game's choice; the player re-dresses and a toast says which. While choosing your CPU opponent it picks theirs. The other team adapts so the two don't clash |
| Rules | Receiver Buttons | Off by default (Blitz Builder's Customize can turn it on). When your quarterback has the ball, a button icon floats over each receiver (left to right △, R1, ○; Y, RB, B on Xbox) and follows him on his route. Press one to throw to that receiver. Before the snap, hold L2 (Turbo) to see the icons. On a phone, small ← ↓ → buttons wrap around A while your quarterback has the ball, and the icons show the same arrows. ✕ still passes the usual way, and the right stick still works as the C buttons. The icons stay hidden on defense and when the CPU is at quarterback. Off puts the C buttons back as they were. On two screens, DS Screens' Stylus has them on for player 1 whatever this says |
| Rules | Pick Your Crew (Modern Blitz) | On by default. After team select, pick who plays for your team: a QB, RB, WR and a FLEX from the skill players left, then **Pick Line** (three linemen), **Pick Defense** (three linemen, four behind them) and **Pick CPU** (their team the same way); ○ goes back a page, **Go Play** starts. Looks only: names, numbers, bodies and shades, not ratings. Picks are remembered per team; **This Week's** puts a page back to the Lineups setting's players |
| Team select | Pro Bowl (Modern Blitz) | At team select press the game's **R** (R2 on most pads; the tip card draws yours): it picks the hovered team's stadium with the Pro Bowl star as your logo (the QB steps away, and the list shows your team code) and moves on. On Choose Opponent, R again makes the CPU a Pro Bowl crew. After team select: your **team code** (three letters, the scoreboard's; the first time, R asks for one on the keyboard; **Type One…** changes it), **uniforms** (2026, 1999 or Random: each player in his own team's, one crew in colour and the other in white), and your crew from every team's starter at each spot (Offense, Line, Defense; **Shuffle** deals a page again). The CPU's crew is dealt at random each game; **Pick CPU** changes it. In the game: a VS card, the stars at play select, codes on the scoreboard, quarter banners and line scores |
| Everywhere | On-screen keyboard | Any text box (Settings → Bracket's Player Names, a Pro Bowl team code): press ✕ / A on it with a controller for a QWERTY keyboard (Shift, Space, Delete, Done; ○ cancels). A keyboard still types straight in |
| Rules | Quick Setup | Original, Party or Tournament codes in one pick, plus any shared rules files (see below) |
| Rules | The Blitz codes | Big Heads, Tiny Players, Infinite Turbo, weather, stadium: on for every game, no code entry needed |
| Rules | Home Field | The home team plays on its real stadium's grass or turf, and a roofed stadium gets no rain or snow (on in Modern) |
| Rules | Play Clock | Normal (9 seconds in the 1st quarter, 5 after), 15 s or 20 s every quarter, when the game's Play Timer is on |
| Rules | Realism | NFL-style rules, each defaulting to the original game: **First Down** 30 / 20 / 10 yards, **Kickoff Touchback** at the 20 / 25 / 35, **Two-Point Try** from the 10 / the 2, **Extra Points Can Miss** (about 94% good), **Overtime** up to 3 / one, then a tie. Fixed when a game starts; Season always plays Blitz's own rules |
| Rules | Smart CPU Opponent | The game's own Smart CPU code, on for every 1-player game: the CPU plays at its top skill. Tougher, not smarter |
| Rules | Smarter Play Calling | Off by default. The CPU kicks the field goal at the end of a half (13 seconds or less, in range), doesn't try field goals over 55 yards, fakes far less, and goes for two only from the 4th quarter on |
| Season | Tiebreakers | The original rules, or the NFL's |
| Season | Rosters (Modern Blitz) | Auto, Blitz 2026, the ROM's own, or another installed pack. **Blitz: Pac-12** plays the eight 2026 Pac-12 schools in Arcade (Season is off for it) |
| Season | Lineups (Modern Blitz) | **This Week** (default): who's starting this week, injuries and all; new lineups are made every morning in season (6:17 a.m. Pacific) and download when the app starts. In an Advanced season each week plays that NFL week's lineup. **Full Strength**: everyone healthy. **Built-In**: the rosters built in. **Week N**: keeps a week's lineup after newer ones arrive |
| Season | Season Mode (Modern Blitz) | Advanced: all 32 teams, the real 2026 schedule and 7-team playoffs |
| Season | Roster Room (Modern Blitz) | **Open** serves a page on this computer only and opens it in your browser: every team's slots, with this week's player in each and anyone on the roster to put there instead. **Lock In** saves your crews and closes the room. They play over the weekly lineup from the next team select, in every mode, and go into an exported ROM |
| Export | Kind of ROM | The launcher's Export ROM screen offers the same. **Lite: Arcade**: all 32 teams and every uniform, the game's own Season and Tournament, any N64 or emulator. **Lite: Season**: the 2026 season (8 divisions, 18 weeks) with each team's 2026 home and away, on any N64. **Full** (default): every uniform and the 2026 season, which needs an Expansion Pak on a real N64. **Custom**: changing any option below (32 Teams, 2026 Season, Every Uniform, the extras) makes it Custom. The Lites leave Choose CPU Opponent off. Classic has **Classic** and Custom. The file name says which kind it is |
| Export | Rosters (Modern) | **Current**: this week's lineups. **2026 Season**: every team at full strength. **Custom**: this week's with your Roster Room crews (picking it on the Export screen opens the Roster Room) |
| Export | Export ROM | Saves a cart-ready .z64 to Downloads, a new file each time (the date and time in its name, so an emulator holding the last one open doesn't matter; an emulator keeps saves per file name, so a new export starts fresh saves), for a real N64 or a flash cart. It always carries your rosters (Roster Room crews included), skin shades, art and the 2026 home and away uniforms. Optional, each from your current settings: **Uniform Picker** (Modern; C-left at team select steps through the team's sets), **Skip Rumble Pak Screen** (on by default), **Choose CPU Opponent** (in a 1-player game the other side's list opens for you once you've picked, under CHOOSE OPPONENT), **Play Clock**, **Blitz Codes**, **Home Field** and **Realism** (on a cart, Season too). When it's done it lists what went in |

The ↻ button at the top of Settings restarts the game without quitting the app.

## Sharing your rules

Rules → **Save Setup to File** saves the current codes as a file in the app folder's `rules` folder and opens the folder. Rename the file (that becomes its name in Quick Setup) and send it to a friend; they drop it in their own `rules` folder (in Modern Blitz, launcher → Custom Content opens the app folder) and it appears in Quick Setup at the next start.

## Screenshots and clips

- **F12** (or a controller's capture button: Xbox Share, Switch Capture, the DualSense mic button) saves a screenshot of the game window.
- **Shift+F12** starts recording a clip, and pressing it again stops it.

Both go to **Pictures → New Blitz**. The first time, macOS asks to allow screen recording for the app.

On Android the **hotkey + Select** (R3 held, or L3 on a one-stick handheld) saves a screenshot of both screens, the upper one on top, in **Pictures →
New Blitz**, where the gallery finds it; saved replays go there too. **Select** opens the menu, and so does a press of the
device's own **Back** (an RG DS's side button, or the back gesture). Holding Back quits: after 2 seconds a note says to hold 3
more, and at 5 the app closes, without asking to save. On GammaOS, though, the system takes a long Back itself: about half a
second in, it closes the app and goes back to its menu, before the app hears of it.

## Save states (experimental)

- **F5** (or on a controller, hold the hotkey, **R3** (the right stick pressed in; L3 too on one-stick handhelds), and press **R1**) saves the game exactly as it is, mid-play included (on an RG DS, whose L1/R1 are turbo, it's **R2**, and **L2** loads; only a press made after the stick is in counts, so holding turbo and then pushing a stick in hard does nothing). **F7** (or **hotkey + L1**) loads it back, in this session or after quitting and reopening the app. A small note at the bottom right of the screen says "Slot 1: state saved" or "Slot 1: state loaded" and fades after a moment.
- **Quitting during a game** (Cmd+Q, the window's close button or Settings → Quit) asks once, **Quit Blitz?**, so a window closed by accident doesn't end a game. **Quit** saves the game as it is and closes the app (**Keep Playing** goes back to it). The next time the app opens, a note at the bottom right (at the top on a phone) says **Pick up your last game?** with the score: press **△** (Y on Xbox, X on Switch; the button the note shows), or click or tap the note, and you're straight back in. Leave it and it goes after about 12 seconds, or as soon as you press **□** or **Start** or pick something in the menus. It's offered once, on the first start after quitting: a game you don't pick up then isn't offered again (the file stays in `savestates` as `quit.declined.state`). This saved game is separate from the three slots and is used up when you resume it. If the game can't be saved at that moment, the app says so and offers **Quit Anyway** (the game is lost) or **Keep Playing**.
- **The hotkey's other buttons:** **□ / ○** (or **D-pad left / right**) pick the slot, **△** (or **D-pad up**) is Redo Play,
  **Select** takes a screenshot, and on two screens **Start** switches them. While it's held the game gets none of these.
- There are three slots, and a fourth, **Auto**, which the app fills itself when it goes to the background in the middle of a
  game (a clamshell closed, another app opened on a phone). **F6** (**Shift+F6** goes back, or **hotkey + ○ / □**) picks one, and the note shows whether it's empty or when it was saved. Saving replaces what's in the current slot. Settings → General → Save State Buttons turns the controller buttons off.
- A state only loads in the same version of the app that saved it. After an update, save a new one.
- Loading puts everything the game keeps in memory back as it was, including its copy of your records and season. If the game saves to the Controller Pak after you load an old state, it saves that older copy.
- At a few moments a state can't be taken, for example while the game is talking to the Controller Pak. The app waits up to ten seconds, then says so in a note.

## Redo Play (experimental)

Botched a kickoff, or want a do-over at a party? Hold the hotkey, **R3** (the right stick pressed in; L3 too on one-stick handhelds), and press **△** or **D-pad up** (or **F8**) to run the last play again.

- During a play, or while its result is on screen, it goes back to that play's play select.
- At play select it goes back one play more, and each press goes back another, up to 8 plays. Potato Mode keeps the last play only, and the menus have no step-back there: their snapshots are too much for a 1 GB handheld.
- Everything rewinds with it: score, clock, down and the stats.
- Each new game starts fresh. Settings → General → Redo Play turns it off.
- **In the menus before a game** it goes back a step instead: picked the wrong team, or Arcade when you meant Season?
  Each press undoes one confirm (✕), back to the main menu if need be, with the cursor where it was. On a phone it's
  the UNDO button at the top right. The game's own □ BACK still works where it did, and at team select (where the game took □ as a pick) □ goes back a step too.

## If something goes wrong

- **The app closed by itself:** Settings → General → **Save Logs** zips `blitz.log` (what happened), the run before's, any crash reports and your settings into one file: in Downloads on a computer, in the app's folder on a phone (the Files app), in `/data/blitzrecomp` on the PS5. Please send it along with what you were doing.
- **Something looks wrong on screen (flicker, garbage, a frozen picture):** press **Ctrl+Shift+F12** while it's happening. It saves three frames and a memory snapshot in the app folder's `dumps` folder; send them with `blitz.log`.
- **No sound:** check Settings → Sound. The app also picks up a new output device (for example headphones) by itself.
- **A controller doesn't respond:** check Settings → Players. If it shows there but nothing happens, press Identify to find which player it is.
