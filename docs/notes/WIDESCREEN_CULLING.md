# Players at the widescreen sides

The team (2026-11-26): on widescreen devices the field fills the width, but players vanish near the left and right sides.

## What the game does

The game culls players on the CPU, against its own 4:3 view. RT64 has nothing to do with it: its `G_CULLDL` is a
no-op and it doesn't model the `.Rej` microcodes' rejection.

- **The cull: ovl_a `0x802FB6A8`**, a process (no `jal` callers) that loops over the players every frame
  (`0x802D1590 + i * 0xB98`, `*(0x80283BE4)` of them). For each one it:
  1. takes his position minus the camera eye (`0x803668B0`); his height is `+0x4` plus `+0x28`;
  2. turns it with the game camera's rows at `0x80366904`, which are unit vectors (built at ovl_a `0x80318E9C`). That
     gives `x` (`f18`), `y` (`f10`) and depth `z` (`f8`).
  3. culls him if `z < 28`, if `|x| > z * 0.769 * (120 / 512 + 0.5)`, which is `0.565 z`, or if
     `|y| > z * (256 + 240) / (2 * 512 * 1.3)`, which is `0.373 z`. 512 and 256 come from `0x8002B674` and
     `0x8002B670`.
  4. sets or clears **bit `0x2` of his flags (`+0x1A4`)** at `0x802FB8C4`. A second rule at `0x802FB918` can clear the
     bit again for reasons that have nothing to do with the camera (`+0x3BC`, `+0x3C8`, `0x802EA418`). The two paths
     join at `0x802FB928`.
  5. also writes his marker position at `+0x750`/`+0x754` and his depth at `+0x758`.
- The 4:3 picture's edges are at `tan` 0.378 across (`tan(31.62° / 2) * 4/3`) and 0.283 up and down. That leaves the
  cull a margin of about 1.5x across. A wider picture's edge is `0.283 * aspect` across: 0.503 at 16:9 (inside the
  margin, but bodies reaching in from past the edge pop) and 0.614 at 19.5:9, which is past the cull. On a phone,
  players vanish well inside the picture.
- **Readers of the bit:**

  | Address | What | Kind |
  |---|---|---|
  | main `func_80255180` `0x802551B4` | the player's draw. Off: render record `+0x28` = 0, so no limb matrices and no model (`func_8027139C`) | drawing |
  | main `func_80255E58` `0x80255E88` | his shadow | drawing |
  | ovl_a `0x802F6BA4` | an off-screen defender's speed ×2.5 (×1.5 with the ball in the air) | **gameplay** |
  | ovl_a `0x802FB1DC` | after the whistle, who to go for | **gameplay** |
  | ovl_a `0x803061A8` | only for players on screen: `rand_mod` rolls, then `func_8027BF6C` | **gameplay** (RNG) |
  | ovl_a `0x802FBD78` | copied to the marker records at `0x80368658` | HUD |
  | ovl_a `0x8032AF8C` | a per-player loop (not looked into) | ? |

## What changed (branch `widescreen-cull`)

The bit steers gameplay, so the game keeps computing it exactly as it did. `src/game/widescreen.cpp` works out a
second, wider answer beside it and gives that only to the two draws. Hooks are in `blitz2000.toml`, so both Classic
and Modern (engine32) get them, and team+ copies them. The sites are byte-identical in team+.

| Hook | Does |
|---|---|
| ovl_a `0x802FB6A8` (entry) | `blitz_ws_cull_begin`: forget the last game |
| ovl_a `0x802FB860` | `blitz_ws_cull_test`: the game's own three tests, with the `|x|` limit (`f2`) times the picture's widening |
| ovl_a `0x802FB918` | `blitz_ws_cull_hidden`: the game's second rule hid him, so the wide answer hides him too |
| ovl_a `0x802FB928` | `blitz_ws_cull_done`: notes the game's final bit; wide = game's bit, or (passes the wide test and not hidden) |
| main `0x802551B4`, `0x80255E88` | `blitz_ws_draw_flags`: in `v1`, the flags get bit `0x2` when only the 4:3 edges hid him. Nothing is written back to RAM |

- **Widening:** the window's width over height divided by 4/3, set every UI tick (`blitz::widescreen_update`, from
  `main.cpp`). This follows RT64's Expand rule. In BLITZ DS mode 3 it is the second screen's shape. The tall view
  (narrower than 4:3), Aspect Ratio Original, and any picture no wider than 4:3 give 1, and with 1 `blitz_ws_draw_flags`
  returns the game's flags unchanged. Manual is taken as RT64's default 16:9. The widening is capped at 4.
- **Kept off:** a player whose bit the game set and something else later cleared (a cut scene, a substitution). The
  PSP has stubs (`src/psp/stubs_hooks.cpp`), so its draws get the game's flags.
- **Checked:** N64Recomp run on the edited `blitz2000.toml` (Classic), with the hooks landing where intended
  (`0x802FB928` after its label). The generated C, `widescreen.cpp` and `main.cpp` compile. A headless test replays the
  game's 4:3 math through the hooks: identical flags at 4:3 over a 4000-case sweep; at 19.5:9 a player at `x/z = 0.6`
  is drawn while the game's bit stays off; depth, vertical, the second rule and a new game are still respected.
- **Not checked:** anything on screen. Not built as an app, and no window was opened. Draw cost: at most every player
  is drawn, as at the line of scrimmage, so the display list never holds more than the game already draws there.

## Still 4:3 at the sides (not looked into)

- The **vertical** test is unchanged. It only matters for the tall view, whose camera differs from the game's anyway
  (`tall_view.cpp`).
- The **markers over the players** (`+0x750`) and their off-screen arrows (`0x80368658`) still follow the 4:3 bit.
- **The ball, the refs, the first-down and scrimmage markers, the stadium and the crowd:** no other code reads the cull
  constants (`0x8002B670`/`0x8002B674` are read here and by the marker and camera code around ovl_a `0x80309758` and
  `0x80319F50`). None of them was checked on screen, so this is still ENHANCEMENTS item 16, R2.
