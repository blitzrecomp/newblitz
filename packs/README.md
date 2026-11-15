# Content packs

A content pack changes rosters and team details without touching the ROM. The game reads its team and player tables from the menu overlay; when that overlay loads, the app writes the pack's values into those tables. Menus and gameplay both use the result.

**Layout:** a pack is a folder with a `pack.json` file. Format `blitz-pack/1`; the example is in `examples/blitz2026-proof/`. That is Classic Blitz's format. Modern Blitz takes `blitz-pack/2` (`docs/BLITZ2026.md`), keeps its packs under `~/Library/Application Support/Blitz2026Recompiled/packs/` and ships with the built-in `blitz2026` pack.

## Using a pack

1. Copy the pack folder into the app's data folder:

   ```
   ~/Library/Application Support/BlitzRecompiled/packs/<pack-id>/pack.json
   ```

2. Put the pack id on the first line of `~/Library/Application Support/BlitzRecompiled/packs/active.txt`.
3. To go back to Classic, empty `active.txt` or delete it.

For development, the environment variable `BLITZ_PACK=<folder or pack.json>` overrides `active.txt`.

Each pack keeps its own Controller Pak under `saves/packs/<pack-id>/`, so seasons saved under one roster never mix with another. Classic saves stay in `saves/`.

If a pack has a mistake, the app tells you what's wrong and plays Classic. It also refuses a pack whose `expect` values don't match the game: for example, replacing a player who isn't the one the pack thinks is in that slot.

## Writing a pack

```
tools/pack.py show ARZ                       # a team's ratings and roster, read from your ROM
tools/pack.py template ARZ --id my-pack      # a starter pack with every current value filled in
tools/pack.py validate path/to/pack          # the same checks the game runs
```

### Teams

`team` is 0-30 or the 1999 abbreviation: `ARZ ATL BAL BUF CAR CHI CIN CLE DAL DEN DET G.B IND JAC K.C MIA MIN N.E N.O NYG NYJ OAK PHI PIT S.D S.F SEA STL T.B TEN WAS`.

| Field | Values | Notes |
|---|---|---|
| `ratings.passing`, `rushing`, `linemen`, `defense`, `special_teams` | 0-5 | The team-select bars. In gameplay, only special teams is used. |
| `city` | up to 31 characters | VS screen text, season text |
| `city_upper` | up to 15 characters | The team-select list. If left out, the upper-cased `city` is used. |
| `city_abbr` | up to 3 characters | Scoreboard |

The VS-screen city and team banners, logos and helmets are images, so a pack doesn't change them. In Modern Blitz the art drop-in does (`docs/BLITZ2026.md`).

### Players

Each team has 16 roster slots with fixed roles:

| Slots | Role |
|---|---|
| 0-2 | OL (no names) |
| 3 | QB |
| 4 | RB |
| 5-6 | WR/TE |
| 7-9 | DL (no names) |
| 10-13 | LB/DB |
| 14 | K |
| 15 | unused |

| Field | Values | Notes |
|---|---|---|
| `slot` | 0-15 | |
| `expect` | `{last, first, number}` | Optional guard: the pack applies only if the slot holds this player |
| `last` | up to 15 characters, letters, space, `. ' -` | Shown in capitals. Keep it to 12 characters or fewer so it fits the labels. |
| `first` | up to 15 characters | Stored, but the game never displays first names |
| `number` | 0-99 | Jersey number on the field, at team select and on name tags |
| `size` | 0.90-1.30 | Body scale (vanilla 1.05) |
| `skin` | 0-5 | Skin shade, light to dark: 5 Lighter, 1 Light, 2 Light-medium, 3 Medium, 4 Medium-dark, 0 Darker (see below) |
| `body` | 0 or 1 | Heavier build |
| `announcer_call` | `none` / `keep` | Renamed players must use `none` (the default) so the old name isn't announced |

The N64 game has no per-player ratings; only team ratings exist.

**Skin shades.** The game itself has two: 1 (Light) and 0 (Darker), its own two sets of arm and face textures. 5 Lighter, 2 Light-medium, 3 Medium and 4 Medium-dark are drawn by the app (`src/game/skin_tones.cpp`) by recolouring those textures from your own ROM's skin colours, so they need this app (an older build refuses a pack that uses them). Both builds accept them (`blitz-pack/1` and `/2`). Each shade's arm colour at team select is about: Lighter #e3c3a8, Light #915b36, Light-medium #865333, Medium #7a4a30, Medium-dark #6e422e, Darker #633a2b.

### Season (2026 build, Advanced mode)

A `blitz-pack/2` pack can carry the league for Advanced seasons (Settings → Season → Season Mode → Advanced):

```json
"season": {
  "year": 2026,
  "divisions": [{"name": "AFC East", "teams": ["BUF", "MIA", "NE", "NYJ"]}, ...],
  "schedule": [[["SF", "LAR"], ["PIT", "ATL"], ...], ...]
}
```

| Field | Rules |
|---|---|
| `divisions` | 8 divisions in this order: AFC East, North, South, West, then the same for the NFC. Each has 4 franchise ids, and every franchise is in exactly one division. |
| `schedule` | Up to 18 weeks. Each week is a list of up to 16 `[away, home]` games, and no team plays twice in a week. |

