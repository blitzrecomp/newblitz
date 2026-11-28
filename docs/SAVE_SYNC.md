# Syncing saves between devices (design, 2026-11-26)

Status: **phase 1 built** on branch `save-sync` (2026-11-27): Export and Import Save File. **Cloud Sync and Sync over Wi-Fi built** on branch `cloud-sync` (2026-11-27, for 1.0.5): Settings → General → Saves, behind a private switch until tested on real devices; the sync server is written and tested but not deployed. Tested by `make test-sync`, `make test-cloud`, `make test-sync-worker` and a Mac build; device checks are in docs/HUMAN_TESTS.md. Phase 1 is section 6, Cloud Sync and Wi-Fi section 7. (Phase 2, the sync folder, is not built: Cloud Sync covers it.) Written for the team. Read from the code on `surface-duo`: `controller_pak.cpp`, `adv_save.cpp`, `pak_guard.cpp`, `pak_backups.cpp`, `savestate.cpp`, `phone_pads.cpp`, `tools/rosters_hub_worker/src/index.js`, `platform/ios/Info.plist.in`, docs/ANDROID.md, docs/IOS_SIDELOAD.md.

## 1. What needs syncing, and what must not

Everything lives under the app folder (Mac `~/Library/Application Support/<app>`, Android `Android/data/<pkg>/files`, iOS the app's Documents, Windows the app folder).

| Thing | Where | Size | Sync? |
|---|---|---|---|
| Controller Pak images | `saves/*.mpk`, `saves/packs/<id>/`, `saves/original/`, `saves/advanced/<pack>/blitz2000_pak1.mpk` | 32 KB each | Yes. This is the player's records, PINs and the Classic/Modern seasons |
| Advanced season files | `saves/advanced/<pack>/season/season-<id>-<gen>.adv` | small (a 0x774-byte record plus franchises, seed, CRC) | Yes, but only with the pak that points at them |
| Settings | `general.json`, `controls.json`, `graphics.json`, `sound.json`, `rules.json`, `season.json`, `party.json`, `export.json` | KB | Partly. `rules.json` and `season.json` yes (they define the league). Controls, graphics and sound are per device (a touch screen and a Retroid differ), so no |
| Save states | `savestates/slot<N>.state`, `quit.state` | 8.4 MB each | **No.** Keyed to a hash of the build's site table, so they load only in the same build, and they hold host-thread positions. They differ per platform. Also never copy `quit.*` |
| `pak_history/` | beside each pak | 32 KB per copy, up to ~30 | Stays local. It is each device's own undo. Never upload it |
| Logs, ROM, art, announcer, mods, packs | | large | No. ROM and packs are the user's own files and the licensed-content rule applies |

Two traps:

1. **A stub and its season file are one save.** In Advanced the pak's note slot holds only a stub: `BZ26ADV`, season id, generation, display team/week and the sidecar's CRC. `load_sidecar` rejects a file whose id, gen or CRC doesn't match the stub, and the slot then shows as missing. Copy a pak without its `.adv` files and the season looks gone. Copy `.adv` files without the pak and they are orphans that pruning deletes. So the unit of sync is a **pack's whole `saves/advanced/<pack>/` tree** (pak plus every `season/*.adv` it or its `.bak`/`.bak2`/history copies refer to), taken while the game isn't writing. The app already writes sidecar first, pak second, and keeps the newest two generations, so a crash gives a matching pair; sync has to preserve that order (write `.adv` files first, the pak last).
2. **The pak guard and history assume one writer.** `pak_guard` keeps a stub on disk against any block write that would clear or change it, except the player's own Save. A sync that overwrites the pak file under a running game is exactly the 2026-11-26 wipe again: the game keeps the pak's file system in memory (the inode cache). So an incoming save must never be written while the game has the pak open. `pak_backups.cpp` already has the right mechanism: before the game opens the pak, replace it at once; during a game, leave a mark in the pak's folder and restart, and `pak_restore` swaps it at the next start's first pak access. Sync should reuse that path, so the replaced pak lands in `pak_history/` and "Controller Pak Backups" undoes a bad sync.

Paths are per pack and per engine (`saves/original/`, `saves/packs/<id>/`, `saves/advanced/<id>/`). A save made under one roster pack isn't valid under another, so a bundle carries the pack id and engine and the receiver files it in the matching folder, or refuses if that pack isn't installed.

## 2. Approaches

### A. Save bundle file (export / import)
One file, `*.nbsave` (a zip): a `manifest.json` (format version, app version, engine, pack id, per pak: file name, SHA-256, season id, gen, team, week, saved-at, device name) plus the pak files and `.adv` files. Export through the OS share sheet or Save-as; import by opening the file ("Open with New Blitz"), the Files picker, or dropping it in the app folder.

- Works on every platform including PS5 (drop into the folder over FTP, as the PS5 flow already does), needs no server, no account, no network and nothing to leak. iOS already has `UIFileSharingEnabled` and `LSSupportsOpeningDocumentsInPlace`, so Files and AirDrop work with the sideload. Android has the share sheet and the SAF picker.
- Manual: the player has to remember. The bundle can go stale; the manifest and conflict rules below are what keep that safe.
- Cheapest to build, and every later approach can reuse the bundle as its payload.

### B. Pairing code through our Cloudflare Worker (KV or R2)
Device A shows a code, device B types it; each pushes and pulls a bundle under a random key. No account: the key is the identity (same model as the rosters hub's long random `HUB_KEY` path, "privacy, not security").

- Real sync with one tap and works across any network, including PS5 and iOS.
- Costs: someone's save data on our infrastructure. KV caps values at 25 MiB (fine, a bundle is KBs) but its free tier is 1,000 writes a day, shared with the roster job and crash reports; R2 has a free tier but needs an account with billing attached. Abuse: the public Worker already serves unauthenticated `/paks` and takes `/reports/crash`; an upload route needs a per-key size cap, a rate limit and expiry (for example 90 days idle). A lost code means lost saves unless the app also keeps the bundle locally, which it does.
- Privacy: the Worker would hold saves keyed by a random string; no IP stored (the crash route already avoids `CF-Connecting-IP`). Encrypt the bundle on the device with a key derived from the pairing secret so the Worker holds opaque bytes. That also means the public build carries no secret: the app holds only the public base address and the per-user pairing secret it generated.
- Adds a service the team must keep running, plus a new "what we store about players" statement for the pseudonymous launch. This is the biggest decision.

### C. Direct device to device on the local network
`phone_pads.cpp` already runs a small HTTP and WebSocket server (port 8770) behind a switch, and Builder already sends files to devices (devicectl, FTP). A "Send my saves to another device" screen could show a short code and let the other copy fetch the bundle from the same network.

- No cloud, no cost, nothing stored anywhere. Fast.
- Both devices must be on at once and on one network; iOS needs the local-network permission; Windows firewall prompts; a PS5 can only be reached by FTP, not by being a client. Pairing needs a code or the hole `phone_pads` has ("anyone on the same network can join") would expose saves, so it must be an explicit, short-lived, code-gated mode. Worth it as a later convenience, and it is a bundle transport, not a different format.

### D. A user-chosen sync folder (iCloud Drive, Google Drive, Dropbox, Syncthing)
The player picks a folder; the app writes and reads bundles there, and the user's own sync service moves them.

- Desktop: simple. Pick a folder inside Dropbox or iCloud Drive and the app reads and writes plain files. Windows and Mac only need a path setting.
- Android: possible via the SAF "choose folder" grant, which works with Drive and Dropbox providers, but providers are slow and sometimes hand out stale copies. iOS: a sideloaded app without the iCloud entitlement can still read and write a folder the user picks in Files with a security-scoped bookmark (that includes iCloud Drive and Dropbox), but bookmark handling needs native code and the picker is awkward. PS5: not possible.
- No server and no cost to us, but the sync engines make their own conflict copies ("file (1)") and may sync a half-written pak. Mitigated by always writing whole bundle files, never the live pak, and by importing only through the app's normal import path.
- This is the same bundle file as A, with a folder watch added.

## 3. Conflict handling

Never merge inside a pak. Decide per save and let the player see what happens.

- **Unit and clock.** For Advanced, the unit is a season id and its generation. `gen` rises by one per save (`adv_save.cpp`, one past the newest on disk) and is the logical clock, so wall time and device clocks don't matter. Compare the incoming bundle's (season id, gen) with the local newest gen for that id:
  - incoming gen greater and the local gen is an ancestor (the bundle also carries that gen's CRC, or the local newest is in the bundle's retained generations): take it;
  - incoming gen equal with the same CRC: nothing to do;
  - incoming gen lower: refuse to overwrite, offer "keep this device's season" (default);
  - same gen, different CRC, or both sides advanced from a shared parent (neither contains the other): a real conflict, show both (team, week, saved-at, device) and make the player choose. Never pick silently.
- **Classic/Modern paks** have no generation. They are one 32 KB image of records and PINs plus season notes. Use a bundle-level "revision" counter and last-synced hash that we keep beside each pak (`saves/sync.json`: for each pak, hash at last sync, revision, device id). Same ancestor logic: if the local pak equals the last-synced hash, the incoming one wins; if both changed since, it is a conflict, and the player chooses a whole pak.
- **Replaced copy.** Whatever is overwritten goes into `pak_history/` first (the existing "copy before each write that changes the image"), and the losing season's `.adv` files are kept by the existing prune rule, which spares any generation a kept pak image still points at. Result: a sync can always be undone from Settings, General, Controller Pak Backups. The import also writes the `.adv` files before the pak, so a crash mid-import leaves either the old pair or the new pair.
- **Validate before writing:** CRC and magic of each file, a 32 KB size for the pak, every stub in the pak resolving to a file in the bundle (or already present), and the app/format version being no newer than ours. Anything off, refuse with a plain message and touch nothing.
- **While playing:** never import over a pak the game has open; use the restart mark path from section 1.

## 4. Recommendation

Build **A first, as the foundation, then add D on desktop and C or B only if people ask.** A bundle with versioning and conflict rules is the part everything else needs, costs nothing to run, carries no personal data, and works on every platform we ship, including the PS5 and the iOS sideload. It also fits the project rules: no secrets in the public build, no accounts, no donations, nothing stored on our side.

Phases:

1. **Save bundle, manual** (smallest useful). Settings, General, Save Sync: Export My Saves and Import Saves. Format as above, with the manifest, hashes, validation, the conflict screen, replaced copies into `pak_history/`, and import through the `pak_restore` mark. iOS and Android use the share sheet and open-with; desktop uses a file dialog; PS5 gets the folder drop. Only pak plus `.adv`, `rules.json` and `season.json`; no states, no per-device settings. Tests: stub and `.adv` mismatch, an older gen refused, a crash between the `.adv` and pak writes, a bundle from a missing pack.
2. **Sync folder on desktop (and Android via SAF).** The same bundle file, written to and read from a folder the player chooses; auto-export at quit and a "newer bundle found" prompt at launch. No live-file syncing.
3. **Nearby send (C),** reusing the Builder/`phone_pads` server pieces, code-gated and off by default.
4. **Cloud pairing (B),** only if 1 to 3 prove too fiddly for players. It would need a clear data statement, end-to-end encryption, size and rate caps, expiry, and the Worker kept apart from the private rosters hub.

## 5. Open questions for the team

1. Is "the player moves a file or picks a folder" acceptable for 1.1, or do you want one-tap cloud sync even if it means the Worker stores (encrypted) saves for the public project?
2. Should `rules.json` and `season.json` travel with saves, or only pak and season files? (A league defined on one device but the pak on another could disagree.)
3. Do the Classic/Modern paks matter, or is syncing the Advanced season the real goal? That decides whether the revision counter and `sync.json` are needed in phase 1.
4. How do we want a conflict shown, and should the older side always be kept in history for 14 days, as `pak_history` does now, or kept longer?
5. PS5: is FTP drop into the folder enough, or should the PS5 later fetch from a folder URL?
6. The bundle must not hold anything that identifies the player. Is the device name in the manifest (for the conflict screen) fine, or should it be a generated label?
7. Android's `Android/data` is hard to reach on recent versions: is a share sheet plus SAF picker enough, or should the app also offer a copy to `Documents/NewBlitz`?

## 6. Phase 1 as built (2026-11-27, branch `save-sync`)

Code: `src/game/save_sync_core.{h,cpp}` (the bundle, its checks, the decisions and the staged apply; no game needed, so
`tests/save_sync_test.cpp` runs it on its own: `make test-sync`, part of `make test`), `src/main/save_sync.cpp` (the
screens), `src/game/controller_pak.cpp` (`pak_sync_apply`, and the staged import at a pak's first access), a Settings row in
`config.cpp`, routing in `firstrun.cpp`, `main.cpp` (settings at start), and the `.nbsave` type in `platform/ios/Info.plist.in`.

**The file.** `New Blitz Saves <date> <time>.nbsave`, a zip: `manifest.json`, `README.txt`, `saves/<folder>/blitz2000_pak1.mpk`,
`saves/<folder>/season/season-<id>-<gen>.adv`, `settings/rules.json`, `settings/season.json`. The manifest has `kind`
(`new-blitz-saves`), `format` (1), the app version and build, the platform, a generated device label, the date, every file
with its SHA-256 and size, and per folder: engine (`original`/`standard`), mode (`advanced`/`pack`/`base`), pack id, what the
pak holds ("LAR, week 6"), each season file (id, gen, CRC, team, week, whether the pak points at it), the season states the
folder has had (`lineage`: id, gen, CRC from its season files, the stubs in its pak, `.bak`, `.bak2` and `pak_history/`), and
the pak states this device has synced (`seen`). It is built only from those two file patterns, so save states, `quit.*`,
`pak_history/`, `.bak` copies, per-device settings, ROM, art, packs and logs can't get in. A pak with no note on it is left
out (it would only wipe the other side's).

**Device label.** `<platform> <4 random letters>` ("macOS 7KQ3"), made once and kept in `saves/sync.json`. Never the
device's own name (open question 6: decided as a generated label).

**Import.** Everything is read and checked before anything is written: a zip, our `kind`, a format no newer than ours, every
entry listed with a matching hash and no other files, safe names, each pak 32 KB, each season file whole (magic, version,
size, CRC, named for its id and gen), every stub in an incoming Advanced pak resolving to a season file with the same CRC
(in the bundle or already here), and the pack installed (a bundle for a missing pack is refused whole, naming the pack). Then
each folder gets a verdict:

- **Advanced** (per season id in either pak, by the stubs): same gen and CRC is nothing to do; a higher gen whose parent
  (this device's gen and CRC) is in their lineage is newer; a lower gen in our lineage is older; same gen with another CRC, or
  neither side containing the other, is a conflict. A season only they have is new (or older, if we had it and dropped it);
  one only we have is newer if they had it, else a conflict.
- **Every pak** (Classic/Modern paks, and an Advanced pak whose seasons agree): `saves/sync.json` keeps the pak states each
  device exported or imported (`seen`, newest 64). Their pak in our `seen` or our history is older; our pak in their `seen`
  is newer; neither (or both) is a conflict. No pak here, or one with no note, is new. This replaced the revision counter
  of section 3: a counter can't be compared across two devices, hashes can.

New and newer are taken; older is kept as it is unless the player says "Use Theirs Anyway"; a conflict always asks ("Keep
Mine" is the default, "Use Theirs", "Cancel"). Then a confirm screen lists what changes, with a box for their Season and Rules
settings (on by default when they differ).

**Putting it in place.** `stage()` writes each taken folder's files into `<folder>/sync_incoming/` and a `ready` mark last.
`pak_sync_apply` (controller_pak.cpp) takes the pak lock; if the game has that folder's pak open, nothing more happens now and
the app restarts (a phone or console closes); otherwise, and at the next start's first pak access, `apply_staged` puts each
season file in place (one already there with other bytes is first kept as `pak_history/season-<id>-<gen>-replaced-<time>.adv`),
then the pak goes in through pak_backups' restore mark: the incoming image is written into `pak_history/` as a dated copy, the
`pak_restore` mark names it, and `apply_restore_mark` keeps the current pak in the history and swaps it. The staging is removed
only after the pak matches, so a crash anywhere is finished at the next start before the game reads the pak; staging cut off
before `ready` is dropped with nothing changed. Controller Pak Backups undoes an import, and its restore now also puts back a
season file an import replaced (`restore_replaced_seasons`), so the restored pak's seasons load. Settings are staged in
`<app>/sync_incoming/settings/` and written at the next start before the settings are read (the old ones kept as
`<name>.before-sync`).

**Where files go, per platform.**

| | Export | Import |
|---|---|---|
| Mac, Windows, Linux | Documents/New Blitz Saves, shown in the file browser | the file dialog, or the newest three in that folder |
| Android | the app folder's `exports/` | the system picker (ACTION_OPEN_DOCUMENT through the frontend's file dialog), or `exports/` |
| iOS | the app folder's `exports/` (visible in Files) | the Files picker, `exports/`, or a `.nbsave` opened into the app (Share → New Blitz; exported type `com.blitzrecomp.nbsave`) |
| PS5 | `/data/blitzrecomp/exports` (FTP) | the newest three in `exports/` (drop one there over FTP) |

Missing on phones: a share sheet for the export (the player finds it in Files or a file manager), and Android "Open with New
Blitz" for a `.nbsave` (no intent filter for it yet). Desktop export has no Save As dialog (the frontend's dialog only opens
files); it writes to Documents as Reset Data's backups do.

**Decisions for the team to confirm.** Settings travel and are on by default when they differ (question 2). Classic/Modern paks
are synced too, with `sync.json` (question 3). A conflict is a per-folder screen with Keep Mine first; the replaced side lives
in `pak_history/` under its normal 14-day rule (question 4). PS5 gets the folder only (question 5). Generated device label
(question 6). Android copies to `exports/` only, no `Documents/NewBlitz` copy (question 7). A missing pack refuses the whole
bundle rather than importing the rest.

## 7. Cloud Sync and Sync over Wi-Fi (2026-11-27, branch `cloud-sync`, for 1.0.5)

Settings → General → **Saves** (the phase-1 row, renamed) opens one menu: **Cloud Sync**, **Sync over Wi-Fi**, **Export
Save File**, **Import Save File**, Cancel. Cloud Sync shows only when the build has a sync server's address; Cloud Sync and
Wi-Fi only in builds with `BLITZ_SAVE_SYNC_NET` (CMakeLists.txt). **Public since 1.0.5** (the owner's call, 2026-11-27,
branch `cloudsync-public`): `BLITZ_SAVE_SYNC_NET` is on in every build, `BLITZ_CONTENT=public` and release builds too, and
the Makefile's `save_sync_server_file` puts build/save_sync_server's address into every Modern app's `sync/server`, as
`pak_server_file` does the season server's (make_release.sh refuses to run without it and checks each staged app has
it); tools/release_scan.sh allows that host beside the season server's. The address stays out of the source (build/),
like the season server's; the Worker's code and its wrangler.toml are in the public tree (no secrets in either). New
players are asked about it once in setup, and after updates (§7.9).

Code: `src/game/save_sync_cloud.{h,cpp}` (code, keys, sealing, server calls, decisions, one sync round; no game needed),
`src/main/save_sync_wifi.{h,cpp}` (Wi-Fi's code, server and fetch on plain sockets), `src/main/save_sync.cpp` (the
screens and the background timing), `third_party/monocypher` (Monocypher 4.0.2, CC0/BSD-2, vendored as is),
`tools/save_sync_worker` (the server), `controller_pak.cpp` (`pak_change_count`, `pak_folder_open`). Tests:
`tests/save_sync_cloud_test.cpp` (`make test-cloud`), `tools/save_sync_worker/test/worker_test.mjs`
(`make test-sync-worker`); both are in `make test`.

### 7.1 Cloud Sync, as the player sees it

- **Turn On Cloud Sync** makes a sync code, sends this device's saves up and says "Cloud Sync is on", with **Add a
  Device**.
- **Adding a device: a 6-digit number** (2026-11-27, branch `sync-pairing`, after the owner tried typing the 24-character
  code on an iPhone and a Surface Duo: "make it easier"). On a device with Cloud Sync on, **Add a Device** shows a number,
  grouped "482 915" and huge, for **2 minutes**, **once**, with a countdown; Cancel (or leaving the screen any way)
  removes it from the server at once; **New Number** makes another and the old one stops working at once; when the other
  device takes it the screen says "Your other device has it". On the new device, Cloud Sync → **Join** shows a number pad
  (big keys; a computer's digit keys work too; B or Backspace takes a digit back), and the sixth digit joins: the device
  gets the long code from the server, keeps it in secure storage exactly as a typed code (§7.2), and joins the slot the
  first device syncs with. The player never sees or types the long code. Wrong number: "That number didn't work", the pad
  cleared. Too many tries from one connection: "Wait a minute".
- **The full code stays as the fallback** (a PS5 with no one to pair at the same moment, a lost device): **Show Full Code**
  on a device that's on (grouped in fours, and as a QR code), and **Join with the Full Code** under the number pad (the
  on-screen keyboard; forgiving: lower case, dashes and spaces, O for 0 and I or L for 1 are fine), which checks there are
  saves behind it ("Nothing is saved under that code yet" if not).
- **Cancel anywhere leaves the device exactly as it was** (2026-11-27, the owner pressed Cancel partway through joining).
  Turn On, Join (number or code) and Use Slot N keep nothing until they go through: Cancel on their working screen, on a
  question they ask, or on its confirm screen, and Cloud Sync is as on or off as before, with the same code, the same
  slot and the same saves, and nothing sent (`cl::Snapshot`, §7.2). Sync Now cancelled sends and takes nothing. Past the first change (a save put in, or
  sent), the working screen says "Finishing…" with Close instead of Cancel, and the change completes. Turn Off is all or
  nothing the same way. A join's questions ("This device has saves of its own") come before anything is kept: Cancel
  there and the device isn't joined.
- **Sizes:** every Saves, Cloud Sync and Wi-Fi screen is drawn large (`firstrun_ui::Screen::large`): text and buttons about
  1.9 times the setup screens' on a phone (iPhone landscape body text ~16 pt), 1.3 times on a computer, the card the full
  width on a phone, buttons two to a row where there's room, and the card scrolls if it's still too tall.
  `BLITZ_PHONE_UI=1` shows a phone's sizes on a computer.
- From then on, by itself: the saves go up 20 seconds after each save the game makes, once it has finished writing the
  pak (one per Save, never per block: `pak_change_count` counts bursts of writes). They come down at launch, every 3
  minutes while the game isn't running (the launcher, before a season is opened) and every 10 while it is.
- **Cloud Sync is on** shows which slot it syncs with, when it last synced, whose saves came in last and the last
  problem, with **Sync Now** (or **Choose Which Saves to Keep** when a question waits), **Add a Device**, **Cloud Save
  Slots**, **Show Full Code**, **Turn Off** (Turn Off, or Turn Off and Delete the cloud copies in all three slots, or
  Cancel) and Back.
- **Cloud Save Slots** (added 2026-11-27 at the owner's request): Slot 1, Slot 2 and Slot 3 under one code, each its own
  chain of revisions on the server (its own id, key and token, derived from the code and the slot number), to keep
  separate seasons or a backup. The slots screen lists what each holds (its folders' "team, week", the device and when),
  and the wording says slots hold Controller Pak and season saves and that save states are never synced. A device syncs
  with one slot (Slot 1 unless switched). **Use Slot N** → "Sync with Slot N?" → Switch: an empty slot gets this
  device's saves; a slot with other saves asks first, folder by folder, before anything on the device or in the slot is
  replaced (newer or older both ask while switching); Cancel on those questions puts the old slot back.
- **Never replaced unasked, either way** (the owner's guard rule, 2026-11-27): a pull takes only what carries on from this
  device's saves (phase 1's lineage: season id and generation, pak hashes in `sync.json`); a push first reads the cloud's
  copy (the one kept from the last sync, or downloaded) against this device's saves, and sends only if the cloud's comes
  before ours in every folder. If the cloud's is newer (a backup put back here), unrelated (data reset, a new season), or
  only the cloud has a folder, nothing is sent: the question shows both sides with team, week, when each was saved and
  the device label, with **Keep This Device's** / **Use the Cloud's** / Cancel. Nobody there to ask (mid-game, after a
  save): nothing is sent, a toast says a choice is waiting, and Cloud Sync's screen offers it.
- **On the PS5** only Cloud Sync is offered (no Export/Import Save File, no Wi-Fi: the console can't share a file or be
  reached by another device's app), with a line saying so.
- Every screen says which devices it works on, and that none of it reaches a ROM made with Export ROM or a real N64's
  Controller Pak.
- Every import goes through phase 1: `read_bundle`'s checks and verdicts, the restore mark, the replaced pak and season
  files into `pak_history/` (Controller Pak Backups undoes a sync). New and newer saves come in without a question; a
  folder whose pak the game has open waits for the next start (nothing is pushed over the cloud's copy meanwhile, and the
  player is told once); older ones are this device's newer saves and go up; **both changed** asks, with Keep Mine first,
  the moment someone can be asked (at launch, or on the Cloud Sync screen: never over a running game). Keep Mine sends
  this device's saves up (the other device is then asked in turn); Use Theirs takes the cloud's.
- The league settings (rules.json, season.json) travel with newer saves, as with a save file (confirmed by the owner,
  2026-11-27): staged when saves are taken from the cloud and put in place at the next start (`save_sync_at_start`),
  never on their own and never with a forced restart (the toast says "their league settings from the next start"; the
  confirm screen after a question has the same box as a file import). A bundle made before that start carries the
  staged settings, not the ones they replace (`settings_file` in save_sync_core.cpp), so settings never bounce back.
  Save states, controls, graphics and sound never travel.

### 7.2 Design

- **The sync code** is the only secret: 24 characters of Crockford base32 (`0-9 A-Z` without I, L, O, U), 120 random
  bits from the system's cryptographic source. Kept in the platform's secure storage (below), never in the state file:
  `<app folder>/cloud_sync.json` holds only the slot, per slot the revision last synced and the fingerprint of the saves
  then, and the last result. `<app folder>/cloud_sync/slot<N>.nbsave` keeps the last bundle synced in each slot (the same
  saves as on the device) for the push guard. No account, no name, no device id: the bundle carries phase 1's generated
  label ("macOS 7KQ3") inside the encryption.
- **The code at rest** (`blitz_host.h secure_put/get/erase`, one implementation per platform; read once a run and kept in
  memory): macOS and iOS, a Keychain generic password (`kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly`;
  `src/main/secure_store_apple.cpp`; on the Mac a build signed differently from the one that stored it makes macOS ask
  once whether New Blitz may read it); Android, AES-GCM under a key made in the Android Keystore that never leaves it, the
  sealed value in the app's private preferences (`BlitzActivity.secretPut`); Windows, DPAPI for this user
  (`%LOCALAPPDATA%\NewBlitz\secrets`); Linux, the desktop's Secret Service through libsecret loaded with dlopen
  (`secure_store_linux.h`), else a file only this user can read (0600, `<app folder>/cloud_sync_code`); the PS5 has no
  keystore a title may use, so the same file there. A state file from an earlier build with the code in it has the code
  moved into secure storage and taken out.
- **Keys**, each a keyed BLAKE2b of a fixed label naming the slot ("new blitz cloud sync v1: slot 1: id") with the code
  as the key (independent pseudo-random functions): the **id** (64 hex) the server files that slot's saves under; the **key** for XChaCha20-Poly1305 (Monocypher's
  `crypto_aead_lock`); the **write token** that proves a writer knows the code. 120 bits leave nothing for a slow
  password hash to add. The id of a fixed code is checked against Python's `hashlib.blake2b` in the test, so the
  derivation can't change by accident (it would strand every paired device).
- **The sealed bundle**: `"NBC1"`, the revision (8 bytes), a random 24-byte nonce, the 16-byte tag, then phase 1's
  `.nbsave` zip encrypted. The header and the id are authenticated with it, so a blob can't be moved to another id or
  relabelled with another revision. A changed byte anywhere, a wrong code or a truncated blob: it doesn't open, and
  nothing on the device changes ("The saves in the cloud can't be read with this code").
- **One sync round** (`sync_once`): put in any import an earlier pull left staged (if its pak isn't open; while one is
  staged nothing else happens), then ask the server for the id's revision. Same revision as last time and the saves'
  fingerprint unchanged: nothing. Same revision, saves changed: push. Another revision: pull first, decide per folder as
  above, put in what's taken, then push if this device holds anything the cloud's copy doesn't (fingerprints compared,
  so a pull never echoes back up). Revision 0 (expired, or deleted): push again.
- **Pairing** (`offer_pairing`, `claim_pairing`): the first device draws P, six digits uniform over 000000-999999 from
  the system's random source, and a random 16-byte salt and 32-byte owner token. It seals "code + slot digit" with
  XChaCha20-Poly1305 under Argon2i(P, salt) (8 MiB, 3 passes; the header and pid authenticated with it) and posts owner,
  proof and the sealed code to `/v1/pair/<pid>`, where **pid** = keyed BLAKE2b("id: " + P) and **proof** = keyed BLAKE2b
  ("proof: " + P), both keyed with the fixed label "new blitz pairing v1" (known answers in the test). The joiner
  computes both from the typed digits, sends the proof as a Bearer token, and gets the sealed code only on a match
  (constant-time, on the SHA-256 the server keeps); the record is deleted as it's handed over, so it works once. It
  then opens it with Argon2i(P, salt) and runs `join(code, slot)`. The owner token lets only the first device cancel the
  number, replace it with a new one, or ask whether it was taken. A number someone else is showing at that moment (409,
  rare) is redrawn. The Argon2i only slows someone who somehow got a sealed blob; the protection is that the server never
  hands it to a wrong guess, allows a connection only a few guesses (§7.3), and keeps it two minutes.
- **Cancel** (`Snapshot`, `Hooks::commit`): Turn On, Join and a slot switch (`turn_on`, `join`, `switch_slot`, and
  `finish_pull` for their questions) first snapshot this device's Cloud Sync state (cloud_sync.json's bytes, the code in
  secure storage, the synced copies), then run as before. `Hooks::commit` is asked just before the first change to the
  device's saves or the cloud's copy (staging a pull, a push, a staged import put in); the screens answer it from the job's
  cancel state (Running → Committed by the job, or → Cancelled by the Cancel button: whichever comes first wins). Unless
  the round went through (pushed, pulled, up to date, waiting, or saves already put in), the snapshot goes back
  byte-for-byte: so a cancel, being offline, a failure, the game saving, or a question not answered yet all leave the
  device as it was. A join's or switch's questions come back with `Outcome::pending` (the code and slot to keep), and
  `finish_pull` keeps them only when the answers go through. Before, a join kept the code first, and a switch stored the
  new slot first (an offline switch then left an ordinary sync running on a slot the player hadn't finished choosing).
- **Compare-and-set**: a push names the revision it replaces (`?if_match=N`); the server takes it only if N is still
  current, and the new one is N+1. A device that loses the race gets 412, pulls, decides, and pushes again (once per
  round): two devices can't overwrite each other unseen. The blob's own sealed revision must be N+1 too.
- **Transport**: the app's existing `http_get` and `http_post` (NSURLSession, HttpURLConnection on Android, WinHTTP,
  curl on Linux, and the PS5's sceHttp2). POST's body is the 32-byte token, then the blob. The PS5's `http_post` was a
  stub; it now sends a real POST through the same sceHttp2 library as its GET (`sceHttp2CreateRequestWithURL(..., "POST",
  url, length)` and the body through `sceHttp2SendRequest`), so the PS5 pushes too. Untested on a console. Crash reports
  stay off on the PS5 (`crash_report.cpp`). No Worker change was needed for slots or the PS5.

### 7.3 The server (tools/save_sync_worker)

A new Worker with its own R2 bucket, apart from the rosters hub and the season server. Routes (`src/index.js`):
`GET /v1/<id>/rev` (`{"rev":N}`, 0 for none), `GET`/`HEAD /v1/<id>` (the blob, `ETag: "N"`), `GET /v1/<id>/previous`,
`POST /v1/<id>?if_match=N` (the app), `PUT /v1/<id>` with `If-Match: "N"` / `If-None-Match: *` and `Authorization:
Bearer <token>` (anything else), `POST /v1/<id>/delete` / `DELETE`. Rules, each tested: compare-and-set (412 with the
current revision); 1 MiB cap (413); the first write sets the token, kept only as its SHA-256, and later writes and
deletes need it (403); 30 writes an hour per id (429); at most 2,000 new ids a day in all (429; `MAX_NEW_PER_DAY`), so
the bucket can't be filled from nowhere; each write keeps the one before as `<id>.prev`; a daily cron removes ids with
no write for 90 days (an R2 lifecycle rule can do the same: DEPLOY.md). R2 is written with `onlyIf: {etagMatches}`, so a
write landing between the server's read and its write fails rather than clobbers. Not covered: two first writes to a
brand-new id in the same instant (only the device that turned Cloud Sync on writes an id first, so this needs two
devices turning on with one code at once).

Pairing's routes (2026-11-27; the saves routes above unchanged): `POST /v1/pair/<pid>[?replaces=<old pid>]` (owner,
proof, the sealed code; 201, 409 for a live number of another device's, 429), `GET /v1/pair/<pid>` with the proof as a
Bearer token (the sealed code once, then deleted; 404 for none, expired or a wrong proof, alike; a wrong proof on a live
pid also deletes it, since only a forger sends one), `GET /v1/pair/<pid>/status` with the owner token (`{"live":…}`,
false for anyone else), `POST /v1/pair/<pid>/cancel` with the owner token (always 200, so it says nothing to others).
Rules, each tested with a fake clock: a number expires 2 minutes after it's made (stored with it; an expired one is
deleted when found); **5 creates or lookups a minute per connection** (Cloudflare's Workers Rate Limiting binding
`PAIR_LIMITER`, whose period can only be 10 or 60 seconds: so about 10 guesses in a number's two minutes), keyed by a
salted SHA-256 of CF-Connecting-IP; at most `MAX_PAIRS` (500) numbers live at once in all (429); the sealed code at most
512 bytes; without the binding, 503 (never unlimited). Not covered: two right lookups in the same instant could both get
the code (both knew the number).

### 7.4 Threat model, and what is stored

- **The server (and anyone who gets into the account)** sees random-looking ids, ciphertext, sizes, write times and
  revision numbers. It can't read a save, get a code back from an id, or forge a save (it has no key). It can withhold
  or roll back a copy (serve an older one): the app then decides "older" and keeps its own saves; or delete one: the
  devices push again. It can't tell who a player is: no account, no name. The one address the Worker reads is
  CF-Connecting-IP, for pairing's guess limit only: hashed with a salt and handed to Cloudflare's rate limiter as the
  count's key, **never stored in the bucket, never logged** by the Worker, and not read by any other route (keep
  Workers Logs / Logpush off for it: DEPLOY.md).
- **Pairing's number** is short on purpose (six digits, a million values), so its safety is the server's: for its two
  minutes, a wrong guess gets nothing and a connection gets 5 guesses a minute. Someone guessing blind against one
  number has about 10 in a million chances; against everyone pairing at that moment (say 5 numbers live) about 50 in a
  million per connection per two minutes, and a botnet of many addresses multiplies that: what they'd win is a random
  stranger's code (read and change those saves; Controller Pak Backups puts them back). The server itself (or anyone
  reading the bucket or the request paths in those two minutes) can work out P from the pid by trying all million, so
  it could open the sealed code: the code is as safe from the server as the 2 minutes it's kept, which is why it's
  deleted when taken, cancelled or expired. (A PAKE would close that gap; Monocypher has none ready, and the long code
  remains for anyone who'd rather type it.) A number shown on screen can be read by someone looking over a shoulder in
  those two minutes, as the long code could.
- **Someone who learns a code** can read and change those saves (and turn the other devices' saves into theirs at the
  next sync; Controller Pak Backups puts them back). The code is shown only on that device's Show Full Code screen, with
  "Keep it to yourself". Turning off and on makes a new code; the old id's copy expires after 90 days, or at once with
  Turn Off and Delete.
- **Someone who knows an id but not the code** (server logs) can read only ciphertext, and can't write or delete (token).
- **A lost code** makes the cloud copy unreachable; every device keeps its own saves, and Turn On on any of them starts
  over with a new code.
- **On the device** the code is in the platform's secure storage (§7.2): another app, a backup of the app folder or a
  copy of it on another device doesn't carry it. Where there's none (Linux without a Secret Service, the PS5) it's a file
  only this user can read. Anyone who can run code as this user on the device can still read it.
- The public privacy page (site/privacy.html, "4. Cloud Sync" and "On your own network", 2026-11-27) says this in the
  site's words: only Controller Pak and season saves, encrypted on the device with a key from the sync code before
  upload, the server can't read them, no account/name/email, the connection's address used only to count pairing tries
  and never stored, deleted after 90 days unused or at Turn Off and Delete; Sync over Wi-Fi goes device to device on the
  player's network, nothing through our server.

### 7.5 Costs

Cloudflare's prices: R2 10 GB-month, 1M Class A and 10M Class B operations a month free, then $0.015/GB-month,
$4.50/M A, $0.36/M B, no egress fees; Workers 100K requests a day free, or $5 a month with 10M requests, then $0.30/M.
A blob is about 50 KB (a pak or two and a few season files, zipped), so ~100 KB a player with the previous copy.

Per player who plays every day on two devices (a heavy case): 3 pushes a day (each: the app's revision check and the
POST; the Worker reads the current copy, writes the previous and the new one: 2 Class A, 2 Class B, 2 requests),
10 revision checks (launch and the timed looks: 1 Class B, 1 request each) and 3 pulls (1 B, 1 request each). A month:
180 Class A, 570 Class B, 600 requests.

| Daily players | Storage | Class A / month | Class B / month | Requests | Cost / month |
|---|---|---|---|---|---|
| 1,000 | 0.1 GB | 0.18M | 0.57M | 20K a day | **$0** (all within the free tiers) |
| 10,000 | 1 GB | 1.8M ($3.60 past the free 1M) | 5.7M (free) | 200K a day: Workers Paid ($5, 6M of its 10M) | **~$8.60** |
| 100,000 | 10 GB (at the free line) | 18M ($76.50) | 57M ($16.92) | 60M a month: $5 + 50M × $0.30 = $20 | **~$113** |

Most players have one device and don't play daily, so real numbers should be a fraction of these. Levers if needed:
looks every 10 minutes in the launcher instead of 3 (halves Class B and requests), no previous copy (a third of
Class A), pushes at most every few minutes. The 30-writes-an-hour and 2,000-new-ids-a-day caps bound what a misbehaving
or hostile client can cost.

### 7.6 Sync over Wi-Fi

For two devices in one room, without the cloud: "open both devices and share before you switch your play session".
**Send My Saves** gathers this device's bundle, seals it and serves it on the local network for ten minutes, showing a
12-character code; **Get Saves** on the other device takes the code, fetches, and imports through phase 1's checks and
questions exactly like a file (Older and Conflict both ask).

- The code packs the sender's address (its last two numbers; the receiver fills in the first two from its own, as on
  any home network), which of 16 ports (8780-8795), and 40 random bits. From the whole code, Argon2i (Monocypher, 4 MiB,
  3 passes) makes the sealing key and the path the bundle is served under. The secret is short, so the slow hash makes
  each offline guess against a captured bundle cost tens of milliseconds; the server answers a wrong path with nothing,
  and stops after 20 wrong tries or ten minutes.
- Plain BSD sockets (`net_compat.h`, as `phone_pads.cpp` and the Roster Room use), one request at a time: the same code on
  every platform, with no ATS or cleartext rules in the way. iOS asks for local network access the first time (the
  Info.plist wording now mentions saves).
- **Finding the other device.** The code names the sender's last two address numbers, so devices on different
  subnets of one home network (192.168.0.x and 192.168.1.x) already find each other. When nothing answers at that
  address (a 10.x network whose second number differs, or a guest network), the getter asks the whole network by UDP
  broadcast on the code's port who has these saves; only the sender with that id answers (a wrong id counts as a wrong
  try). Failing that, **Type Its Address**: the sender shows its address under the code. iOS doesn't let a sideloaded
  app broadcast (that needs Apple's multicast entitlement), so on an iPhone it's the code's address or the typed one.
- Not on the PS5 (only Cloud Sync is offered there).

### 7.7 Testing

Automated (no account, nothing leaves the computer):

- `make test-cloud`: codes (made, typed sloppily, rejected), the derivation's known answer, sealing (round trip, a
  changed byte anywhere, a wrong code, a moved id, a cut blob), `next_step` and `split_pull`, Wi-Fi on loopback (round
  trip, wrong code, 20 tries, the address filled in), then **two app folders syncing through the Worker's own code**
  (`local_server.mjs`, R2 in memory): turn on, join with a sloppily typed code, league settings staged for the next start
  and not bounced back, no echo, push and pull both ways, an open
  pak waiting, a conflict waiting then asked both ways (Keep Mine on one, Use Theirs on the other, the replaced pak in
  `pak_history/`), a lost race (412), a changed blob on the server, the server wiped, busy, off, offline; then the push
  guard (a backup put back: nothing sent, asked, Use the Cloud's; an unrelated season: asked, Keep This Device's sends
  it; unanswered: asked again, nothing sent), Cloud Save Slots (a slot each its own chain, an empty slot filled,
  switching asks for newer and older both, state per slot kept), the code's file (0600, moved out of an old state file),
  and Wi-Fi discovery and a typed address; then pairing (numbers, the pid and proof's known answers, sealing; a join by a
  number typed "482 915", once only, a wrong number, Cancel, New Number replacing, a Slot 2 number, offline), Cancel at
  every step (Turn On, Join, a join's question, its answers, a switch, Sync Now: each cancelled, offline or unanswered
  leaves the state file, the stored code, the synced copies and the saves byte-for-byte as they were) and the guess
  limit as the player sees it. 437 checks (the local server allows 40 pairing tries a minute, `PAIR_TRIES`).
- `make test-sync-worker`: every server rule above against R2 in memory, pairing's with a fake clock (made, a wrong number
  404, the right one once then gone, expiry at 2 minutes, cancel, replace, the global cap, 5 a minute per connection so 10
  in two minutes, no address ever stored or given to the limiter, 503 without the limiter, the saves routes unchanged).
  83 checks.
