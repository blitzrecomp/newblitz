# Changelog

## 1.0.5 (unreleased)

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
