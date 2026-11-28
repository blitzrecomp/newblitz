# The 2026 Season pak

2026-11-13. The public, open-source New Blitz carries no licensed NFL content. One download, the **2026 Season pak**,
adds it back: the 2026 rosters and weekly lineups, uniforms, team art, VS helmets, the announcer's team calls, Houston
and Pro Bowl. The public app plus the pak plays exactly what a private build plays. If the server goes away, the app
still works: Original 1999 as always, and New Blitz without the season (the cartridge's teams, with every feature that
doesn't need 2026 content).

## What's where

| | Private build (`make app-modern`) | Public build (`make app-modern BLITZ_CONTENT=public`) |
|---|---|---|
| Roster pack, snapshot, team art, uniforms, VS helmets, announcer, Houston, Pro Bowl | in Resources | not in the app; the pak |
| The '27 logo overlays (`art/logo27`), `art/files/blitzlog.wms`, the credits page | in Resources | in Resources (ours) |
| Live-rosters address (`rosters/hub`, `token`, `repo`) | from `build/rosters_*` | none |
| Season server address (`paks/server`) | from `build/pak_server`, when there | from `build/pak_server` |
| League paks (CFL 2026, Pac-12 2026) | not bundled; catalog paks like the season | not bundled; catalog paks |
| Device kits (`make device-kits`) | built from private device builds | build ps5-/android-/ios-modern with `BLITZ_CONTENT=public` first |

The public Mac app builds in `build/app_modern_public`, so a private build beside it is untouched. `BLITZ_CONTENT` is
passed to every Modern target (win, ios, android, linux, ps5) and to CMake (which then bundles no roster pack).

One list feeds both a private build's Resources and the pak (`SEASON_PARTS` and `SEASON_APP_FOLDER` in the Makefile,
plus `packs/blitz2026`, `build/blitz-rosters` and `build/engine32/houston`), so the two can't drift.

## The pak

`make pak-2026` (tools/make_season_pak.py) writes `build/paks/season2026.nbpak`, the same file as
`season2026-<version>.nbpak`, and `build/paks/catalog.json`. About 1.8 MB, 1,513 files. The zip is deterministic (sorted
entries, fixed times): the same content gives the same SHA-256.

```
pak.json             format blitz-pak/1, id season2026, version (the roster snapshot's time), min_build,
                     headshots (on), and every file's SHA-256
LICENSE-CONTENT.txt  unofficial, not affiliated, personal use
packs/blitz2026/     the roster pack (its id stays blitz2026: crews, Pro Bowl and Settings test it)
rosters/             manifest.json, players.json, lineups/*.json (no headshots, no hub or token)
uniforms/ art/files/ art/vs_right/ announcer/ houston/ probowl/
```

`tools/make_season_pak.py check <file>` runs the same checks as the app.

## In the app

- **Content roots** (src/game/content_dirs.cpp, season_pak.h): `content_dirs()` is Resources, then
  `<app folder>/paks/season2026/` when a valid pak is there. Every loader reads them in turn and the app folder last, so
  a player's own add-on still wins. Sites: the roster pack and its built-in season (content_pack_tp.cpp), art/files and
  the announcer (art_dropin.cpp), Houston and the VS helmets (engine32.cpp), uniform sets (uniform_picker.cpp), Pro Bowl
  art (crew.cpp), Export ROM's announcer files, Settings' pack choices, and the roster snapshot seed (rosters_live.cpp).
- **Same bytes either way**: art files are applied in name order and Houston, VS helmet and Pro Bowl files are added in
  name order, so where a file came from (Resources or the pak) never changes the game's file table. A folder listing's
  order is the disk's own; it used to decide this.
