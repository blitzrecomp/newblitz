# First run: what a new player hits, and the hints that help

> **Status (2026-10-19):** Modern Blitz now runs on the original v1.1 game, not the Team Extension (docs/BLITZ2026.md). What follows describes the Team Extension build as it was planned or researched; it still applies to that optional build.

A lap of a brand-new install (empty app folder, no ROM, Modern Blitz, 2026-10-18), screen by screen, and the plan that came out of it. The lap's screenshots were cleared out; `build/shots/first_run_review.png` (not in git) shows the hints and launcher that came out of it.

## What the lap found

| # | Where | What a new player hits |
|---|---|---|
| 1 | Launcher, no ROM | LOAD ROM and CHANGE ROM both show, though there is nothing to change yet. Nothing says which ROM the app needs. |
| 2 | Load ROM | The file picker could get stuck on screen: it ran as a blocking modal inside SDL's event pump and stopped taking input. **Fixed (d2f28a6):** a sheet on the game window. |
| 3 | Start Game | The welcome note didn't show: it was marked as shown but never drawn. It also arrived as a modal you had to dismiss before playing. |
| 4 | Title, main menu | Fine: the game's own screens say "✕ Select" in the player's buttons. Nothing says how to open Settings. |
| 5 | Controller select | The first real puzzle. Nothing says to push the stick left or right to choose a team's side. |
| 6 | Rumble Pak screen | "Please insert Rumble Paks into controllers now" means nothing on a Mac. The skip is off for a new install. |
| 7 | Team select | Nothing says △ changes the uniform (Uniform Picker) or that you can choose your CPU opponent. |
| 8 | Kickoff | The game's own tips say "use TURBO", "TURBO + PASS", in N64 terms: not which of your buttons is turbo. |
| 9 | Play select | A 3×3 grid of plays, and nothing says how to pick one, flip it or turn the page. |
| 10 | Save states, Redo Play | R3 combos nobody would find without the guide. |
| 11 | Settings | Opened with Esc or Create (View on Xbox). Not obvious, and nowhere on screen. |

## Plan

### A. Straight fixes (no hints needed)

- **Launcher:** without a ROM, show what to load under the title ("Needs your NFL Blitz 2000 ROM: USA, v1.1" / Modern: "…with Zoinkity's Team Extension, or a community update built on it"), and hide CHANGE ROM until a ROM is loaded.
- **Skip Rumble Pak Screen** on by default for new installs (an existing setting stays as it is).
- **The welcome modal** is replaced by the first hints, which don't block play.

### B. Hints

Small cards at the bottom left (toasts are bottom right), in the Blitz '27 look:
- a cyan **TIP** eyebrow, a yellow title, one or two lines of body text;
- the player's own buttons drawn as glyphs (player 1's bindings: a DualSense shows ✕ / R2, an Xbox pad A / RT, a Switch Pro B / ZR, and the keyboard its keys);
- a quiet footer: *Hints: Settings → General*.

Rules:
- **Each hint shows once**, the first time its moment comes, and is remembered in Settings.
- **A card takes no input** (the game keeps every press). It fades when its moment ends (you leave the screen) or after about 12 seconds.
- **One card at a time**, and none during the attract demo, over Settings, or while a prompt is up.
- **Turning them off:** Settings → General → **Hints** (on by default). **Show Hints Again** brings back the ones already seen. The first card says how to turn them off.

| Hint | Shown when | Says |
|---|---|---|
| Load your ROM | Launcher, no ROM | Which ROM (Classic / Modern), that the app keeps its own copy, a .zip is fine |
| Settings any time | The game's first menus (after the intro) | {menu} or Esc opens Settings; ○ backs out. Hints can be turned off there |
| Pick your side | Menus, once a controller's in the game (Arcade → Play) | Push the stick left or right at Controller Select to choose your team's side, then {A} |
| Team select | Team select | Stick to choose, {A} to pick; {CLEFT} changes the uniform (if on); Choose CPU Opponent lets you pick who you play (if on) |
| Pick a play | First play select | Move to a play and press {A}; {B} flips it; {Z} turns the page |
| Offense | First play on offense | {Z} turbo; {A} passes; {B} jumps ({Z} + {B} spins) |
| Defense | First play on defense | {A} switches player; {B} tackles (dives with {Z}) |
| Do-overs | Third play select | Hold {R3}: + {L1} saves, + {R1} loads, + D-pad ↓ replays the last play |
| Settings tour | First time Settings opens | {L1} {R1} change tabs; Rules turns on the Blitz codes; Players shows who's who and phones |

Both apps; the ROM wording differs. Every hint can be previewed with the dev command `HINT <id>`.

### C. Later (not this pass)

- A one-page "How to play" in Settings (the guide's best bits) for anyone who skipped the hints.
- The phone page's own first-run note.
