# Changelog

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
