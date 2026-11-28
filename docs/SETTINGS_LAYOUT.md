# Settings layout (1.0.5, branch `settings-1.0.5`)

General had grown to some 40 rows. In 1.0.5 it keeps the app-wide ones and the rest are drawn where a player would look
for them. **Only where a row is drawn moves**: every option keeps its id, its config and that config's file
(`general.json`, `party.json` …), so no saved setting is read from anywhere new and nothing is reset. The table is
`src/game/settings_layout.h`; RecompFrontend patch 0052 (`recompui::config::place_option`) draws a config's row on another
tab, writing it to its own config (saved at once) and keeping it current by polling. `tests/test_settings_layout.py`
checks every 1.0.4 General id still exists and is either kept on General or placed exactly once.

## Tab by tab

| Tab | What's on it | Moved in from |
| --- | --- | --- |
| **General** | Main Menu, Blitz Builder, Change ROM, Switch Game; Volume; About; Hints, Show Hints Again; **Freeze Game in Settings** (new); Save Logs, Send Logs (test builds), Send Crash Reports, What Crash Reports Send; Copy Debug Info (Development) | — |
| **Graphics** | The renderer's own rows (Apply), then DISPLAY: TV Mode, Frame Counter (Development); POTATO MODE (Android): Potato Mode, Two Screens (Potato), Two-Screen Sharpness, GPU Waits, Frame Copy | General |
| **Two Screens** (only with a second screen, or the Development rows shown) | SCREENS: Two Screens, DS Screens, Swap Screens; CAMERA: Field Ahead, Camera Zoom Out; CUSTOM LAYOUT: Custom Layout, its 21 dials, Reset Custom Layout | General (SCREENS, CAMERA); the dials were only the live `TALL` command |
| **Controls** | CONTROLLER, above the button mappings: Player 1 Rumble, Rumble Strength, Joystick Deadzone, Button Icons, Fullscreen Button (computers), Touch Controls (phones), Background Input (computers) | General |
| **Players** | unchanged | — |
| **Teams** | its own rows (Game, Rosters, Lineups, Roster Room, Season, Tiebreakers…), then BRACKET: Player Names, Each Round, Matchups, Create Tournament, Show Standings | Bracket tab (hidden now; `party.json` unchanged) |
| **Rules** | its own rows, then DO-OVERS: Redo Play, Instant Replay; GAME FLOW: Skip Intro Screens, Skip Rumble Pak Screen | General |
| **Saves** (new, `saves.json`) | SAVE STATES: Save State Buttons, Resume Interrupted Games; CONTROLLER PAK: Controller Pak Backups, Save Sync (when that branch is in); then rows other features add; ALL YOUR DATA: Reset Data, Restore a Backup | General |
| **Add-Ons** | unchanged | — |

Sound and Export stay hidden as before (Volume is on General; Export has its own screen).

## For Save Sync (branch `cloud-sync`)

Its `save_sync` row, still added to General in config.cpp as that branch does, is already placed on Saves under
CONTROLLER PAK by the table: merged as it is, it shows there. Anything new can go in the Saves tab's own config instead:
`blitz::saves_tab_add_entries([](recomp::config::Config& saves) { saves.add_bool_option(...); })` from a static
initializer (settings_tabs.cpp), drawn after the Controller Pak rows and before Reset Data.

## Two Screens

- **Modes** (as before): Two Screens on/off, DS Screens (Both Screens, Game + Companion, Stylus, Controller; L3 steps
  through them), Swap Screens.
- **Presets**: Field Ahead (0.40-0.65) and Camera Zoom Out (0-30%), saved in General as before.
- **Custom Layout** (`screens.json`, this device only): off by default, and off is exactly 1.0.4's picture. On, it starts
  from the presets in use (nothing moves until a dial does), greys the two presets out, and shows the dials as sliders:
  - Camera: Field Ahead (0.20-0.95), Camera Zoom Out (0-60%), Sky Limit, Marker Height, Wide View on Throws (0 off),
    Wide View Ease Back, Wide View Angle, Wide View Farthest.
  - Text and panels: Scoreboard Screen, Scoreboard Size, Banners Lower, Down & Distance Screen / Height / Height (Lower) /
    Words, Score Panel Height, Big Message Height, Rules Card Height, Play Result Sideways, Pause Menu Height, Kick Meter.
  - Reset Custom Layout puts every dial back. Built-in values are tall_view.cpp's start values (panel -29, panel_words 0,
    card 0, score_lift 8, msg_up 10, result_dx 0, pause_up -3, banner 0.12, board 1.35, score 0, meter 0, panel_low 0,
    panel_low_y -28, sky 28, mark 18, wide_out 0, wide_in 0.025, pread 45, wide 320); `make test-screens` reads them from
    that file and fails if the table drifts. The dev kit's `TALL <name> <value>` still sets any of them for the run.
- The rule (two_screens.h `resolve`): Custom Layout off, the camera from the presets and everything else built in (only
  tilt and zoom are set, so a dev's `BLITZ_TALL` file still works); on, every dial is the player's, clamped to its range.

## Freeze Game in Settings

Settings → General → Freeze Game in Settings (on; off by default on a Potato Mode device under 1.5 GB). Before, the game
ran on under Settings with no input: a play went on, play select's clock ran down. Now opening Settings once the game has
drawn holds it at its dispatcher (src/game/settings_hold.cpp; the same place and way as the background hold and Instant
Replay), with RT64 presenting every VI so the menu and the held picture keep drawing.

- **Live redraw**: a Graphics change (once applied) or a Two Screens change saves the moment as the mark `settings` (in
  memory only, never a file), then loads it, runs 14 dispatcher passes with the new settings and holds again. Each load
  goes through `restore_snapshot`: the Controller Pak is never written and the game's view of it is refreshed
  (`pak_state_restored`); Redo Play's plays are kept for this mark. Not on a Potato device under 800 MB (8 MB copy).
- **If the moment can't be saved or loaded within 0.8 s** (a Season screen, a load waiting on the game's processes) the
  request is cancelled (`blitz_savestate_mark_cancel`) and the game stays held without redraws: a late load can never
  land in the middle of play.
- **Let go**: Settings closing; the setting turned off; any save or load (Save & Quit, Main Menu's save: the game runs
  until it's done, then holds again); the app going to the background (its Auto slot needs the game); the UI thread not
  ticking for 0.75 s. The freeze rescue, the stall watch and BLITZ DS's frame-rate guard don't count a held game.
- Log lines: `[settings] hold: ...`.
