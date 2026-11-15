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
| Pac-12 league pak | when in the checkout | not bundled |
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
- **Add-Ons** (addons.cpp): a SEASON row with Remove when it's installed; "Get the 2026 Season" when it isn't, "Update
  2026 Season" when the catalog has a newer one; Add… takes a `.nbpak` (or a zip with pak.json at its root).
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
- Dev hooks: `BLITZ_PAK_SERVER=<url>` (the server; "" for none), `BLITZ_PAK_INSTALL=<file>`, `BLITZ_PAK_DOWNLOAD=1`.

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

## For the owner

1. Deploy the Worker (`npx wrangler@4 deploy` in tools/rosters_hub_worker) or a separate public Worker with the same
   code, and decide the public host name. Nothing has been deployed.
2. Put the public base (`https://<host>/paks/`) in `build/pak_server`; release builds pick it up.
3. `make pak-2026`, then `HUB_UPLOAD_SECRET=... tools/publish_pak.sh`.
4. Release builds: `make app-modern BLITZ_CONTENT=public` (and the device builds with it before `make device-kits`).
   Make sure `build/rosters_hub`, `rosters_token` and `rosters_repo` don't end up in anything public (the public build
   leaves them out).
5. Decisions still open: whether headshots stay on in pak.json; the pak's version scheme (default: the roster
   snapshot's time; `--version` overrides); signing the catalog (Ed25519) as later hardening.

## Not done / known gaps

- Team names in crew.cpp (crew panel titles) and roster_room.cpp are still in the binary (shown only with the pak);
  content_pack_tp.cpp's nickname→franchise table is how the cartridge's own teams are recognised.
- PSP: `make psp-kit` still merges a private build's content; a public PSP kit would read the pak instead.
- Windows: `Send to Android/iPhone` doesn't run there (as before); the public Windows zip is untested.
- Device installs from the drop folder: built, not run on hardware.
