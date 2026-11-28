# Changelog

## 1.0.6 (unreleased)

### New paks: two leagues and new NFL uniforms

- **CFL 2026** and **Pac-12 2026**, two league paks: the nine CFL clubs and the eight schools of the 2026 Pac-12, with
  their real 2026 rosters (healthy starters), logos, helmets, end zones and every home, away and alternate uniform.
  They play Arcade and Tournament. Get them in Settings → Add-Ons (they also arrive by themselves with updates on),
  then choose one under Settings → Teams → **League**.
- **The 2026 Season's NFL uniforms, redone.** All 32 teams' home, away and alternate sets (116 choices) were redrawn
  and checked on the field, with new matchup-screen helmets (Pittsburgh's Steelmark on its own side, Carolina,
  Washington) and corrected end zones (Arizona, Indianapolis, the Giants, the Jets, Tennessee). It's a 2026 Season pak
  update, so it reaches every version of New Blitz, 1.0.0 on.
- **Shoulders the old model couldn't paint.** A pak can now give a uniform its own shoulder mapping and a different
  right sleeve, so Colorado State's and Utah State's shoulder stripes run unbroken and the CFL's two-sided sleeves show
  as drawn.

### Your cartridge

- **The v1.0 cartridge works.** NFL Blitz 2000 (USA) came out twice, v1.0 and v1.1. New Blitz plays v1.1, and until now
  a v1.0 dump was turned away. The two are the same game with its code moved around, so New Blitz now updates a v1.0
  dump to v1.1 as it goes in, checks the result is exactly v1.1, and tells you it did ("That's the v1.0 cartridge: New
  Blitz updated it to v1.1"). Your own file isn't changed. It works wherever a ROM comes in: Load ROM, Blitz Builder,
  Change ROM, a phone or handheld's file picker or app folder, the PS5's folder, and other launchers. The update is a
  38 KB patch of v1.1's code that ships with the app; it holds no game art or sound.
- **Another launcher's ROM that doesn't work says so on screen.** When a launcher starts New Blitz with a file
  (`--import-rom`) that isn't the game, or is a damaged dump, New Blitz now stays on its own launcher and tells you why,
  instead of quietly playing the ROM it had before.

### When the game stops

- **The freeze rescue covers the breaks between quarters.** If the game stopped responding at the end of a quarter or
  at half time (seen in Original 1999), nothing happened: no message and nothing to pick up. Now New Blitz keeps the
  last play there too, as it does mid-play, and offers to pick up from it next time. When there's truly nothing to
  keep (Redo Play off), it still tells you the game stopped and to open New Blitz again, and Quit no longer waits on the
  stuck game.

### Add-Ons

- **Paks load by themselves.** Every add-on pak New Blitz posts (the 2026 Season now, the league paks as they come)
  installs and updates itself when the app starts, under Settings → Teams → **Keep Rosters and Uniforms Up to Date**,
  which is on by default. Turn it off and nothing downloads by itself. What arrives during a game goes in the next time
  New Blitz starts. On mobile data, big downloads wait for Wi-Fi. If you had it off, New Blitz asks you once after
  updating: "Keep rosters, uniforms and new paks up to date?"
- **Check**, at the top right of Add-Ons, looks for new paks right now and downloads them with a progress bar. When
  there's nothing new it says "Up to date."
- Paks you added yourself (Add…) are never replaced or updated behind your back, and a pak you remove stays removed
  until you press **Get** on its row.
- **Open Folder works on phones.** On an iPhone it opens New Blitz's folder in the Files app (On My iPhone → New
  Blitz). On Android it opens the Files app at New Blitz's folder, which now also appears in the Files app's menu and
  in file pickers as "New Blitz". If no app can open it, Add-Ons says where it is.
- **Android: leaving the file picker.** Back or Cancel in the system file picker (Add…, Load ROM, Change ROM, Import
  Saves) now returns you to the screen you were on, and the game keeps running behind the picker.

### Leagues

- **Settings → Teams → League** picks whose league you play: **NFL 2026** or any league pak you've added, like the
  CFL's. It's one row with ◀ ▶ arrows, so every pak fits, whatever the screen (before, the fifth one was cut off).
  A league pak plays Arcade and Tournament; Season, Advanced and the Play Editor are the NFL's, and the main menu greys
  them out. **Rosters** and **Lineups** are the NFL's, so they hide while a league pak is chosen.