Updating a season means updating this section, along with the rosters.

### League paks (2026 build): your own league, Arcade only

A pack with a `"league"` section replaces the NFL with its own teams: 2-31 of them, played in Arcade (and Tournament or the Play Editor if it says so). Season is off, and so is Advanced. `tools/make_league_pak.py` builds one (its example is an eight-school college league). Install it with Settings → Add-Ons → Add… and pick it in Settings → Teams → Rosters.

```json
"league": {"name": "Pac-12", "modes": ["arcade"],
           "ladder": [["USU", "TXS"], ["CSU", "SDS"]], "ladder_done": ["BSU", "WSU", "FRE", "SDS"]},
"teams": [{"id": "BSU", "base": "DEN", "city": "Boise State", "nickname": "Broncos", "select": "Boise St.",
           "abbr": "BSU", "ratings": {...}, "players": [{"slot": 3, "last": "Hendrix", "number": 7}, ...]}]
```

| Field | Rules |
|---|---|
| `league.modes` | `arcade` (always), `tournament`, `play_editor`. Season never: the main menu greys it and the cursor skips it. |
| `league.ladder` | Optional. The 1-player CPU's opponents, easiest tier first: up to 9 tiers of 1-4 teams. Default: two at a time in pack order. `ladder_done`: the 4 picks once every team is beaten. |
| `id` | 1-4 letters, not an NFL franchise's id. |
| `base` | The NFL franchise the team is built on: CPU plays and skill, uniforms, 3D models and the art the pak replaces. Give each team its own base. |
| `city`, `nickname`, `abbr` | Required. `select` (the team select list) defaults to the city. |
| `announcer` | `none` (default: no calls, so a "Broncos" school isn't called Denver) or `base`. |
| `uniform` | Optional `{"home": "PIT.home", "away": "PIT.away"}`: another franchise's uniform. `clashes_with` lists teams whose jerseys look alike. |
| `vs_helmet_right` | Optional: a `files/added/` image (at most 14 characters) for the right side of the VS screen, facing left, instead of the left helmet mirrored (which reverses a logo). `files/added/` images are new game files, not replacements. |
| `uniform_files` | Optional: the team's own uniform textures in `files/added/`, by part and side (`"jersey_home": "bsu_jer_h.wms"`; parts helmet_side, helmet_stripe_front, helmet_stripe_back, facemask, shoulder, jersey, pant_stripe, pants, socks, numbers). Each must match the base part's size and format. A team with its own jersey clashes only with the teams in its `clashes_with`. `make_league_pak.py --uniforms` converts finished PNGs into a pak and works out `clashes_with` from the jersey colours. |
| `cpu_skill`, `ratings`, `players` | As above. The base team's players are cleared; name at least slot 3 (the quarterback at team select). |

**Art:** `files/*.wms` in the pack folder replace game images by name, only while the pack is active. Name them after the base team's files (its logos, city and team titles, win text, VS helmet, end zones, helmet decal): no NFL team plays in a league pak, so nothing else changes. `tools/make_league_pak.py` reads those names and sizes from your ROM and draws every image itself.

Each pack keeps its own saves (`saves/packs/<id>/`).

**Real college rosters, for personal use:** `tools/college_roster.py --pack "<app folder>/packs/pac12"` fills each school's 15 slots with this season's starters from ESPN (recent games' starters, ranked by yards and tackles; the kicker from the roster). It writes only to an app folder, never to the repo: players' names belong to them, so the bundled pak keeps fictional players. Run it again during the season to refresh; a shade you set for the same player stays. `make_league_pak.py --art` keeps an existing pack.json's players.

## Licensing

Packs in this repo must contain only your own data. Don't commit art, rosters or files taken from other people's ROM hacks. See `docs/CONTENT_MAP.md` §13.

## Quick refresh from ESPN (`make roster-update`)

`tools/roster_fetch.py` reads ESPN's public roster and depth-chart JSON for every team and fills each team's 15 player slots with healthy starters: OL (tackles and center first), QB, RB, two receivers, three DL (3-4 teams: edge outside linebackers count as DL), four defenders and the kicker. A player on a reserve list (injured reserve, PUP, suspension) is replaced by the next man on the depth chart; one who is only out for this week keeps his slot. A slot whose current player is still a healthy starter is left alone, so only real changes show up. Each slot's heavier build (`heavy`) is set from ESPN's position and weight, as the original game hands it out (linemen, linebackers and tight ends; backs over 225 lb, receivers over 230, DBs over 225; never QBs or kickers). Without `--apply` it prints the changes with a reason for each; `make roster-update` applies them through `tools/roster_pack.py build` (changelog, validation, version stamp). Team ratings are not touched. ESPN's site API is unofficial: use it for an occasional refresh, not on a schedule that hammers it.


## Editing by hand: Roster Room

Roster Room (Settings → Teams → Roster Room) opens in your browser, served by the app on your own computer, and sets any
team's default crew: who plays each of the 15 slots, over this week's lineup. **Lock In** saves the crews; they play
from the next game, in every mode, and go into an exported ROM. To share rosters with others, make a pack instead
(above).

A later `make roster-update` can replace a slot when ESPN's healthy starter there is someone else.
