# Changelog

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
