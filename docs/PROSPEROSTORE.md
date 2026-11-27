# ProsperoStore: listing New Blitz on the PS5 homebrew store

Researched 2026-11-15, updated 2026-11-26 (see the **2026-11-26 update** at the end; pages read only, nothing submitted). Decide before submitting: the "NFL Blitz" name and
"bring your own ROM" questions in **Open questions**.

## What it is

- [prosperostore.net](https://prosperostore.net/): a community catalog (about 217 entries), maintained by darkxex,
  created by BlackBearReloaded. A native PS5 app reads it (title PPSA99000, alpha;
  [github.com/darkxex/ProsperoStore](https://github.com/darkxex/ProsperoStore)) and browses, installs, updates and
  uninstalls on the console.
- The catalog is a GitHub repo of signed entries:
  [github.com/darkxex/ps5-homebrew-catalog](https://github.com/darkxex/ps5-homebrew-catalog).
- The app needs ShadowMountPlus 1.7beta4 or newer and an ELF loader on port 9021; installs tested on firmware 6.02 and
  12.70 (others "experimental"). It doesn't mention etaHEN. Installed app folders need 777 permissions.

## How a title gets listed

- A GitHub pull request to the catalog, made by the account that owns the source repo (so `blitzrecomp`). The
  [submit page](https://prosperostore.net/submit/) helps: pick the repo, the release file and the icon, fill in the
  listing, and it opens the PR. A maintainer reviews it. Rules:
  [docs/submitting.md](https://github.com/darkxex/ps5-homebrew-catalog/blob/main/docs/submitting.md),
  [docs/review-policy.md](https://github.com/darkxex/ps5-homebrew-catalog/blob/main/docs/review-policy.md).
- **Package:** a `.zip` of the `<TITLEID>/` folder (`eboot.bin`, `sce_sys/param.json`, art), attached to a public
  GitHub Release (pre-releases allowed). They don't host files.
- **Versions:** a new tag and a new asset for each version; never a moving tag like `latest`, never a replaced asset
  (the sha256 must keep matching). Raise `contentVersion` in `param.json` each release (`01.000.070` style).
- **The record:** `apps/<TITLEID>.json` with `titleid`, `name`, `kind`, `category` (one of 11), `description`,
  `license` (SPDX), `author`, `version` (matches the tag), `source_repo`, `artifact_url`, `sha256`, `icon_url`.
- **Icon:** square PNG/JPEG/WebP, 512×512 recommended (256 minimum, 2 MiB maximum); `sce_sys/icon0.png` works. Pin
  `icon_url` to a tag or commit.
- **Title id:** four letters and five digits, first come first served, permanent. PPSA99961 looks valid
  (PPSA99999, CUSA and NPXS are refused). A title id can be reserved as "Coming soon" (5 per account, renewed every
  180 days).

## Content rules

Native PS5 titles only: no ELF payloads, PS4 packages, "emulator ROMs", pirated content, malware or repacks. The licence
must allow redistribution (GPL-3.0 is fine) and the submitter must own the source repo. Developers answer for what
their app bundles or downloads.

## Open questions (ask in the PR or an issue first)

- **Recompilations / bring-your-own-ROM:** not mentioned. New Blitz ships no game data, so it reads as in scope, but
  that's our reading, not their policy.
- **"NFL Blitz" in the name:** no trademark section; they only forbid names and icons that imitate an official product
  or another listing.

## What we'd need to prepare

1. A PS5 zip per release from `make release` (`PPSA99961/` with `eboot.bin`, `sce_sys/param.json` with
   `contentVersion`, `icon0.png` 512×512), as a release asset with its own name per version.
2. Note: today our PS5 build arrives without a ROM; the store-installed copy would need the "set up from your
   computer" path (Blitz Builder sends the ROM and setup) or a ROM picked on the console. Check that flow before listing.
3. The catalog record (GPL-3.0, a category, the description, sha256), the PR from `blitzrecomp`, and its "Validate
   submission" check passing.

## Other PS5 catalogs (one line each)

- [AnchorPS5-catalog](https://github.com/Kuroge/AnchorPS5-catalog): a PR-based `catalog.json` linking GitHub releases
  (Spanish-first).
- LightningMods' Homebrew Store (installed through [etaHEN](https://github.com/EchoStretch/etaHEN)): submission process
  not found.
- Overdrive: piracy-adjacent, avoid.

## 2026-11-26 update

Read-only again: the catalog repo (a full copy of `main`, 240 records, 1,285 commits), its docs, the submit page, ProsperoStore's
README, and the open and closed issues and PRs. Nothing was opened, commented or signed in.

### Short answer

**Yes, we can very likely list, as a `console-ports` entry.** The old open question (are recomps and bring-your-own-ROM
accepted?) is answered by the catalog itself, and the risks have moved: our zip must not contain `launch.elf`, a store-installed
copy has no first-run path on a PS5 for the ROM, and `contentVersion` is stuck at `01.000.000`. Details below.

### 1. What changed since 2026-11-15

- Rules are the same: native PS5 app with its own title id, `.zip` of `<TITLEID>/` on a public GitHub release, 12-field record,
  PR from the account that owns the repo ([submitting.md](https://github.com/darkxex/ps5-homebrew-catalog/blob/main/docs/submitting.md),
  [metadata.md](https://github.com/darkxex/ps5-homebrew-catalog/blob/main/docs/metadata.md)).
- New to us (the 11-15 notes missed these):
  - **A category made for us:** `console-ports` is "Games from consoles, rebuilt to run natively: decompilations and
    recompilations. They usually need your own game data." ([metadata.md, Categories](https://github.com/darkxex/ps5-homebrew-catalog/blob/main/docs/metadata.md#categories)).
  - **A release scan:** a second CI job, "Scan the release", downloads the zip and reports whether the app can leave the
    sandbox (payload loader address 127.0.0.1:9021/9020, `elevate_proc`, `etahen_jailbreak`, payload files with their SHA-256).
    It is information for the reviewer; "only an archive that is unsafe to unpack, or unreadable, fails the check"
    ([automation.md](https://github.com/darkxex/ps5-homebrew-catalog/blob/main/docs/automation.md#release-scan)). Payloads not on
    `helpers/approved.json` get a warning, and a rebuilt payload is flagged again each release.
  - **A daily-ish update bot:** the "Release updates" workflow runs every four hours and opens a PR to move a record to the
    owner's newest release; the check accepts these from any account. So after the first listing, weekly releases need no
    PR from us ([automation.md](https://github.com/darkxex/ps5-homebrew-catalog/blob/main/docs/automation.md),
    [versioning.md](https://github.com/darkxex/ps5-homebrew-catalog/blob/main/docs/versioning.md)).
  - `icon_member` (icon read from inside the zip), the optional `internalOnly: true`, and shared title ids
    (`apps/<TITLEID>-<owner>.json`) exist now. Descriptions are at most 200 characters, plain text; `name` at most 64 and unique
    ignoring case; `license` must agree with what GitHub detects for the repo.
  - Reviewers (and AI agents acting for them) work from a runbook that says "Not a payload, PS4 package, backport, ROM or web
    page" and "The main file is a payload (`.elf`...)": out of scope (`docs/maintainers/listing-runbook.md`).
- The submit page ([prosperostore.net/submit](https://prosperostore.net/submit/)) works in the browser, "nothing is sent to this
  site", and needs you signed in to GitHub as the repo's owner. Its text still says nothing about ROMs or names.

### 2. Recompilations and bring-your-own-ROM: accepted, with plenty of precedent

- The policy never mentions them, except the closest line: developers answer for "anything it bundles or downloads", and
  "pirated commercial content" is refused. "Emulator ROMs" (a ROM you ship) are out; a recomp that wants your own copy is not.
- 66 of the 240 records are `console-ports`; most need your own game data. Closest to ours (N64, static recompilation, GPL-3.0):
  - PPSA69424 "Wave Race 64 Recompiled": "needs your own Wave Race 64 (USA) (Rev A) ROM."
  - PPSA69423 "Rush 2 Recompiled": "needs your own Rush 2: Extreme Racing USA (USA) ROM."
  - PPSA64065 "Conker's Bad Fur Day" and PPSA92064 "Conker's Bad Fur Day Recompiled"; PPSA64064 "DK64 Rekongpiled".
  - PPSA99310 "Mario 64", PPSA99620 "Ship of Harkinian (Ocarina of Time)", PPSA99621 "2 Ship 2 Harkinian (Majora's Mask)":
    "Bring your own ROM", "requires your own game assets".
  - Others: Burnout 3, Blue Dragon, Lost Odyssey ("No game assets included."), Wind Waker, ICO, Halo (decomp).
- How they word the data: a final clause in the 200-character description ("Bring your own ROM", "No game data included"), and
  the setup steps in their own README. None puts a setup guide in the record.
- Rejections: no closed-unmerged PR in the last 60 was about ROMs or recomps. The one licensing fight (issue #503, PhazonPrime)
  was the owner withdrawing over third-party library licences, not the store refusing a ROM-based port. Issue #473 shows the
  stance on non-native things: a PC companion app with PS5 payloads was told it doesn't fit.

### 3. Names and icons

- No trademark rule. The review policy asks only that the name, description and icon "must not imitate another listing or an
  official product", and forbids squatting other projects' names or ids.
- Licensed names are everywhere and merged: "The Legend of Zelda: Tears of the Kingdom", "Mario 64", "Mario Kart World: Pit Stop",
  "Super Mario Sunshine", "Animal Crossing for PS5", "Halo: Combat Evolved", "Need for Speed: Underground", "Tekken Tag Tournament 2",
  "Def Jam: Fight for NY REMASTERED", "Spider-Man: Edge of Time". Wording like "Unofficial native PS5 port of ..." is used.
- No NFL, FIFA or other sports-league listing exists, so there is no precedent for a league mark specifically. Our record
  says "New Blitz 2027"; the public builds already avoid the NFL wordmark. Two safe choices: keep "New Blitz 2027" and say
  "a recompilation of NFL Blitz 2000" in the description (a plain factual description, like the Zelda and Mario entries), or ask first
  (draft (b)). Our icon must not be the NFL shield or the original box art.

### 4. Technical fit

- **Installer:** ProsperoStore needs ShadowMountPlus 1.7beta4 or newer and "a local ELF loader on TCP port 9021". It does not mention
  etaHEN. It unpacks the zip's folder into `/data/homebrew` (`eboot.bin` at `/data/homebrew/PPSA99961/eboot.bin`), sets `777`
  itself for store installs, and "does not start" apps: ShadowMountPlus adds the tile and the player starts it from the home
  screen. That is the same target and registration route as our Builder (FTP to `/data/homebrew/PPSA99961`, ShadowMountPlus
  registers it; docs/PS5_HOMEBREW.md), so the app folder fits as it is.
- **`launch.elf` must stay out of the zip.** It only exists so Blitz Builder can tell the console to start the title after the
  FTP send; the store never runs it, and a payload `.elf` in a release is the exact thing the policy calls out of scope and the
  scan flags. The title's own `eboot.bin` is the app. (The scan may still note network use or elevation imports; a grep of `src/main/platform_ps5.cpp` for 9021, 9020, etaHEN and
  elevate finds none, so expect "no sign" or a note, not a failure. We can't run their scan ourselves.)
- **etaHEN:** our target console runs etaHEN and ShadowMountPlus together, and the store's own requirement is ShadowMountPlus plus a
  port-9021 loader, which etaHEN provides. Our README can keep saying "jailbroken, etaHEN" and add "or ShadowMountPlus with a
  loader on 9021". We have only ever tested etaHEN + ShadowMountPlus (firmware 13.60), and the store says 6.02 and 12.70 are its
  tested firmwares: say so in the README.
- **Layout and size:** `PPSA99961/` = `eboot.bin`, `sce_module/libc.prx`, `sce_sys/` (`param.json`, `icon0.png` 512x512,
  `pic0.dds`, `pic1.dds`), `art/`, `assets/`, `packs/`, `paks/`, `recompcontrollerdb.txt`: about 68 MB unzipped. Fine (limit 2 GiB).
  `titleId` in `param.json` is PPSA99961, and `PPSA99961.json` is free in the catalog today (checked 2026-11-26).
- **ROM and first setup: would not work as-is for a player with no computer.** A store-installed copy arrives with no ROM, no
  `season.json`/`rules.json`/`builder_done`. On the PS5 the first-run screens are skipped ("always set up from a computer",
  src/main/firstrun.cpp), the game imports the ROM from `/data/blitzrecomp/` at startup, and with none it only logs
  "[rom] no ROM in ... yet" (src/main/main.cpp). There is no on-console way to give it a ROM except FTP to `/data/blitzrecomp`,
  and the store has no step for that. What we'd need (pick, in order of effort):
  1. Say it in the README and the description ("put your ROM in `/data/blitzrecomp/` over FTP, or send it from Blitz Builder"):
     no code, but a bare black screen on first start is a poor first minute.
  2. A "no ROM yet" screen on the PS5 that says where to put the file and which one (USA v1.1) and retries when it appears;
     tiny, and the right fix whatever else we do.
  3. Let the store-installed app run with defaults (no builder_done): check that Rules, Season and general settings fall back
     cleanly. Not tested on a PS5; the PS5 is stock now, so any of this is untestable by us until a console is back.
  Blitz Builder still works on a store-installed copy (it FTPs into the same folder; the "update" would then replace the store's
  eboot with the Builder's kit, which the store would later see as out of date. Harmless but worth a line in the docs).
- **Permissions:** the store sets `777` on its own installs; nothing for us to do. Our setup files already go to
  `/data/blitzrecomp` (outside the app folder, so store updates keep them; the store keeps the previous folder in
  `/data/prosperostore/previous/PPSA99961` until the next update).

### 5. Versioning for weekly releases

- **Tag:** new tag per release, never `latest` or a moving tag; the record's `version` equals the tag, with or without a
  leading `v`. Our computer-app tags (`v1.0.3`) are the natural choice, but then the PS5 zip must be attached to the same
  GitHub release as the apps, and a release's assets must never be replaced after listing (a re-upload changes the digest).
  Asset name: anything ending `.zip`; recommended `PPSA99961.zip`. A single name repeated across tags is fine (each tag has its own
  URL), but `artifact_url` and `sha256` must be unique across the catalog, so never reuse the same file for two tags.
- **contentVersion:** `NN.NNN.NNN`, raised every release, never lowered or reused. This is how a console learns of an update.
  **Ours is hard-coded `01.000.000` in `platform/ps5/param.json.in`** (and that is what is inside the pinned 1.0.2 kit), so
  consoles would never see an update. Map it from the version: 1.0.4 -> `01.000.004`, 1.0.10 -> `01.000.010`, 1.1.0 -> `01.001.000`
  (major, minor, patch padded 2.3.3), and have `make release` write it into `sce_sys/param.json` before building. The catalog reads it
  from `sce_sys/param.json` at the release tag in the repo, else from inside the zip, so it works with a zip-only
  `param.json`, but a committed copy is "the reliable way". `blitzrecomp/newblitz` has no `sce_sys/` folder today (checked
  2026-11-26), so either commit one, or rely on the zip copy and accept the reviewer note.
- **Tested vs untested:** a store listing means each weekly release is published to anyone on the store. From 1.0.4 the PS5 build
  is untested on a PS5 (STABLE.txt `tested: no`), and no console is available to us. Say "untested on hardware" in the release
  notes, or keep listing only the 1.0.2 build (805) until one is tested. Decide before the first PR; a listed record can't be
  quietly walked back (a withdrawal is an issue, as #503 and #790 show).
- **Pre-release flag:** allowed. The update bot proposes the newest release, pre-releases included, so a beta PS5 build would be
  picked up too. Keep betas off the release the zip is attached to, or on a separate repo.
- **What the release publishes today:** no PS5 artifact. The v1.0.3 release (`gh api repos/blitzrecomp/newblitz/releases`) has
  the Android APK, iPhone IPA, Linux, macOS, Windows downloads and SHA256SUMS; the PS5 kit is only inside the computer apps
  (`devices/ps5`, from `make device-kits`, with `launch.elf` and STABLE.txt beside it). `make release` never writes a PS5 zip.

### 6. Risks and what changes on our side

Risks:

1. Review is manual and can take time; first listing wants a maintainer's approval anyway. We are the repo owner, so no
   `third-party` label.
2. "NFL Blitz" in the description, and the league mark in any art. The name policy is lenient in practice but unwritten.
3. Our eboot is untested from 1.0.4 on, and each weekly store update goes to real users with no console for us to try it on.
4. A first start with no ROM shows nothing useful (above).
5. `contentVersion` never changes, so no store update prompts until fixed.
6. The release scan will list our ELF/eboot behaviour for a reviewer (network use for the roster server, phones over websocket
   for controllers); it is information, not a block, but the description should not hide the network features.

Changes needed:

- **Done 2026-11-26:** `make release` writes `PPSA99961.zip` (tools/release/make_ps5_store_zip.sh, called by
  tools/release/make_release.sh): the PS5 kit the release sends by default (`build/devices/ps5`, from the pinned
  `build/ps5_stable/current`) with `PPSA99961/` at the top, no `launch.elf` or `STABLE.txt` (refused if any `.elf`, link or
  `__MACOSX` gets in), `contentVersion` set from the version in the zip's `param.json`, `icon0.png` checked to be 512x512.
  It's in `SHA256SUMS`, the release scan reads it with the rest of `build/release/<v>/`, the release notes list it, and the
  script prints the `gh release create` line with it. Name: `PPSA99961.zip` for every release (the catalog's recommendation;
  the update bot's first rule is "the same name", and each tag has its own URL).
- **Done 2026-11-26:** `contentVersion` from VERSION: platform/ps5/ps5_stage.cmake turns `make ps5-modern VERSION=1.0.4` into
  `01.000.004` (1.1.0 -> `01.001.000`, 2.3.15 -> `02.003.015`; a version that doesn't fit NN.NNN.NNN stops the build). A dev
  build without VERSION keeps `01.000.000`. `param.json` has no other version field that follows the app (`masterVersion`
  `01.00` is the submission master, left as it is).
- **Done 2026-11-26 (untested on a PS5):** the "no ROM yet" screen. On the PS5, with no ROM stored, a screen over the launcher
  says to use Blitz Builder → PS5 from the computer app, or to copy the ROM to `/data/blitzrecomp` over FTP. It looks in that
  folder every 2 s (and on **Check Again**), so a ROM copied in is taken without a restart; then the launcher (Play) is back.
  Still to do: a README line for store installs.
- **Icon:** use `icon_member: "PPSA99961/sce_sys/icon0.png"` with `icon_url: null` (the catalog's rule: `icon_url` must be
  null when `icon_member` is set). The zip always has it (the NEW BLITZ 2027 tile, 512x512); nothing to commit.
- Decide the name and the STABLE/untested policy.

### (a) Draft catalog record: `apps/PPSA99961.json`

Fill the `<...>` after the release exists. `description` is 163 characters (limit 200). `author` is the public identity.

```json
{
  "titleid": "PPSA99961",
  "name": "New Blitz 2027",
  "kind": "game",
  "category": "console-ports",
  "description": "Native PS5 port of New Blitz 2027, a recompilation of NFL Blitz 2000 (N64) with modern rosters and online play. No game data included: needs your own USA v1.1 ROM.",
  "license": "GPL-3.0",
  "author": "Blitz Recomp",
  "version": "<tag, e.g. v1.0.4>",
  "source_repo": "https://github.com/blitzrecomp/newblitz",
  "artifact_url": "https://github.com/blitzrecomp/newblitz/releases/download/<tag>/PPSA99961.zip",
  "sha256": "<sha256 of that exact zip, lowercase hex>",
  "icon_url": null,
  "icon_member": "PPSA99961/sce_sys/icon0.png"
}
```

Check before filling: the description wording ("online play" only if true on the PS5; the season server and roster downloads
are real, but say what is), whether `name` should drop "2027", and that GitHub detects GPL-3.0 for the repo (it does:
`gh api repos/blitzrecomp/newblitz` says `GPL-3.0`). If the asset name must be unique per tag across a shared `artifact_url`
space, nothing to do: the URL includes the tag.

### (b) Draft question to the maintainers (an issue, or the PR description)

> Hello, and thanks for running the catalog.
>
> We would like to list New Blitz 2027 (title id PPSA99961), a GPL-3.0 static recompilation of the N64 game NFL Blitz 2000
> (N64Recomp), as a native PS5 app: https://github.com/blitzrecomp/newblitz. It ships no game data. The player supplies their own
> USA v1.1 cartridge dump, which the app checks and keeps in `/data/blitzrecomp`, the same way the Wave Race 64 Recompiled and
> Rush 2 Recompiled listings work. The zip would hold only the `PPSA99961/` folder (no `.elf` payloads), under the `console-ports`
> category.
>
> Two questions before we open the pull request:
>
> 1. Is a recompilation that needs the player's own ROM fine for the catalog, as the other `console-ports` entries suggest? Is there
>    anything you would like in the description or README about the ROM?
> 2. The original game's name includes a league's mark. We plan to call the listing "New Blitz 2027" and say in the description
>    that it recompiles NFL Blitz 2000, with our own icon and no league logos. Is that acceptable, or would you prefer we leave
>    the original name out of the description?
>
> We also publish a new version about once a week, each as its own tag with a new zip and a higher `contentVersion`. Is there
> anything else we should do so the update bot picks them up cleanly?
>
> Thank you,
> the Blitz Recomp team

### (c) Steps to submit, and what `make release` must produce

What to produce (`make release` does this since 2026-11-26; tools/release/make_ps5_store_zip.sh):

- Asset `PPSA99961.zip` (the catalog's recommended name), attached to the GitHub release for tag `v<version>`, listed in
  `SHA256SUMS`. Built from the PS5 kit's `PPSA99961/` folder, with that folder at the top of the archive:

  ```
  PPSA99961/eboot.bin
  PPSA99961/sce_module/libc.prx
  PPSA99961/sce_sys/param.json     (titleId PPSA99961, contentVersion 01.000.NNN raised each release)
  PPSA99961/sce_sys/icon0.png      (512x512)
  PPSA99961/sce_sys/pic0.dds, pic1.dds
  PPSA99961/art/  assets/  packs/  paks/  recompcontrollerdb.txt
  ```

  No `launch.elf`, no `STABLE.txt`, no `__MACOSX`, no symlinks (`ditto -c -k --norsrc --noextattr --noqtn --keepParent` as the
  computer-app zips are made, or `zip -r -X`). It must be a RELEASE build with no build paths, like the stable kit, and pass
  `tools/release_scan.sh` (add the zip to the files it unpacks).
- Decide which build goes in: 1.0.2 build 805 (tested) or the version's own (untested from 1.0.4).

Steps (all by hand, in order; nothing here has been done):

1. Settle the open questions (draft (b)) and fix the changes in section 6 (zip, `contentVersion`, no-ROM screen, icon in the repo).
2. Release 1.0.4 (or later) as usual, with `PPSA99961.zip` attached to the release. Do not re-upload or replace it later.
3. `shasum -a 256 PPSA99961.zip` and compare with the digest GitHub shows on the release page.
4. Optional, cheap: reserve the title id as "Coming soon" first (5 per account, refreshed every 180 days), though it is free today.
5. Post the question (b) as an issue on the catalog; wait for the answer.
6. Sign in to GitHub as the `blitzrecomp` account (the repo owner; make its organisation membership public if it is an org),
   open prosperostore.net/submit, paste `https://github.com/blitzrecomp/newblitz`, choose the zip asset, category `console-ports`,
   kind Game, the icon, and open the PR. Or fork the catalog and add `apps/PPSA99961.json` by hand ((a)). One file under `apps/`
   only.
7. Read the "Validate submission" check and the "Scan the release" summary; fix any failure (a new tag and zip if the asset
   needs to change, never a replaced asset).
8. After the merge: weekly releases are proposed by the update bot (a PR that changes `version`, `artifact_url`, `sha256`,
   `icon_url`). Our job is only: a new tag, a new zip, a higher `contentVersion` committed before tagging, and release notes (they
   are shown as the store's release notes).
9. Update README/site: "Also on ProsperoStore (PS5)", with the ROM instructions from section 4.

### Open questions only the maintainers can answer

1. Is a recompilation with a player-supplied ROM welcome (the records say yes, the policy is silent)?
2. Is "NFL Blitz 2000" acceptable in the description, and is "New Blitz 2027" an acceptable name? (No league-name listing exists.)
3. Will they accept a title whose releases are not tested on hardware by the developer, if we say so in the notes?
4. Does the update bot handle a release that carries many other assets (apps for six platforms) and picks the right `.zip`, and
   how should the asset be named so it is unambiguous ("Attach one `.zip` of the app folder" is their advice)?
5. Is a title with a network feature (roster downloads, season server, phone controllers over websocket) fine, and does the
   release scan need any notes from us?
