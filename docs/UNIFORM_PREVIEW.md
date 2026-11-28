# Uniform preview (offline)

See a uniform on the game's own player model without running the game. Run everything from the repo root with
`.venv/bin/python`. Each sheet takes about 10 seconds per set.

## Commands

```sh
H=/path/to/league_pak_art_out      # the handoff root

# Pac-12 team: sheets for home, away and every alternate, plus a page you can drag around
.venv/bin/python tools/uniform_preview.py $H/pac12/CSU --out build/uniform_preview/CSU.png --html build/uniform_preview/CSU.html
# CFL team, with a reference row of the NFL team it is built on
.venv/bin/python tools/uniform_preview.py $H/cfl/BCL --out build/uniform_preview/BCL.png --reference
# a BUILT league pak's team, as the app dresses it: each set's "uv_override" applied (per-cap textures such as the
# CFL's right sleeves, and a table's coordinates on the team-select model)
.venv/bin/python tools/uniform_preview.py build/league_paks/cfl --team EDM --out build/uniform_preview/EDM.png
# one set only (home | away | part of an alternate's name)
.venv/bin/python tools/uniform_preview.py $H/pac12/CSU --set away --out build/uniform_preview/CSU_away.png
# one view at any angle: yaw 0 front, 90 his left side, 180 back, -90 his right side; pitch up/down; dist 0.5 = closer
.venv/bin/python tools/uniform_preview.py $H/pac12/CSU --set home --yaw 90 --pitch 10 --target helmet --nearest --out x.png
# NFL art round: a set folder or a whole team folder (build/nfl_uniforms_out/LAR/home_2026)
.venv/bin/python tools/uniform_preview.py build/nfl_uniforms_out/LAR/home_2026
# loose files: name the parts one by one, and the NFL team whose layout they're on (missing parts: its away part)
.venv/bin/python tools/uniform_preview.py --base LAR --helmet-side h.png --jersey j.png --numbers n.png
```

Other options: `--number 7` (default 18), `--target player|helmet|torso|legs`, `--size 600x800`.

## What the sheet shows

- **Player:** front, three-quarter, left side, back and right side, with the game's texture filtering.
- **Helmet:** the left side, then the right side. Each is shown at game size, then at 4x with nearest-neighbour texels
  so you can judge single pixels.
- **Numbers:** the back and front of the torso, the jersey texture with the number drawn in as the game builds it, and
  the numbers strip.
- **Pants:** both sides of the legs.
- **Textures:** each part exactly as the importer stored it. The helmet side is in the game's stored layout (rows 14-29
  flipped), not as you painted it.
- **Reference** (`--reference`): the NFL base team's player, for comparison.
- `--html`: a self-contained WebGL page. Drag to orbit, use the wheel to zoom, pick the set from the menu; buttons
  jump to the standard views and the helmet sides.

## Helmet art

The helmet side texture is 32x32. Its **left half wraps one side of the helmet and its right half the other**. The
"Helmet, left side" panel is the player's left (facemask pointing left in the picture). A logo in profile (an animal
or a bolt) must face the **front** on both sides. Painted "as on the helmet", that means its head points to the
**outer edge** of each half: left in the left half, right in the right half. Lettering must read the same way in both
halves. The band that shows on the side of the shell is rows 14-29. A logo drawn above or below that band lands on
the crown or the brim.

## What it is

- **Conversion:** the PNGs go through the real importers into a scratch folder (`tools/make_league_pak.py` for league
  teams, `tools/import_uniform_sets.py` for NFL sets), so flips, half orientation and the 16-colour reduction show as
  they will in game.
- **Numbers:** the numbers are drawn into the jersey as `main func_80256580` does it. Number colours take jersey
  palette entries 13-15, so a jersey that uses those entries changes colour in game.
- **Model:** `build/uniform_mannequin/mannequin.json` is the team-select QB, taken from an RDRAM dump of your own game
  by `tools/make_mannequin.py`. It holds the meshes, UVs, the RDP tile of every draw, and which part each texel
  shows. It is derived from the ROM, so it stays in `build/` and is never committed.
- **Making it again:** start a test instance, go to team select, run `tools/live.sh dump`, then
  `.venv/bin/python tools/make_mannequin.py <app folder>/dumps/rdram_<t>.bin`.
- **ROM files:** it reads `build/teamplus.z64` or `build/baserom.z64`, or the copies in `build/appdata2026`, plus the
  design kit in `build/league_pak_design_kit`.
- **Check against the game:** take the dump and a screenshot together (`tools/live.sh dump; tools/live.sh shot x`),
  then run `... --compare build/shots/live/x.png --mannequin <that mannequin>`. It draws the game, the previewer and the
  dump's own textures side by side.

## Where this stands (2026-11-28)

Merged into 1.0.6 (06e336f; the `uniform-preview` branch is superseded). It reads a built pak's sets with their
`uv_overrides` and per-cap textures (docs/design/SEASON_PAK.md); the `--html` page shows cap textures, not new
coordinates, so check coordinates in the game or with `tools/dump_views.py`. It is a first look, not a replacement for
seeing the set in game next to the stock one.
