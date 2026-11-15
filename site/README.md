# The New Blitz 2027 website

Static HTML and CSS with one small script, no third-party requests, no trackers. The house style is the app's own
(Blitz '27: docs/design/redesign/tokens.json): navy to purple grounds, cyan grid panels with chrome corners, Barlow
Condensed ExtraBold Italic caps in yellow, one orange-to-yellow flame.

## Pages

`index.html` (home), `download.html`, `start.html` (getting started, per device, controls), `features.html`,
`season.html` (the 2026 Season and paks), `contribute.html` (roster packs, uniform sets, ports, agent prompts;
its `<span data-text="repo">` spans get the repo address from site.json), `faq.html`, `press.html`, `privacy.html`, `legal.html` (credits, licences,
trademarks), `404.html`. Plus `sitemap.xml`, `robots.txt` (both written by the build) and `_headers` (Cloudflare Pages:
CSP, caching).

## Preview

```sh
cd site && python3 -m http.server 8642     # then http://localhost:8642/
```

Links are root-relative (`/assets/...`), so serve `site/` as the root.

## Settings: site.json

| Key | What |
|---|---|
| `repo` | The public GitHub repository. Every Download button points at `<repo>/releases/latest`; source, issues and docs links follow it. Placeholder until launch: `https://github.com/blitzrecomp/newblitz`. |
| `trailerYouTubeId` | The trailer's YouTube video id (the `v=` part). Empty: the home page shows the trailer card with "Coming soon". Set: a click-to-play poster that loads the `youtube-nocookie.com` player only when pressed. |
| `siteUrl` | The site's public address, for canonical links, Open Graph tags and the sitemap. Default `https://newblitz.pages.dev`. |
| `discord`, `youtube`, `x` | Community links. |

After changing one: `python3 site/tools/build.py` (deploy.sh runs it anyway).

## The build: tools/build.py

Standard library only (Pillow, if installed, also makes web copies of screenshots). It rewrites the pages in place and is
safe to run any number of times. It fills:

- `<!--@head-->`, `<!--@header-->`, `<!--@footer-->`: the shared head (title and description from the page's
  `<!--page title="…" description="…" -->` line, Open Graph and Twitter tags, favicon, the CSS/JS with a cache-busting
  version), the header with the current page marked, and the footer with the trademark notice.
- every `<a data-link="…">`: `repo`, `repo:/path`, `discord`, `youtube`, `x` from site.json.
- `<!--@trailer-->` on the home page, from `trailerYouTubeId`.
- every `<figure class="shot" data-shot="…" data-label="…">` and the press page's `<!--@gallery-->`, from the screenshots
  (below).
- `sitemap.xml` and `robots.txt`.

To add a page: copy one, change its `<!--page … -->` line and `<main>`, add its name to `PAGES` in build.py (and to
`NAV` for the header), and build.

`tools/check.py` checks every internal link and `#anchor`, every asset, the HTML basics (one h1, unique ids, alt text,
balanced tags) and prints each page's weight. `tools/make_assets.py` remakes the web images and fonts from the repo's
brand kit (needs Pillow, fontTools and brotli); its outputs are committed.

## Screenshots

Every page has named screenshot slots. Until a slot has a real screenshot, it shows the brand's thumbnail frame with a
label saying what belongs there. To fill them:

1. Put the image files in `site/assets/shots/` (PNG, JPEG or WebP; any size, 16:9 looks best).
2. Write `site/assets/shots/shots.json`:

   ```json
   [
     { "slot": "multiplayer", "file": "four_players.png", "caption": "Four players at play select", "what": "Play select with four player colours, two of them phones" },
     { "slot": ["export-rom", "export-rom-kinds"], "file": "export.png", "caption": "Export ROM", "what": "The Export ROM screen" },
     { "file": "big_heads.png", "caption": "Big Heads", "what": "A tackle with Big Heads on" }
   ]
   ```

   `slot` (a name, or a list of names) says where it goes; without one, the file name minus its extension is tried as
   the slot, and the shot still appears in the press gallery. `caption` shows under it; `what` is the alt text. Every
   entry, in order, makes up the press page's gallery.
3. Done: the pages pick shots.json up by themselves (site.js fills the slots in the browser). Running
   `python3 site/tools/build.py` (deploy.sh does) bakes them into the HTML instead, adds width/height, and with
   Pillow makes 1280-wide WebP copies in `assets/shots/web/` for the pages (the originals stay the click-through).
   It also lists slots still on placeholders. `python3 site/tools/build.py --slots` lists every slot name.

| Slot | Page(s) | What it should show |
|---|---|---|
| `multiplayer` | home | Four players at play select (or a game), phones and pads together |
| `instant-replay` | home, features | Instant Replay: the scrubber over the last play, slow motion |
| `receiver-buttons` | home, features | Receiver Buttons: the icons over each receiver mid-play |
| `season-2026` | home, features, season | The 2026 Season: team select with today's teams and uniforms |
| `export-rom` | home, features | Export ROM: the screen picking the kind of cartridge |
| `builder-first-run` | start | Blitz Builder asking "Add the 2026 season?" |
| `builder-where` | start | Blitz Builder asking "Where do you want to play?" |
| `android-handheld` | start | The game running on an Android phone or handheld |
| `blitz-ds` | start, features | BLITZ DS: the play across two screens of a dual-screen handheld |
| `export-rom-kinds` | start | Export ROM's kinds list (Lite: Arcade, Lite: Season, Full, Custom) |
| `widescreen` | features | A widescreen, full-resolution moment mid-play |
| `phone-controller` | features | A phone as a controller, held sideways (the controller page) |
| `players-settings` | features | Settings → Players: who is who, the QR code, Identify / Swap |
| `tournament-bracket` | features | A tournament bracket (up to 8 named players) |
| `redo-play` | features | Redo Play: the toast or the return to play select |
| `realism-rules` | features | Settings → Rules with the Realism options |
| `house-rules` | features | Big Heads (or another Blitz code) on in a game |
| `pick-your-crew` | features | Pick Your Crew: choosing the starters |
| `touch-controls` | features | The on-screen touch controls on a phone |
| `addons` | features | Add-Ons: the installed paks, the 2026 Season row |
| `hints` | features | A first-run tip card, in the controller's own buttons |

Keep screenshots free of league and team logos and real player faces where possible (the site's own rule: no NFL, team
or player names or logos in its imagery), and of anything personal (file paths, account names).

## Deploy (not run yet)

```sh
site/deploy.sh            # build, stage the public files, deploy to Cloudflare Pages project "newblitz", branch main
site/deploy.sh --dry-run  # build and stage only
```

It deploys with the public account's own wrangler login (`XDG_CONFIG_HOME=build/wrangler_public`). The staging copy
leaves out `tools/`, this README, `deploy.sh` and `site.json`. If the site gets its own domain, set `siteUrl` first.

## Rules for the content

No NFL, team or player names or logos in the imagery or copy (beyond the factual "NFL Blitz 2000" and the trademark
notice), no ROM links, no download link for the season pak (it comes only through the app), no one's personal name, no
donation links. Facts come from the repo's README, CHANGELOG, docs/PLAYING.md and the platform guides; the privacy
page follows what the code connects to (`git grep -n http_get -- src`).
