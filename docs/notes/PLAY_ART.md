# Play art: the routes of your play, drawn before the snap

The team (2026-11-26): Madden-style play art. Before the snap, draw the routes of the play you picked on the turf (an arrow
from each receiver's lineup spot), for your own team only, and take it away at the snap.

**Status: option A built on branch `play-art-turf` (2026-11-26), not yet run in the app.** Ribbons on the turf in the
game's own 3D pass; see "What was built" at the end. The dig below still holds. Tags: [S] read in the code, [V] checked
against the game's own art, [I] inferred.

## Where a receiver's route comes from

```
0x802EA4AC[side]  chosen play  ->  play record (0x24 bytes, main)
                                     +0x00  formation pointer  ->  7 x 0x14-byte lineup entries
                                     +0x04  flags (2 punt, 4 FG, 0x10/0x20 fakes, 0x40 the CAPS plays 27-44)
                                     +0x08  7 route-script pointers, one per player slot
```

Player slot `s` of side `t` is player record `t*7 + s` (`0x802D1590 + i*0xB98`): slots 0-2 the linemen, 3 the QB,
4-6 the receivers (the RB is one of the three). [S]

- **Offense plays:** 45 records, pointers at ovl_a `0x80336B24`, names at `0x80336BD8` (Switch Up = index 0; the list
  is in `build/analysis/cpu_logic/playnames.json` and `tools/analysis/play_routes.py`). Defense: 18 at `0x80336C8C`
  (same record format; not decoded here). A Play Editor play is a runtime record (the CPU notes see `0x802E9228`), so
  reading the record through `0x802EA4AC` covers custom plays too. [S, editor: I]
- **Lineup, ovl_a `0x803028EC`** [S]. For each slot, from formation entry `f = form + 0x14*s`:
  - `x = spot*7.8 - 390 ± f.x` (side 1 uses `100 - spot`; the sign follows the team's direction `+0xB1C`), clamped
    to ±460.2. **7.8 game units = 1 yard** (`0x40F9999A`), so `f.x / 7.8` is the depth in yards (negative = behind).
  - `z = ball_z ± f.z` (`ball_z` = `0x802EA494`): `f.z` is negated for side 1, and the whole thing is mirrored when the
    play is flipped (`0x802EA4B4`, the □ flip / the CPU's 40%).
  - `+0x08` stance animation (`func_8027CB0C`), `+0x0C` the drone mode copied to player `+0x19C`, `+0x10` flags
    (receivers: `& 7` = 1, 2, 4).
  - It also clears player `+0x1A4` bits `0x800000, 0x1000, 0x200, 0x100, 0x20, 0x8`.
- **Route hand-out, ovl_a `0x8030EE90`** [S], called right before the lineup (`0x80300024`, `0x80300A74`,
  `0x80332F20`): for every player `i`, `*(0x80368A40 + i*0x94) = record(side i/7)->routes[i%7]`. That word is the
  script cursor. **So at the line (play phase `0x802EA4A8` = 1) RAM already holds each player's route.**
- **Interpreter, ovl_a `0x80310B98`** [S], run per tick from the drone tick `0x8030FA80` (mode table `0x803348A0`
  indexed by `+0x19C`). Jump table `0x80338688` (opcode - 1, opcodes 1-20). Debug strings next to it name the ops
  (`recvr %2x begins RUN_YDS(%d)`, `reaches los+%d`, `turns to %d`, `goes into BLOCK mode`, `begins improvising`,
  `drone %2x hits unknown script command %d`).

## The route script

Big-endian 32-bit words: an opcode, then its arguments.

| Op | Name (`play_routes.py`) | Words | What |
|---|---|---|---|
| 1 | NOP | 1 | nothing (starts most receiver scripts) |
| 2 | TURN | 2 | face direction `a` (below), snapped to one of 32 (table `0x803348CC`) |
| 3 | RUN_TO_LOS | 2 | run until `a` yards past the line ("reaches los+%d") |
| 4 | RUN_YDS | 2 | run `a` yards in the current direction (measured from where it started) |
| 5 | IMPROVISE | 1 | end of script: free AI ("begins improvising") |
| 6, 7 | (mode 3) | 1 | end of script, drone mode 3 |
| 8 | BLOCK | 1 | end of script: block (linemen use 5, many QBs/backs 8) |
| 9, 14 | WAIT_A, WAIT_C | 1 | if a defender is within 62.4 units: play a look/cut animation and wait for it [I] |
| 13 | WAIT_B | 1 | similar, 23.4 units [I] |
| 10 | COVER_HINT | 4 | dir, chance %, time: at random (chance scaled by the difficulty table `0x80335C00`) tells the **covering defender** which way the break goes. Not part of the path |
| 11, 12 | FLAG_ON, FLAG_OFF | 1 | set/clear drone flag `0x10` (+`0x40` in `+0x8`); they bracket legs, meaning not found [I] |
| 15 | COVER_HINT2 | 3 | dir, delay, written into the covering defender's drone block [I] |
| 16 | WAIT_TICKS | 2 | wait `a` ticks |
| 20 | SET_68 | 2 | flag `0x2000000` and `+0x68 = a` |
| 17-19 | | | unknown (the "unknown script command" print) |

All 45 offense plays decode cleanly with these sizes: every script ends in IMPROVISE or BLOCK. [S]

**Directions** [S, V]: 1024 = a full turn, **0 = straight downfield**, ±256 = across. In the offense's frame
(D yards downfield, L yards across with **+L = the offense's right = screen right** behind the offense camera), a leg
goes `dD = cos(a·2π/1024)`, `dL = -sin(a·2π/1024)`: a positive angle bends to the offense's left. (TURN subtracts the
camera yaw `0x803668A4` because drone steering is stick-like; the movement code adds it back. COVER_HINT, which
doesn't, gives the same world direction: `+0x100` for side 0, `+0x300` for side 1.)

**Mirroring** [S for the mechanism, V/I for the rule]: TURN, COVER_HINT and COVER_HINT2 negate their angle when player
`+0x1A4` bit `0x200` is set. The lineup clears that bit; no plain `ori 0x200` sets it (probably an animation-script
flag command, `0x8027E654`). **Checked against 8 play-select tiles: the receivers who line up left of the ball run
mirrored, the rest don't.** So Midway authored routes as "negative = toward your own sideline", and one script works
from either side. With a flipped play the formation mirrors and the same rule follows. Worth one save-state check
(log `+0x1A4 & 0x200` for slots 4-6 at phase 1 and phase 2) before relying on the rule instead of the bit.

**Length:** scripts are long (20-50 yards): after the route proper come "keep getting open" legs before IMPROVISE. The
tiles show only the first two to four legs. Play art should cut the path (by length or depth, or at a chosen op).

## Decoded examples (yards from the ball; mirror applied)

Run `python3 tools/analysis/play_routes.py "QB Post"` (all plays without a name; `--json` for a file).

| Play | Slot (start D, L) | First legs | Tile |
|---|---|---|---|
| **Switch Up** | 4 (-4.4, +7.7) | in at 22° to LOS+10 → out 8 → out-and-up 6 → up 17 | ✓ inside release then up the right |
| | 5 (-5.5, -3.8) mirrored | up-left 45° 10 → up 9 → out 5 → back 5 (comeback) → long | ✓ |
| | 6 (-5.5, +3.8) | up 4 → right 14 → up 24 | ✓ |
| **QB Post** | 4 RB (-6.4, +1.1) | swing right in 2-yd steps → up the right to 14 → in 8 | ✓ |
| | 5 (-1.9, -5.1) mirrored | up-left 7 → up 8 → left 7 → up 5 → up-right 22 (the post) | ✓ |
| | 6 (-2.0, +5.1) | up-right 8 → up 10 → left 14 → up 5 → up-left 22 | ✓ |
| **Slip Slide** | 4 RB (-6.4, +1.1) | left 12 behind the line, curl up → up 17 → up-right | ✓ the swing left |
| | 5 (-1.9, -5.1) mirrored | up-left 15 → up 6 → right 7 → up 12 | ✓ |
| | 6 (-2.0, +5.1) | up-right 12 → left 25 | ✓ the left arrow |

`python3 tools/analysis/play_art_compare.py build/play_art_compare.png` draws each tile (false colour; the tiles' own
palettes are empty, the game colours them at draw time) beside the decode raw and mirrored: QB Post, Switch Up,
Zig Zag, Cruisin', Upper Cut, Slip Slide, Criss X, Hail Mary. Tiles are 77x61 CI4 with rows padded to 78 pixels
(the old `build/analysis/glyphs/png/swtchup.png` came out blank because of that and the empty palette).

The slot 3 (QB) script is `TURN 0, RUN_TO_LOS 20` on the under-center plays and BLOCK elsewhere: it only matters if
someone else ends up with the ball [I].

## Options

Common to all: at phase 1, for the side with the ball when a human controls its QB (`receiver_targets.cpp`'s
`presnap()` already finds this), read `0x802EA4AC[side]`, walk slots 4-6 (and 3 on trick plays) with the decoder's
rules, start each path at the player's **actual** position (`+0x0`/`+0x8`, so flips and lineup clamps are already in),
mirror the ones left of the ball, and cut each path. Hide on phase 2 with a ~0.2 s fade. It's about 150 lines of C++
for the walker, shared by every option.

**Style:** one colour per receiver, the Receiver Buttons order (left to right as they line up: △ / R1 / ○, or the
pad's own glyphs from `receiver_glyphs()`), so the art also teaches which button throws where; the button glyph at
the start of each line; a team-colour variant as a setting. Rounded joints, an arrowhead at the end, 70% opacity.
Show it while Turbo is held (where the pre-snap receiver icons already appear), or always for the first second at
the line.

| | What | Cost | Risks |
|---|---|---|---|
| **A** | **Ribbons on the turf in the game's own 3D**: extra tris appended to the display list after the field draw (around the camera hook `blitz_camera_tag`, `gfx_ext.cpp`), depth-tested so players stand on top of them. | High: find the point between field and players, set combiner/texture state, an RT64 matrix group so interpolation doesn't smear; Classic and Modern both; PSP stubs. | Display-list surgery; z-fighting with the turf; it's the only option that looks truly "painted". |
| **B** | **Projected 2D overlay**: project the field points with the camera eye `0x803668B0` and rows `0x80366904` (the cull's math in `WIDESCREEN_CULLING.md`; check against the game's own `+0x750/+0x754` screen positions), draw lines in the UI layer above the game. | Low-medium. RmlUi has no line primitive: thin rotated divs per leg, or a small custom element. | Draws over the players (fine at 70%); RmlUi vs RT64 frame timing (the camera is still at the line, so small); widescreen and the tall DS view need the same widening the receiver icons use. |
| **C** | **First break only** ("hot route" stubs, ~10-12 yards) in A or B. | A setting on top of A or B. | Less to read, fits the tall view, never runs off the top of the screen. |
| **D** | **Second screen diagram** on BLITZ DS / two-screen devices: a top-down board like the play-select tile, drawn from the decode (ds_view.cpp's RML panel), with the defense's lineup as dots. | Low, no projection. | Only on dual screens; the eyes leave the field. A natural extra, not the main feature. |

**Recommendation: B + C first**, behind a Rules setting (Off / First break / Full routes), shown while Turbo is held at
the line and faded at the snap, colours from Receiver Buttons. It reuses `presnap()` and the icon layer, needs no
display-list work, and works the same in Classic, Modern and team+ (the hooks and addresses are in main and ovl_a,
which team+ shares; check the play tables' team+ bytes first). Do D for BLITZ DS when B lands (same walker). Keep A for
later, if the overlay-on-top look bothers anyone.

Other risks to check when building:
- **Camera at the line**: steep enough that across-the-field legs foreshorten; deep legs run off the top, so cut
  around 25 yards past the line.
- **Defense**: their records (18 plays) use the same scripts for coverage; a human defender's own assignment could be
  shown the same way later. Never show the opponent's art.
- **Audibles / flips after play select**: re-read the record every frame at the line instead of caching it.
- **Mirror rule**: confirm with the bit (above) once; until then the left-of-ball rule matches every tile checked.
- **Two humans on offense** (2v2): both see the same art; the colours follow player 1's buttons.

## Mocks

`python3 tools/analysis/play_art_mock.py FRAME.png OUT.png --play "Da Bomb" [--style hot]` paints a decoded play on
a screenshot at the line (a homography fitted by hand to the trailer's `bomb.mp4` first frame, which is Da Bomb at 4th
and goal; the fit also shows the field is about 47 yards wide in game units, not 53.3).
- `build/play_art_mock_full.png`: option B, full routes cut at 27 yards.
- `build/play_art_mock_hot.png`: option C, first 12 yards.
- `build/play_art_compare.png`: tiles beside the decode.

The mock lines lie over the players (as option B would); option A would put them under.

## Files

- `tools/analysis/play_routes.py`: decoder (plays, formations, scripts, traced paths).
- `tools/analysis/play_art_compare.py`, `tools/analysis/lzo.py` (pure-Python LZO for the tiles): the tile check.
- `tools/analysis/play_art_mock.py`: the mock.

## What was built (option A, branch `play-art-turf`, 2026-11-26)

The team picked A: the art painted **on the turf**, under the players. Settings → Rules → **Play Art**: Off / **First Break**
(default) / Full. Default First Break because it's a look at your own play only, never the opponent's, and changes no
rules; one line in `add_play_art_option` makes it Off.

**Where it draws.** `gfx_load_camera` runs three times a frame in play (dumps `build/dumps/rdram_*`, all six the same):
the field's pass (F3DEX2: stadium, turf, the line and first-down stripes, the shadows), then `G_LOAD_UCODE` F3DLX2.Rej
and the second load for the players, then F3DEX2 again for the ball and effects. The existing hook
(`gfx_ext.cpp blitz_camera_tag`, before 0x8000FABC) now calls `blitz_play_art_camera(rdram, gp, call)`; at call 1 (the
players' load, after its two matrix groups) `play_art.cpp` writes at the game's own cursor:

```
DE010000 <commands>          branch over the data
<64-byte Mtx>                1/4 scale (vertices in quarter units: s16 has room, joints stay smooth)
<vertices>                   16 bytes each, colour in the vertex
E7 pipesync, gEXMatrixGroup 0xB4000000 (PUSH), G_MTX modelview LOAD|PUSH
D9 clear all, set ZBUFFER|SHADE|SHADING_SMOOTH (no cull), D7 texture off, cycle 1CYCLE
E2 render mode 0x00504A50 (ZB_XLU_SURF: depth test, no depth write, IN*A + MEM*(1-A)), FC G_CC_SHADE
per batch of <= 32 vertices: G_VTX, G_TRI2/G_TRI1
E7, G_POPMTX, gEXPopMatrixGroup, then the camera's own state again (geometry ZBUFFER|CULL_BACK, render mode 0x0F0A4000)
```

The turf wrote depth (`Z_CMP|Z_UPD`), so the ribbons (0.8 units up) test against it; the players are drawn next and
stand on top. Everything is world space through the game's projection, so the tall view and widescreen need nothing.
The list lives in the frame's own buffer like the rest of gfx_ext's additions; it's skipped (logged) if the list so far
plus the art would pass 8 KB (the art is 0.8-2.5 KB, the cursor ~2.9 KB in at that point; capacity 0x5DC0). Its own
matrix group, pushed and popped, keeps the scene group's matrix order the same whether or not the art is drawn.

**Gating.** Phase `0x802EA4A8` = 1, the gameplay camera, play select not on screen, a human on the offense
(`0x802EA47C` bit 0's seven players, any with a player slot), and **not** when the defense has a human too (a shared
screen would show your play to the other player; `PLAYART h2h 1` shows it anyway). Kicks (record flags 2/4 without the
fake bits 0x30) draw nothing. Re-read every frame (audibles, the □ flip), faded in over 6 frames, gone at phase 2.

**The routes.** `play_art_walk.h` ports `play_routes.py` (same ops, same mirror rule, same RUN_TO_LOS maths): all 45
plays x 7 players match the Python paths to 0.02 yards (`make test-play-art`). The world axes aren't taken from side or
flip flags: the seven players' spots are correlated with the formation's x and z over every pair, which gives the
downfield and across signs whatever the side, the direction or a flipped play. Each route starts at the receiver's
actual spot. **First Break**: legs to the first turn over 25 degrees, then up to 5 yards of the new direction (over
however many legs keep it), 15 yards at most. **Full**: to the script's end, 40 yards at most, 14 points.

**Style.** Ribbon 0.55 yards wide, mitred joints (a wedge fills a turn over 120 degrees), arrowhead 1.15 yards long,
alpha 208. Colours left to right on screen as Receiver Buttons orders them (`+0x750`): green, blue, red (△ R1 ○);
an Xbox pad's Y is yellow. Not drawn: the QB, the defense, button glyphs at the start (an RmlUi layer could add them).

**Checked headlessly:** the walker against Python; every list the 45 plays make walks clean (branch, data, loads of 32
or fewer, triangle indices inside the load, push/pop paired, state restored; 2.5 KB the biggest); the four changed app
sources compile with the Modern and Classic build flags. `play_art_test --rdram DUMP DLIST SIZE OUT [first|full]
[lineup]` splices the art into a real frame at the players' camera load and `tools/gbi_dump.py --render` draws it:
Switch Up's three routes on the turf in the right shapes, order and colours, arrowheads and joints as meant
(`lineup` moves the offense's records onto their formation spots, as the dumps are mid-play).
**Not checked:** a run of the app (no build here, and the Mac app wasn't launched); RT64 itself (z-fighting, MSAA, the
Rej microcode path), the tall view and BLITZ DS, Android/Adreno.

**Live, for testing** (`dev_input.cpp`; on Android `tools/android/remote.sh cmd live PLAYART ...`):
`PLAYART first|full|off|auto` (auto = the setting), `PLAYART width 2.2` (half width, units), `lift 0.8`, `alpha 208`,
`ztest 0|1`, `h2h 0|1`. Log lines `[playart]`, on a change only: `play 0 'Switch Up' side 0 first: routes 3/2/1 legs,
9 prims, 800 bytes, list at +2952`, `gone (phase 2)`, `none: a human on defense too ...`, `... no room in the frame's
list`.

Files: `src/game/play_art.cpp` (game side, option, live command), `src/game/play_art_walk.h` (walker, geometry, list),
`gfx_ext.cpp` (the call), `house_rules.cpp` (the option), `src/psp/stubs_hooks.cpp` (the option, off), `dev_input.cpp`
(PLAYART), `tests/play_art_test.cpp`, `tests/test_play_art.py`.