- **A pak you add shows up straight away.** Add-Ons (Add…, Check, or the automatic downloads) puts it in the League list
  without relaunching the app, and asks **"Play CFL 2026 now?"** (Yes / Not Now), on the launcher or in Settings, never
  over a game. A pak removed while it's your league puts League back to NFL 2026 and says so.
- **Look, then choose.** ◀ ▶ only look through the leagues (the name dims, with "Press × to choose"); **×** (or Enter,
  or a tap on the name) chooses. Leaving with a league looked at but not chosen asks **"CFL 2026 isn't chosen yet."**
  with **Restart to Switch** or **Keep NFL 2026**.
- **Changing a league no longer does nothing until a restart.** During a game, choosing a league asks **"Switch to CFL
  2026? New Blitz restarts to load its teams."** with **Restart Now** or **Later** (it waits for the next start). Game,
  Rosters and Season Mode, also read as the game starts, ask the same with **Restart** and **Cancel** (Cancel puts it
  back). Lineups needs no restart: every game reads it.
  While a change waits for a restart, the ↻ Restart button at the top of Settings shows a dot, and its prompt says what
  waits.
- **League messages are New Blitz's own.** "CFL 2026 plays Arcade and Tournament. To play a Season, switch League to NFL
  2026 in Settings → Teams" is an in-game box now (with a **Switch to NFL 2026** button), not a system dialog, and the
  same goes for a content pack or pak that didn't load.
- Your settings carry over: Rosters set to a pak becomes League = that pak with Rosters on Auto, including settings
  that come in through Cloud Sync.
- **Switching leagues with Restart starts clean.** After a restart from one league to another (the CFL to the Pac-12),
  the Uniform Picker no longer offers the last league's uniforms to the new league's teams.
- **A league pak that can't load adds nothing.** Before, a rejected pak's images could still go into the game (the
  NFL team select showed another league's logo). Now it contributes nothing, you get a message ("CFL 2026 couldn't be
  loaded: ... Playing NFL 2026."), and the NFL plays as usual. Lineups' "Current (Week 12)" also fills in once a lineup
  or the 2026 Season pak arrives while the app is running.

### Lineups, simpler

- **Lineups** is **Current (Week 12)** or **All Starters (Healthy)**. Current is the freshest lineup New Blitz has,
  labelled with its week; All Starters is the old Full Strength. Built-In and the pinned **Week N** are gone, and a
  saved Built-In or Week N becomes Current. An Advanced season still plays each week with that week's lineup.

### Seasons

- **"Standard or Advanced season?"** When you start a new season, New Blitz asks which one: **Standard** is the classic
  Blitz season; **Advanced** is the real 2026 schedule in today's eight divisions, with weekly lineups that follow the
  real season, NFL tiebreakers and 7-team playoffs. It starts from your Season Mode setting and saves your answer
  there. Picking the other one restarts the game into it (each keeps its own saves), and that next new season isn't
  asked again. Never asked in the middle of a season.

### On the field

- **Play Art looks like Blitz now.** The routes are drawn the way the playbook's own play cards draw them: fat lines
  with a shaded edge, a black drop shadow and broad barbed arrowheads, still in your Receiver Buttons' colours (made
  as bold as the cards' red). They stand out on snow and at night, where the old see-through ribbons faded.

### Menus

- **Back at Controller Select no longer locks the confirm button.** After leaving a game, pressing Back there could
  leave the confirm button doing nothing until another button was pressed (a controller reporting a button still held).
  Confirm now always goes through. Back steps through the menus one screen at a time, and the menu history it keeps
  takes an eighth of the memory it did.
- **START is in one place on every controller.** The touch controls, two screens' Controller mode and the phone
  controller page all put START, a small ▶, at the bottom centre (Controller mode had it at the top, the phone page in
  its top bar). Controller mode's PASS, JUMP and TURBO no longer look cut off at the top and bottom: their shine and
  lip follow the button's edge instead of running straight across it.
- **The Uniform Picker no longer repeats a uniform.** At team select a side starts on Auto, which says which set the game
  will use ("home uniform · Auto"). Pressing the uniform button steps through home, away and the team's alternates, each
  once, starting after Auto's set; "game default" is no longer a stop. □ takes a side with a pick back to Auto (a second □
  goes back a screen as before). The game's jersey-clash swap still applies to Auto.