- `tools/save_sync_two_apps.sh`: **two copies of the Mac app** with their own app folders (build/sync_test/A and B)
  against the local Worker, for trying the screens by hand on one Mac (Cloud Sync and Wi-Fi between the two).

On real devices: docs/HUMAN_TESTS.md ("Cloud Sync and Sync over Wi-Fi").

### 7.8 Decisions to confirm

1. (Confirmed 2026-11-27, with phase 1's choices: Classic/Modern paks sync too, league settings travel with saves and
   apply at the next start, a missing pack refuses the whole bundle, generated device labels only.)
2. Conflicts found by the background sync wait for a moment someone can be asked (launch, or the Cloud Sync screen),
   never over a running game; a toast says so.
3. Older saves aren't asked about in an ordinary Cloud Sync: this device's newer ones carry the cloud's on and go up
   (the other device then sees "newer"). Switching slots asks about them.
4. The code stays readable on the device (Show Full Code), from secure storage.
6. Slots: three, one active per device, nothing synced between slots; Turn Off and Delete clears all three.
7. The Mac's Keychain may ask once after an update signed differently; the code is read once a run.
5. The server: its own R2 bucket; 90-day expiry; 30 writes an hour per code; 2,000 new codes a day in all.

### 7.9 "Playing on multiple devices?" (2026-11-27, branch `cloudsync-public`, for 1.0.5)