- `season_content()`: the season is here (bundled or the pak). Without it:
  - Advanced Season (the default) steps aside to Standard for the run, quietly (adv_mode.cpp `latch`).
  - The 32nd team is a neutral stand-in, "Expansion", in the Titans' art (engine32.cpp). It keeps slot 31 and the HOU
    id inside the app, so the 32-team Season, saves and Export work the same with and without the pak. No club name
    is built into the app for it any more: Houston's names come from the pak's houston/houston.json. (Thirty-one teams
    would change the Season's layout and saves when the pak is added.)
  - Pro Bowl (R1 at team select) needs the Blitz 2026 pack in play (uniform_picker.cpp).
  - Hidden: Lineups, Roster Updates, Roster Room, Season Mode, Uniform Picker, Unpicked Uniforms, Pick Your Crew, and
    Export's Rosters, 2026 Season and Every Uniform Set; the Export screen offers Original 1999 (and Custom).
- **Install** (season_pak.cpp): size cap; whole-file SHA-256 against the catalog (downloads); entry names (no absolute
  paths, `..`, backslashes, drive letters; only the pak's folders); entry count and unpacked size caps; pak.json format,
  id and min_build; every file against pak.json's list (and nothing unlisted); the roster pack parses. Unpacked to
  `paks/.incoming-*`, then swapped in with two renames (a failure puts the old one back). The verified zip is kept as
  `paks/season2026.nbpak` for Send to Device. The roster snapshot is copied into `<app>/rosters` when there's none
  there or the pak's is newer. No restart when it goes in before the game starts; Add-Ons says "Restart to use it" in
  a game.
- **Blitz Builder** (builder.cpp): when the season isn't here, the first question is "Add the 2026 season?" [Add It]
  [Just the 1999 Game] instead of "Which Blitz?". Add It downloads and installs, then asks "Keep rosters up to date?"
  (Settings → Teams → Roster Updates). A failure (offline, no server, a bad checksum) says why, plays the 1999 game,
  and points to Add-Ons.
- **Add-Ons** (addons.cpp): a SEASON row with Remove when it's installed; Add… takes a `.nbpak` (or a zip with pak.json
  at its root). Since 1.0.6 a row with Get for each posted pak that isn't in, and Check (top right) in place of the "Get
  the 2026 Season" / "Update 2026 Season" button (below).
- **Open Folder** (1.0.6): a computer's file browser, as before. iPhone: the Files app at the app's Documents
  (`shareddocuments://`; Files → On My iPhone → New Blitz), which is the drop folder: a ROM or a `.nbpak` put there goes
  in at the next start (the app folder itself is in Application Support, which Files can't show; the message says so and
  points to Add… for the rest). Android: the Files app at the app folder through the app's own documents provider
  (`BlitzFolder.java`, authority `<package>.folder`), which also lists "New Blitz" in the Files app's sidebar and every
  picker (Android 11+ hides `Android/data` from file managers); when no app takes it, the message says where it is (☰
  → New Blitz, or the path). Hidden on the PS5 only, where nothing can open it: its folder is reached over FTP.
- **Roster Updates** (`live_rosters` in season.json): no roster request at all while it's off. On by default only in
  a build that carries its own source (a private build); the public app asks. With the pak, the address is the
  catalog's (`live_rosters.hub`, under the season server), never the private hub key. The GitHub-token source reads its
  repo from `BLITZ_ROSTER_REPO` or `rosters/repo` (`build/rosters_repo`): no account is named in the source.
- **Headshots** (crew.cpp): ESPN photos only in a private build, or with the pak when its pak.json says `headshots`
  and the server's catalog hasn't switched them off. Otherwise no request leaves the app; the cards show Blitz
  portraits.
- **Devices**: Send to iPhone / Android / PS5 puts `season2026.nbpak` in the device's drop folder beside the ROM; the
  app installs it at its next start, before the game (`paks_at_launch`), then removes it there. A pak copied there by
  hand works the same. A pak that fails is renamed `.failed` with a notice.
- Dev hooks: `BLITZ_PAK_SERVER=<url>` (the server; "" for none), `BLITZ_PAK_INSTALL=<file>` (as Add… does: the player's
  own), `BLITZ_PAK_DOWNLOAD=1`, `BLITZ_TEST_METERED=1` and `BLITZ_TEST_METERED_LIMIT=<bytes>` (1.0.6).
- Since 1.0.6 this covers every pak, not just the season: see "Every pak, by itself" below.

## The server

The rosters post office Worker (tools/rosters_hub_worker) has public routes under `/paks/` that neither need nor show
the hub key: `catalog.json`, `<name>.nbpak` (cached for good: versioned names), and `rosters/<path>` (the weekly roster
files for pak installs that opted in, only while the catalog allows). Uploads: `PUT /upload/paks/...` with the existing
upload secret (`tools/publish_pak.sh`: the pak first, then the catalog, which the Worker refuses if it names a pak that
isn't uploaded). KV holds up to 25 MiB a value; R2 is the move if paks grow.

The catalog (`blitz-paks/1`) is the kill switch: drop the season2026 entry or set `"available": false` and no new
installs happen (installed copies keep playing, with headshots and live rosters off). `"headshots": false` and
`"live_rosters": false` in the entry switch those off for everyone. The app reads the catalog at install, and at launch
only when something uses it (live rosters on, or headshots allowed by the pak): with both off, no request at all.

Tested locally only: `node test/worker_test.mjs` (in-memory KV; `make test` runs it) and the app against a local
`python3 -m http.server` serving a catalog and a pak.

## Every pak, by itself (1.0.6)

The team, 2026-11-27: "i think we should just have these autoload." Since 1.0.6 every pak the catalog lists installs and
updates itself, the season and the league paks (Pac-12, CFL) alike, and Add-Ons has a Check button.

**When.** At every start (`paks_at_launch`, in the background) while Settings → Teams → **Keep Rosters and Uniforms Up
to Date** is on. It's on by default everywhere now (before, a public build had it off until Blitz Builder asked), it's
shown in both games, and first run's "Updates and reports" asks it wherever there's a server. Off: nothing downloads by
itself (only the catalog is read, for the season's headshot switch, as before); Check still works when pressed.

- A pak that isn't in: downloaded and installed. Before the game starts it goes straight in; once a game is running it
  waits as `paks/<id>.update.nbpak` and goes in at the next start (never under a running game). A toast says what came.
- A pak that's in and the catalog's SHA-256 differs from the installed one's: the same, as an update.
- **Left alone:** a pak the player installed by hand (Add…; `paks/<id>.origin.json` says `"from": "file"`), a pak the
  player removed (`paks/removed.json`; Get or Add… brings it back), an entry with `"auto": false` (offered, not pushed:
  Get and Check take it), an entry or pak.json whose `min_build` is newer than the app.
- **Metered networks:** on Android's metered network (mobile data, a hotspot) or an Apple device's expensive or Low Data
  path (`network_metered`, blitz_host.h), the automatic downloads take at most 10 MB a start (`auto_metered_limit`); the
  rest wait for Wi-Fi and Add-Ons says so on the row ("Waits for Wi-Fi … Get downloads it now"). Check and Get always
  download. Computers and the PS5 don't ask (false).
- **Progress:** Add-Ons shows the running sync under its buttons ("Downloading Blitz: Pac-12 (2 of 3)… 0.6 MB of 1.4
  MB" and a bar). Macs and iPhones count bytes (NSURLSession), Android too (`BlitzActivity.httpGetProgress`); Windows,
  Linux and the PS5 show the step and fill at the end.
- Turning the setting on (Settings, or first run's box) starts a sync at once.

**Add-Ons.** The paks come first: each installed one (SEASON, LEAGUE or the catalog's kind) with Remove; then each the
catalog offers that isn't in, with **Get**. **Check** (top right) reads the catalog now and downloads what's new or
changed, by hand (so also `"auto": false` ones, also on mobile data), and says how it went: "Up to date.", "Added
Blitz: Pac-12.", "Blitz: Pac-12 is downloaded: in at the next start." (an update while a game runs; Restart is offered
on a computer), "Couldn't check for new paks: the New Blitz server can't be reached." A new pak Get or Check brings in
while a game runs goes in at once, as Add… always did ("Restart to use it").

**Installed state** in `<app folder>/paks/`: `<id>/` (the pak), `<id>.origin.json` (`from`: `catalog` or `file`, and the
zip's `sha256`), `season2026.nbpak` (the season's zip, kept for Send to Device), `<id>.update.nbpak` + `.update.sha256`
(waiting for the next start), `removed.json`, `catalog.json` (the last catalog read). A pak installed by 1.0.5 has no
origin file: it counts as the server's (1.0.5 updated it whatever its source), with its kept zip's SHA-256.

**Upgrading from 1.0.5:** a public 1.0.5 that never ticked Roster Updates saved `"live_rosters": false`. Such a player
is asked once, on the launcher before the game (never over one): "Keep rosters, uniforms and new paks up to date?" Yes
turns it on and syncs at once; No leaves it off and it's never asked again (Exit counts as No). Once per app, not per
version (General's hidden `content_ask_done`); never on a fresh install (setup's Updates and reports asks it); settled
for good by any start that finds the setting on; not on the PS5. When Cloud Sync's "Playing on multiple devices?" is due
at the same start, this one comes first and Cloud Sync's after it, one screen at a time. Rule: src/game/content_ask.h,
`make test-content-ask` (tests/content_ask_test.cpp). Shot: build/shots/live/content_ask.png.

### The catalog (format unchanged: `blitz-paks/1`; new optional fields)

```json
{
 "format": "blitz-paks/1",
 "paks": {
  "season2026": {"title": "2026 Season", "version": "2026.11.14.0100", "file": "season2026-2026.11.14.0100.nbpak",
                 "size": 1784633, "sha256": "…", "min_build": 593, "kind": "season",
                 "headshots": true, "live_rosters": true},
  "pac12":      {"title": "Pac-12 2026", "version": "2026.11.28.2", "file": "pac12-2026.11.28.2.nbpak",
                 "size": 0, "sha256": "…", "min_build": 994, "kind": "league",
                 "summary": "Eight 2026 Pac-12 schools: Arcade and Tournament"},
  "cfl":        {"title": "CFL 2026", "version": "2026.11.28.2", "file": "cfl-2026.11.28.2.nbpak",
                 "size": 0, "sha256": "…", "min_build": 981, "kind": "league",
                 "summary": "The nine CFL clubs: Arcade and Tournament"}
 },
 "live_rosters": {"hub": "rosters/"}
}
```

- **Keys** are pak ids: 1–32 of `a-z 0-9 _` (the folder, the file's prefix; the Worker's name rule already allowed any).
  Every entry is a pak the app may install; 1.0.5 and older read only `season2026`, so other entries are harmless there.
- Required as before: `file` (relative to the catalog, or a full address), `sha256`. Used: `title`, `version`, `size`
  (progress and the metered allowance), `min_build`.
- New, optional: `kind` (Add-Ons' label: `season`, `league`, …), `summary` (the row's line before it's installed),
  `auto` (default true; false: offered with Get, never pushed by itself), `available` (false: withdrawn, as before).
- The season's switches (`headshots`, `live_rosters`) and the top-level `live_rosters.hub` are unchanged.

### A league pak

The same `blitz-pak/1` zip, any id. It may hold `packs/` (its roster pack, with its own art in `packs/<id>/files`,
which plays only while the pack does), `uniforms/`, `art/`, `announcer/`; never `rosters/`, `houston/` or `probowl/`
(the season's alone: one roster snapshot, one Houston, one Pro Bowl). Every `packs/<x>/pack.json` must be a
`blitz-pack/2` named `<x>`. Installed paks are content roots after the season, in id order (content_dirs.cpp), so
Settings → Teams → Rosters lists the league's pack (shown: the pak's title, e.g. "Pac-12 2026", beside Auto, 2026 Season, ROM's Own; since 1.0.6 Settings → Teams → League picks it as the whole league). Art in
a league pak's own `art/` would cover every game's: keep it under the pack.

```
make pak-league ID=pac12 TITLE="Pac-12 2026" PACK=packs/pac12 VERSION=2026.11.28.2 MIN_BUILD=<release build> SUMMARY="..."
make pak-2026                      # the season; both put their entry into build/paks/catalog.json, keeping the other
HUB_UPLOAD_SECRET=... tools/publish_pak.sh   # every pak the catalog names that's in build/paks, then the catalog
```

`tools/make_season_pak.py check <file>` checks any pak as the app does. League paks default to `min_build` 950 (the
first build that reads them); set `--min-build` when they need a later one (`MIN_BUILD=` with make). Keys and the
first build that reads each: `uv_overrides` 968, per-cap `caps` 981, `cart_files` 994. **When posting a pak for a
release, set `MIN_BUILD` to the release build** (docs/RELEASE.md, "Posting paks"). A league pak plays Arcade and
Tournament only (a modes list in its pack.json, key name (unverified)); Season, Advanced and the Play Editor stay the NFL's.

**Per-set texture coordinates** (2026-11-28, `src/game/uv_overrides.cpp`): a league's pack.json may carry
`"uv_overrides"` tables, and a set (a team's `"uv_override"` home/away, an alternate's `"uv_override"`) one of them plus
its own texture. Players wearing that set draw the shoulder with that texture and the table's coordinates on the player
models it lists; every other player, model and set keeps the stock texture with the stock coordinates. A pak with them
needs build 968 or later (older builds refuse the key). `tools/uv_override.py` makes them from a design table (the
Pac-12's `stripe_review/dev_candidate`), after the roster, art and uniform steps:

```
O=<pak folder>/packs/pac12; A=<handoff>/league_pak_art_out/pac12
.venv/bin/python tools/league_roster.py --research <handoff>/league_pak_research/pac12 --pack $O/pack.json --version 2026.11.28.2
.venv/bin/python tools/make_league_pak.py --art $A --out $O
.venv/bin/python tools/make_league_pak.py --uniforms $A --kit build/league_pak_design_kit/pac12 --out $O
.venv/bin/python tools/uv_override.py --table <handoff>/stripe_review/dev_candidate/shoulder_uv.json --pack $O
.venv/bin/python tools/make_league_pak.py --cart <handoff>/cart_safe_shoulders --out $O
make pak-league ID=pac12 TITLE="Pac-12 2026" PACK=$O VERSION=2026.11.28.2 MIN_BUILD=994 \
     SUMMARY="Eight 2026 Pac-12 schools: Arcade and Tournament"
```

**Per-cap textures** (2026-11-28, the CFL's right sleeves): the stock model maps one shoulder texture onto both upper
arms in opposite directions, so lettering that reads on one arm reads backwards on the other. A table may name its
caps by the bone matrix each cap's draw is placed with (`"caps": {"right": "0B000300", "left": "0B000340"}`, the same
on all six player models), and a set's `"uv_override"` may give a cap its own texture: `{"table": "shoulder_sleeves",
"caps": {"right": "edm_shr_h.wms"}}` ("file" is then optional: without it the rest of the part keeps the set's own
texture). The set's list loads the cap's texture just before that cap's G_MTX and the set's own again after it. A
table whose `"uv"` equals `"st"` keeps the stock coordinates (no vertex copies). Sets without an override are drawn as
before; the Pac-12's `"file"`-only overrides are unchanged. Needs build 981 or later (older builds refuse the key). The **CFL pak** is built this way, from the design agent's
handoff (`--league cfl --handoff … --research … --sleeves …`); the low-detail player models (two of the six) have no
cap table and keep the stock shoulders (docs/STATE.md, "Known issues").
`tools/uv_override.py --sleeves <list>` writes the table and binds the companions; the CFL importer runs it:

```
H=<handoff>; O=build/league_paks/cfl
.venv/bin/python tools/make_league_pak.py --league cfl --handoff $H/build/league_pak_art_out/cfl \
    --research $H/build/league_pak_research/cfl --sleeves $H/component_audit/right_arm_assets.json
make pak-league ID=cfl TITLE="CFL 2026" PACK=$O VERSION=2026.11.28.2 MIN_BUILD=981   # then MIN_BUILD = the release build
```

The list gives per set "team", "set", "optional_right" (a PNG relative to its "optional_paths_relative_to") and
"right_sha256" (checked). `tools/uniform_preview.py <pack folder> --team EDM` previews a built pak's sets with their
overrides (cap textures and coordinates; the `--html` page shows the cap textures, not new coordinates).

**Cart textures** (`"cart_files"`, Export ROM): an exported cart runs the game's own code, so a set's uv_override can't
come along; the cart would show the set's own (stock-coordinate) shoulder file. A set's `"cart_files"` (the team's
`{"home": {"shoulder": <file>}, "away": ...}`, an alternate's `{"shoulder": <file>}`) names the `files/added` texture
the cart wears for that part instead: drawn for the stock coordinates. The app keeps drawing the set's own files and
its uv_override. `make_league_pak.py --cart <manifest>` (a JSON of `{"sets": [{team_id, set_id, file, sha256}]}`,
e.g. the Pac-12's `cart_safe_shoulders.json`) checks each PNG's SHA-256, encodes it at the set's own part's size and
format as `<own file>c.wms` and writes the keys. Generic: any part, any league (the CFL's right sleeves later). A pak
with them needs build 994 or later.

**League carts** (Export ROM, 1.0.6; src/game/export_rom.cpp, tools/rom_picker/*.asm): a league's 2-30 teams repeat
through the cart's 31 slots (slot s holds team s mod n, as engine32.cpp does in the app); each team's own slot is its
highest (22-30, the ones the game's 9-entry right-helmet list can name); the pak's CPU ladder moves to those slots;
team select steps over N teams so each shows once; the main menu wraps over Arcade (and Tournament) and blanks Season
and Play Editor; no Houston. The file is named "New Blitz <PAK ID> ...". Checked in ares with the CFL and Pac-12
paks, never on a cartridge. Known gaps: docs/STATE.md.

`tools/uvo_check.py <dump> --pack $O` lists, for a dumped frame, each player's model and lists and checks every set's
caps against the pak; `tools/dump_views.py <dump>` draws a dumped team-select player front, back, overhead and sides.

**Tested (Mac, 2026-11-27)** against a local catalog (`BLITZ_PAK_SERVER=http://127.0.0.1:<port>/`, python's http.server
and a slowed copy for the progress bar), never the live one: a fresh public app autoloaded the season before the game
started; Pac-12 came by itself at the next start and showed under Rosters; Check said "Up to date."; a newer Pac-12 by
Check during a game waited and went in at the next start (toast "Blitz: Pac-12 updated"); Remove kept it out at the next
start and Get brought it back; a Pac-12 added by hand (`BLITZ_PAK_INSTALL`) wasn't replaced; a metered network
(`BLITZ_TEST_METERED=1`, `BLITZ_TEST_METERED_LIMIT=<bytes>`) left the season for Wi-Fi; with the setting off nothing
downloaded at start and Check still did; an unreachable server said so. Screenshots: build/shots/live/ (addons1,
addons2, check_uptodate, check_progress, check_staged, removed, get_done, metered_wifi, off_check, offline_check).

## For the owner

Deployed since 2026-11-14: the Worker (season server, address in `build/pak_server`; release builds pick it up), the
catalog and the 2026 Season pak. Weekly roster files are served under `/paks/rosters/` while the catalog allows.
Releasing paks: docs/RELEASE.md, "Posting paks". Private builds keep `build/rosters_hub`, `rosters_token` and
`rosters_repo`, which a public build leaves out and the release scan checks for.
Decisions still open: whether headshots stay on in pak.json; signing the catalog (Ed25519) as later hardening. The
pak's version scheme is the roster snapshot's time (`--version` overrides); league paks use `yyyy.mm.dd[.n]`.

## Not done / known gaps

- Team names in crew.cpp (crew panel titles) and roster_room.cpp are still in the binary (shown only with the pak);
  content_pack_tp.cpp's nickname→franchise table is how the cartridge's own teams are recognised.
- PSP: `make psp-kit` still merges a private build's content; a public PSP kit would read the pak instead.
- Windows: `Send to Android/iPhone` doesn't run there (as before); the public Windows zip is untested.
- Device installs from the drop folder: built, not run on hardware.
