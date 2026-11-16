# ProsperoStore: listing New Blitz on the PS5 homebrew store

Researched 2026-11-15 (pages read only; nothing submitted). Decide before submitting: the "NFL Blitz" name and
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