### Restart on every device

- **Restart is back on phones**, at the top of Settings and in Add-Ons, and it no longer closes the app: the game
  starts over inside New Blitz in a moment, like pressing an N64's reset button, with your settings read again. Handy
  after changing Game, Season Mode or a roster pak, which wait for the next start.
- **Main Menu** on a phone now goes straight to the launcher instead of closing the app, and a restored Controller
  Pak or an imported save goes in without reopening New Blitz.
- **Reset Data** on Android starts the app again by itself. An iPhone can't reopen an app, so it still closes and
  tells you to open it again.
- On Android, Restart after changing Potato Mode, Swap Screens or the two-screen Potato settings starts the whole app
  again, since those are set as it opens.

## 1.0.5 (2026-11-27)

### Your saves on every device

- **Cloud Sync.** Play a season on your phone and pick it up on your handheld or computer. Settings → **Saves** →
  Cloud Sync: turn it on, then on your other device choose **Join** and type the **6-digit number** the first one
  shows (it works once, for 2 minutes). From then on your saves go up after each save the game makes and come down
  when New Blitz starts. It syncs your Controller Pak and season saves, not save states (those only load in the build
  and device that made them).
- **Your saves are locked before they leave the device.** They're encrypted with a key made from your sync code, so
  our server can't read them. No account, no name, no email. Copies are deleted after 90 days unused, or when you turn
  Cloud Sync off and delete. The privacy page has the details.
- **Nothing is replaced behind your back.** A save that doesn't carry on from yours is never taken or sent silently:
  New Blitz shows both (team, week, when, which device) and asks. Whatever is replaced goes to Backups first.
- **Cloud Save Slots.** Three slots under one code (Slot 1-3), for separate seasons or a spare.
- **Sync over Wi-Fi** sends your saves straight to another device on the same network, and **Export / Import Save
  File** makes a file you can move yourself (Drive, AirDrop, a USB stick). On a PS5, Cloud Sync is the way.
- New Blitz asks once when you set it up, and after a few updates, whether you play on more than one device. **No**
  and **Don't Ask Again** are always there.

### Two screens

- **Surface Duo.** Spanned, the game is a two-screen DS: the field across both, play select and your thumbs below.
  It opens spanned by itself, and turns into the DS hold when it's held like a book. On one panel it's landscape,
  leaving the other panel for another app (held upright, it reminds you to turn it).
- **Wide upper screens** (Retroid Pocket Duo, AYN Thor): the top screen fills with field instead of black bars, and
  the 4:3 lower screen shows the same view at the same scale. Tested on a Mac at their sizes; tell us how it plays on
  the real thing.