The owner's question, in setup and after updates, so players find Cloud Sync without opening Settings:

- **The screen** (firstrun.cpp step E, setup's own look): "Playing on multiple devices?", "Turn on Cloud Sync?", and "You
  can turn it on later in Settings → Saves. It syncs your Controller Pak and season saves, not save states." Buttons:
  **Yes** opens Cloud Sync's own screen (`save_sync_open_cloud`: Turn On Cloud Sync, Join with the 6-digit number,
  Cancel; Cancel and Back close it rather than showing the Saves menu) and setup goes on when it closes (CloudWait, as
  Customize waits for Settings); **No** (not now: asked again after a later update); **Don't Ask Again** (a third button:
  setup's screens have buttons, no check boxes beside a choice). Exit counts as No.
- **When:** in setup, after Updates and reports, on the same paths (this device plays here: This Computer, or a phone's
  own setup; also a phone set up from the computer, after its Updates and reports). Not on the computer's Phone or
  Handheld, PS5 or N64 paths. After an update (a new `BLITZ_VERSION`; dev builds are all 0.9.0, so rebuilds never count),
  at the first start: `firstrun_unfinished` is true while it's due, so that start opens on the launcher (not straight
  into the game, config.cpp skip_launcher) and setup comes up by itself with E alone (eyebrow "CLOUD SYNC"); the answer, then
  the game starts as it would have. Never over a game in progress. On the PS5 (no setup of its own) the same, once it has
  its ROM; so its first start after a send asks too.
- **How often:** in setup, then after up to 3 updates (setup's ask isn't counted: "during onboarding and 3x after every
  update", the team 2026-11-27; a new player: setup and three updates; a player from 1.0.4: three updates), at most once per
  version, only while Cloud Sync is off and the build has it, never after Don't Ask Again.
  Yes counts as an ask (they saw the screen); turning Cloud Sync on stops it.
- **Kept in** General's hidden `cloud_ask_count` (0-3, update asks), `cloud_ask_never` and `cloud_ask_version` (the version last asked
  at, or setup finished at without asking) in general.json: one record per app, whichever game mode (Original 1999 or New
  Blitz) is chosen. The rule is `src/game/cloud_ask.h` (`due`, `answered`, `passed`), unit-tested by
  `make test-cloud-ask` (in `make test`).
- Decided (team, 2026-11-27): setup's ask doesn't count toward the 3; a phone set up from the computer asks at its first
  start and after updates (it may be the main device); a PS5 is never asked (Cloud Sync is still in its Settings → Saves).