- **Settings → DS**, a tab of its own on two-screen devices: the modes (Both, Companion, Stylus, Controller), the
  camera, and a **Custom Layout** with a slider for every piece (the score panel, the big messages, the tip cards, the
  camera's tilt and zoom...). The drive board's new **Screens** button steps through the modes by touch.
- **Controller mode** turns the lower screen into a touch controller in the phone controller's style.
- **The upper screen's pieces sit where they belong:** 1ST AND 30 inside its plate at the bottom; TOUCHDOWN! and the
  score panel clear of each other and of the hinge, with no jump; FUMBLE in its box; play results whole; the punt and
  kick meters back; the pause menu centred; tips dismissed by a touch on either screen.
- **The drive board:** square team logos, end zones in team colours, and how each drive ended (TD, FG, PUNT, INT,
  FUMBLE, DOWNS, SAFETY, END HALF). The receiver icons show over the game in every mode, never over the board.

### On the field

- **Play Art.** Hold Turbo at the line to see your receivers' routes painted on the turf, Madden style, in their
  Receiver Buttons' colours. Settings → Rules → Play Art (Off, First Break, Full).
- **Jersey numbers read the right way.** Players farther from the camera wore their numbers backwards (the original
  game's own slip); fixed in every mode.
- **Both stadium end boards read the right way** in the Day, Night and Snow stadiums.
- **Players stay drawn to the edges of a widescreen picture**, shadows and all.

### Controllers and menus

- **Settings, tidied.** General keeps the app-wide things; graphics settings moved to Graphics, the controller's to the
  top of Controls, game options to Rules, the bracket to Teams, and a new **Saves** tab has save states, Sync Now,
  Backups and Reset Data. Nothing you set was reset. Opening Settings in a game holds it still underneath.
- **A controller's buttons, your way.** Settings → Players → **Buttons** on each player: Auto, PlayStation, Xbox or
  Switch. A PlayStation-style pad that says it's a Switch controller (some do, on Android) can be told: the bottom
  button then hikes and OKs, in the game and the menus. Button Icons set to PlayStation does it for any such pad.
- **START is a ▶** on the touch and phone controllers.
- **Instant Replay's buttons fit** on phones and handhelds.

### Fixes

- **Loading a save state from before your last season save asks first** (Load It or Cancel), instead of saying so
  afterwards with only OK.
- **Android: no "isn't responding" box after loading a save state.**
- **The on-screen keyboard** no longer opens twice or hides behind the screen that asked for it.

## 1.0.4 (2026-11-26)

- **Phones: no more pink screens after the app switcher.** Coming back to New Blitz could leave the game, and then
  every menu, bright pink. The app now stops all drawing while it's in the background and reloads the menus' pictures
  when it's back.
- **No false "The game stopped responding".** Time spent in the app switcher or on the Home Screen counted as a frozen
  game. It no longer does; a game that's really stuck is still rescued.
- **Instant Replay shows the play you just ran.** Opened at the line before the snap, it showed the next play's
  line-up instead. The last play now stays until the ball is snapped, and a quarter break no longer loses it.
- **Long messages fit their box.** A four-line message ran out of the bottom of its card.
- **Your Controller Pak can be put back.** New Blitz now keeps a dated copy of your pak each time it changes (the last
  20 changes, and one a day for two weeks). Settings → General → **Controller Pak Backups** lists them with what each
  holds, for example "LAR, week 6", and puts the one you pick back. The pak you had is kept as a copy too.
- **A 2026 Season can't drop off the pak.** On an iPhone a season disappeared from Load after a session of save
  states; its files were fine, but the pak no longer pointed at them. Now only your own Save to that slot can change a
  saved season on the pak, and the season files any copy points at are kept.
- **Loading a save state no longer rolls back your Controller Pak.** A state remembers the pak as it was when you
  saved it. Loading an old one could make the game think its note was damaged, delete it and offer to create an empty
  one ("BLITZ NOTE NOT FOUND!"). That is how the season above was lost. Now the game reads the pak as it is when you
  load a state, so seasons, records and settings saved since then stay.
- **Handhelds: no crash on a key press at the launcher.** A menu that lost track of where it was closed the app on an
  RG DS. It now stays where it is.
- **PS5: a copy installed without a ROM says how to add one.** Instead of an empty launcher, a screen says to send it
  from Blitz Builder on your computer, or to copy your ROM to `/data/blitzrecomp`; a ROM copied there is found within
  a few seconds. Each release now raises the PS5 app's version, so the console sees updates (untested on a PS5).
- **PS5: Blitz Builder now sends 1.0.4.** It sent 1.0.2, the last build we ran on a PS5, so PS5 players missed every
  fix since. We don't have a jailbroken PS5 to test on right now, so this build hasn't run on one: if you play on a
  PS5, tell us how it goes on Discord. We're looking for PS5 testers.

## 1.0.3 (2026-11-24)

- **Quitting no longer crashes.** On Windows most quits mid-game ended in a crash, and a Mac or Linux quit could too:
  the app let go of the game's memory while part of the game was still running. 13 crash reports from players pointed
  at it; thank you for sending them.
- **Phones and handhelds: the menus keep their size.** Coming back to New Blitz from the app switcher (or the
  screenshot sheet) could leave every menu squeezed into the top-left corner, with taps missing it. Seen on iPhone;
  the fix is in every version, so no phone or handheld should see it.
- **Quitting saves your game.** Quitting during a game (Cmd+Q, the window's close button, Settings → Quit) asks one
  thing, **Quit Blitz?**, so a window closed by accident doesn't end a game; **Quit** saves the game and closes. No
  more "Save this game?" and "Quit without saving?". If the game can't be saved at that moment, it says so and offers
  **Quit Anyway** or **Keep Playing**.
- **Picking it up is one button.** The next start no longer stops on "Pick up your game?": a note with the score
  (bottom right, or the top of a phone's screen) says **Pick up your last game?** for about 12 seconds. **△** (the
  button it shows), a click or a tap loads it. **□** or **Start** (or a pick in the menus) puts it away. It's offered
  once, on the first start after quitting: leave it and that game isn't offered again. A game kept by the freeze
  rescue is offered the same way.

## 1.0.2 (2026-11-17)

- **A frozen game can be picked up again.** If the game stops responding mid-game, New Blitz keeps the start of the
  play it stopped on and says so ("Close New Blitz and open it again to pick up from ..."); the next start offers it
  through "Pick up your game?". Before, a freeze lost the game: save states and the background Auto slot waited on
  the stuck game. A stall that passes changes nothing, and a newer Save & Quit save is kept over it.
- **Season: a pre-game screen.** PLAY WEEK → PLAY now opens a screen with both teams already set: pick each team's
  uniform from the sets the 2026 Season has for it, or Pick Your Crew, then Play (or Start). □ goes back to PLAY WEEK.
  A week without a game for you (a bye, out of the playoffs) plays as before.
- **PS5: tested on a PS5.** Blitz Builder sends 1.0.2's PS5 build (build 805), which was played on a PS5 before
  release, and says so. Later versions keep sending it as the tested build, with their own as **Send Beta (untested)**.
- **PS5:** Instant Replay records at 640x360 and 25 fps (was 320x180 at 15 fps, the small handhelds' size): the
  console reported only part of its memory.
- **Phone controllers:** a second, third or fourth phone can join while one is playing, and a phone that drops can
  come straight back. Before, every other phone waited at "connecting" until the first one left.
- **Pick Your Crew:** any player's Start moves on, a phone controller's too. Before, only player 1's did, so a phone
  that joined after a gamepad (as player 2) pressed START for nothing.
- **Pick Your Crew** shows the lineup the game will play. With the Advanced season switched on it showed the latest
  week's lineup while the game played another (full strength), so a row you left alone wasn't who took the field:
  the Rams' two tight ends on the panel, Puka Nacua in the game.

## 1.0.1 (2026-11-15)

- **On-screen keyboard:** Up and Down from the bottom row (Shift, Space, Delete, Done) go to the key straight above
  or below, not one further along.
- **Settings → General** no longer crawls on slower devices (the RG DS ran it at 5-9 fps): its description panel was
  rebuilding itself every frame while the pointer rested on it.
- **PS5:** after a phone controller left, the DualSense could drop and reconnect every 100 ms until restart (a phone's
  pad had been given the DualSense's id). Fixed; controllers that come and go no longer fill the log.
- **PS5:** if a start ever hangs, the log now says what the app was doing.

## 1.0.0 (2026-11-15)

The first public release. The 0.9.x builds are its test builds.

- **New Blitz 2027:** NFL Blitz 2000 (USA v1.1), recompiled to run natively. One app per computer (Mac, Windows,
  Linux) that is also the **Builder**: it checks your ROM, sets the game up and sends it to a PS5 or an Android device
  (dual-screen handhelds in beta), saves the iPhone app (.ipa) for you to install with your own Apple ID (Sideloadly
  is the one we recommend; AltStore and SideStore work too), or exports a `.z64` for an N64 or an emulator.
- **Set up once, on your computer:** the first run asks where you want to play (this computer, a phone or handheld,
  a PS5, or an N64/emulator), takes your ROM, offers the 2026 Season, then sends everything the device needs, ROM
  included, so phones and consoles start ready to play. Download it as one file: `Setup.exe` (Windows), a `.dmg`
  (Mac) or an AppImage (Linux, untested).
- **Two ways to play:** **Original 1999**, the cartridge as it was, and **New Blitz**, with the 2026 Season pak: 32
  teams, 2026 rosters and schedule, current uniforms and team art, Standard and Advanced seasons, Pick Your Crew and
  Pro Bowl. The pak is a one-click, optional download. Keeping rosters and uniforms up to date is on by default and
  can be turned off; crash reports are off unless you turn them on (Settings → General shows what one contains).
- **Around the game:** hi-res, widescreen and 60 fps or more; controller remapping; couch play for up to 4 players;
  phones as controllers; save states, Redo Play and Save & Quit; tournaments for 2 to 8 players; the Blitz codes as
  house rules, Play Clock, Realism rules and a smarter CPU (Settings → Rules); Settings → General → Save Logs for
  bug reports.
- **Paks:** Add-Ons installs season, uniform, art, announcer and rules paks.
