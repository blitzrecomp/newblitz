# New Blitz on your own iPhone: build it and sideload it with your own Apple ID

Written 2026-11-04; the public `.ipa` added 2026-11-14. Two ways onto an iPhone or iPad, both signed with **your** Apple ID, which can be a free one. Nobody else's certificate or name is involved.

- **The public app's `.ipa` (no Mac needed; §3, Route B).** The New Blitz download for Mac, Windows or Linux carries the iPhone app, unsigned. Blitz Builder → Somewhere else → A console or phone → Phone → **Save the iPhone app (.ipa)** saves it to Downloads → New Blitz for iPhone with your ROM and setup beside it, and you install it with AltStore, SideStore or Sideloadly.
- **Build it yourself (§2, Route A).** The source, your own NFL Blitz 2000 (USA) v1.1 dump, a Mac with Xcode.

The last section answers a different question: whether the app can be passed around privately to a group.

Tags: **[V]** checked in this repo or in a source cited with its date. **[I]** inference, not tested yet. **[HW]** must be tried on a phone.

> **Read this first: the free-Apple-ID route has one open technical question.** The game reserves 4 GiB of address space at start-up (`docs/IOS_FEASIBILITY.md`, device probe 2026-10-21). On the maintainer's iPhone that works because the app carries Apple's **Extended Virtual Addressing** entitlement. Apple's capability table lists that entitlement for paid Apple Developer Program and Enterprise members only, **not for free accounts** ([Supported capabilities (iOS)](https://developer.apple.com/help/account/reference/supported-capabilities-ios), read 2026-11-04) [V]. The **Increased Memory Limit** entitlement, the other one we use, does work with a free Personal Team when Xcode signs the app ([MacNativeGames issue #99](https://github.com/ly05010419/MacNativeGames/issues/99), 2026-10-21; [SideStore issue #1616](https://github.com/SideStore/SideStore/issues/1616), 2026-09-24) [V]. Whether the 4 GiB reservation fits without Extended Virtual Addressing has **not been tried** [HW]. The signs are good, though. In that same issue, another iOS port, signed by free Personal Teams through Xcode, mapped about 6 GiB when the profile lacked increased-memory-limit, and fit a 16 GiB layout once the profile granted it. Both are more than our 4 GiB [I]. Step 5 of Route A shows how to check with the repo's small probe app before you spend an hour on the full build. A friend with a **paid** account ($99 a year) avoids the question entirely.

---

## 1. What a free Apple ID gets you

From Apple's [About your developer account](https://developer.apple.com/help/account/basics/about-your-developer-account) (read 2026-11-04) [V]:

| Limit | What it means for New Blitz |
|---|---|
| **The provisioning profile expires 7 days after it's issued** | After 7 days the app won't open; it just bounces back to the Home Screen. Your saves stay as long as you don't delete the app. Re-sign and reinstall within the week (§2, step 9) and you carry on where you were. |
| **Up to 3 apps per device** | That counts every app signed by your free account on that phone. Classic and Modern are two separate apps, so both uses two slots. The probe would be a third, so delete it afterwards. AltStore or SideStore count against the 3 as well (§3). |
| **Up to 10 App IDs, each expiring after 7 days** | Each different bundle ID you sign uses one. Pick your bundle IDs once (step 4) and keep them, or you'll run out for the week. |
| **Up to 3 devices** | Your iPhone, plus maybe an iPad. |
| **No distribution** | A free-account build is for your own devices only. |

A **paid** Apple Developer Program membership ($99 a year, [enrollment](https://developer.apple.com/programs/enroll/)) makes profiles last a year, lifts the 3-app limit and grants Extended Virtual Addressing. The steps are the same.

---

## 2. Route A: build on your Mac with free Xcode (recommended)

### What you need
- An Apple Silicon or Intel Mac on a recent macOS, with **Xcode** from the Mac App Store (free). Open it once and let it install the iOS platform.
- Homebrew's `cmake`, `ninja` and `python3`. `make env` checks them.
- An iPhone on **iOS 17 or later** (the build's deployment target is 17.0, `Makefile` `IOS_CMAKE`) and its USB cable. The maintainer's phone runs iOS 27.
- Your own **NFL Blitz 2000 (USA) v1.1** dump. Nobody can send you one; see CONTRIBUTING.md's one rule.
- A **passcode** on the phone. iOS needs it for Developer Mode and pairing.

### Step 1. Build the game for the Mac side first (same as CONTRIBUTING.md)
```sh
make env && make venv
tools/build_deps.sh            # SDL2 and FreeType for the Mac (once; it also downloads the sources the iPhone build reuses)
# put your v1.1 dump in rom/
make rom-import                # writes build/baserom.z64 and pins its SHA-1; the recompiler reads the ROM here
```
**The ROM appears twice in this guide, for different jobs.**
- On the Mac, the build reads it to generate the game's code and assets.
- On the phone, the app needs its own copy (step 8). The app never has a ROM built into it.

### Step 2. The iPhone libraries and the app
```sh
tools/ios/build_deps.sh        # SDL2 (with the iOS 27 scene patch) and FreeType for arm64 iOS 17+, into build/ios/prefix (once)
make ios-classic               # build/ios/app_classic/BlitzRecompiled.app: Original 1999 / Classic Blitz
make ios-modern                # build/ios/app_modern/BlitzRecompiled.app: New Blitz (Modern); see below
```
**Modern needs its content first.** Run `make houston` and `make team-art` (docs/BLITZ2026.md). They build the 2026 art and uniforms into `build/appdata2026`, and `make ios-modern` copies that into the app (the `modern_content` step in the `Makefile`). With an empty `build/appdata2026`, Modern still builds, but without the new art [I].

The build itself is unsigned. `platform/ios/ios_bundle.cmake` even deletes an old signature and profile after every build. Signing happens next, with your account.

### Step 3. Your Apple ID in Xcode, and your team ID
1. Xcode → Settings → **Accounts** → **+** → Apple ID, and sign in. A free account shows as "*Your Name* (Personal Team)".
2. Select the team, then **Manage Certificates…** → **+** → **Apple Development**. That creates your signing certificate in the Mac's keychain.
3. Find your **team ID**, ten letters and digits, in the certificate's OU field:
   ```sh
   security find-certificate -c "Apple Development" -p | openssl x509 -noout -subject
   # ... OU=ABCDE12345 ...   <- that's your team ID
   ```
   `tools/ios/install.sh` reads it from `BLITZ_IOS_TEAM`.

### Step 4. Make the app yours: bundle ID and entitlements (free accounts)
**A bundle ID belongs to the first team that registers it.** The repo's IDs, `com.blitzrecomp.newblitz` and `com.blitzrecomp.newblitz.classic`, are taken by the maintainer's team, so Xcode will refuse them for you. Choose your own, for example `com.<yourhandle>.newblitz` and `com.<yourhandle>.newblitz.classic`. Put each in **both** places that name it, because they must match:
- `platform/ios/ios_bundle.cmake`: the two `set(BLITZ_IOS_BUNDLE_ID ...)` lines, which feed the app's Info.plist. Rebuild with `make ios-classic` / `make ios-modern` afterwards.
- `tools/ios/install.sh`: the `classic)` and `modern)` lines of the `case` that sets `BUNDLE_ID`, which go to the stub and the profile.

**Free accounts only: edit `platform/ios/BlitzRecomp.entitlements`.** Delete the `com.apple.developer.kernel.extended-virtual-addressing` key and its `<true/>`, and keep `increased-memory-limit`. With that key left in, Xcode's automatic signing fails for a Personal Team. That's expected from Apple's capability table [I: the exact error text is untested here]. With a paid account, leave the file alone.

> For the repo: a `BLITZ_IOS_BUNDLE_ID` / `BLITZ_IOS_ENTITLEMENTS` override in `install.sh` and `ios_bundle.cmake` would turn this step into two environment variables. That change isn't made yet.

### Step 5. (Free accounts, once) Check the memory layout with the probe
`tools/ios/probe` is a tiny app that tries exactly what the game needs: reserve 4 GiB, make 512 MiB of it writable, report the memory iOS grants, and check the Metal GPU. With a free account:
1. Delete the extended-virtual-addressing key from `tools/ios/probe/BlitzProbe.entitlements`.
2. In `tools/ios/probe/CMakeLists.txt`, change `com.blitzrecomp.probe` to your own ID.
3. Build and run it:
```sh
cmake -S tools/ios/probe -B build/ios/probe -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DBLITZ_IOS_TEAM=<your team ID>
xcodebuild -project build/ios/probe/BlitzProbe.xcodeproj -scheme BlitzProbe -sdk iphoneos -allowProvisioningUpdates
```
Install it from Xcode, or with `xcrun devicectl device install app --device <id> <the built .app>`, and open it. You need **`PASS reserve 4 GiB address space`**. If it fails, it prints "largest that worked: N GiB". Then New Blitz can't start on a free account as the runtime stands; stop here, and use a paid account instead. Delete the probe afterwards: it holds one of your 3 app slots.

### Step 6. Prepare the phone (once)
1. **Plug it into the Mac.** Unlock it and tap **Trust This Computer**.
2. **Let Xcode prepare it.** In Xcode → Window → **Devices and Simulators**, wait until the phone shows as ready; Xcode copies its developer disk image.
3. **Turn on Developer Mode** (iOS 16 and later). Settings → Privacy & Security → **Developer Mode** → on. The phone restarts; after the restart, confirm **Turn On** and enter your passcode. The switch only appears once a Mac with Xcode has seen the phone, or after an attempt to run a developer-signed app.

### Step 7. Sign and install
```sh
BLITZ_IOS_TEAM=<your team ID> tools/ios/install.sh modern     # or classic
```
What it does (read `tools/ios/install.sh`):
1. **Gets a profile.** It builds a stub app (`tools/ios/stub`) with your bundle ID and the entitlements file, using Xcode's automatic signing with `-allowProvisioningUpdates`. That registers the phone with your team, registers the App ID and creates the 7-day development profile.
2. **Signs the real app.** It copies the stub's profile into the real app and signs it with your certificate and the stub's entitlements.
3. **Installs and launches it.** `xcrun devicectl device install app`, then a launch. The phone must be unlocked for the launch.

The first launch on a free account fails with "Untrusted Developer". That's step 8.

### Step 8. Trust yourself, then give the app your ROM
1. **Trust your certificate.** Settings → General → **VPN & Device Management** → under Developer App, your Apple ID → **Trust**, and confirm. The phone needs internet for this; it checks with Apple.
2. **Open New Blitz.** The launcher asks for the ROM (docs/PLAYING.md, "Load ROM"). You can hand it over either way:
   - **Load ROM** opens the Files picker. Pick the `.z64`/`.v64`/`.n64`, or a `.zip` with one inside, from iCloud Drive or On My iPhone. The app checks that it's the USA v1.1 cartridge and keeps its own copy.
   - Or put the ROM in the app's own folder: Files → On My iPhone → New Blitz (the app shares its Documents, `UIFileSharingEnabled`). On the Mac you can drag it into the app in Finder → *your iPhone* → **Files**. Then open the app.

### Step 9. Every week: refresh before day 7
Run the same command again within the 7 days:
```sh
BLITZ_IOS_TEAM=<your team ID> tools/ios/install.sh modern --no-launch
```
- The stub build asks Apple for a fresh profile, and installing over the old app with the same bundle ID **keeps your saves and season**. A newer build of the game installs the same way.
- If you missed the week, the app just won't open. Run the command anyway; nothing is lost unless you deleted the app.
- **Never delete the app to "fix" it**: that deletes its data. Back up your saves first if you must.

### Troubleshooting

| Symptom | Likely cause and fix |
|---|---|
| `no paired iPhone connected` | Unlock the phone, re-plug it, tap Trust; check it shows in Xcode's Devices window. To pick one phone among several, set `BLITZ_IOS_DEVICE=<id>`, using an id from `xcrun devicectl list devices`. |
| The stub build fails about the bundle ID ("cannot be registered", "not available") | Someone else's team owns it. Choose another (step 4); it costs one of your 10 App IDs for the week. |
| The stub build fails about a capability | The extended-virtual-addressing key is still in `BlitzRecomp.entitlements` on a free account (step 4). |
| "Maximum number of apps for free development profiles" | You're over the 3-app limit. Delete one of your sideloaded apps (the probe, the other flavour) and try again. |
| The app opens and closes at once, after it worked for days | The 7 days are up. Run step 9. |
| The app starts, then quits during loading (free account) | Probably the memory reservation (the box at the top). Run the probe, step 5. |
| `none of the profile's certificates is in this keychain` | Create the Apple Development certificate on this Mac (step 3.2). |

---

## 3. Route B: the public app's `.ipa` (no Mac needed)

The public download (`make release`; docs/RELEASE.md) carries `NewBlitz.ipa`: `Payload/NewBlitz.app`, built with `BLITZ_CONTENT=public` (no 2026 content inside; the 2026 Season pak brings it) and signed by **no one**. It carries an ad-hoc signature only so that its entitlements (`platform/ios/BlitzRecomp.entitlements`: increased memory limit and extended virtual addressing) travel with it for the signing tool to ask for; an ad-hoc signature names nobody. `make device-kits BLITZ_CONTENT=public` makes it (the `ios_ipa` step in the `Makefile`) and refuses an app that was ever signed for a team.

### Step 1. Save it from Blitz Builder
On your Mac, Windows PC or Linux PC: launcher → **Blitz Builder** → Somewhere else → **A console or phone** → **Phone** → **Save the iPhone app (.ipa)**. It writes `Downloads/New Blitz for iPhone/`:
```
NewBlitz.ipa
README.txt                these steps, short
Copy to the iPhone/       blitz.z64 (your stored ROM), season.json and rules.json (this computer's setup),
                          season2026.nbpak (when the 2026 Season is installed), builder_done (skips the phone's Builder)
```

### Step 2. Install it with your Apple ID
Use one of the tools below, on a Mac or a Windows PC. Each asks for your Apple ID, makes your own development certificate and a profile for your phone, and re-signs every binary in the `.ipa`; they can rewrite the bundle ID to one your team can own (`com.blitzrecomp.newblitz` belongs to the maintainer's team). Then on the phone: **Developer Mode** on (§2, step 6.3) and **Trust** your Apple ID (step 8.1).

### Step 3. Give it the ROM and the setup
The app shares its Documents (`UIFileSharingEnabled`, `LSSupportsOpeningDocumentsInPlace` in `platform/ios/Info.plist.in`), and at start it imports a ROM from there and moves `season.json`, `rules.json`, `builder_done` and a season pak in (`main.cpp`, the drop folder; `paks_at_launch`). Open New Blitz once so its folder exists, then copy everything in `Copy to the iPhone` into it:
- **On the phone:** Files → On My iPhone → New Blitz (AirDrop or iCloud Drive gets the files onto the phone first).
- **From a Mac:** Finder → *your iPhone* → **Files** → drag them onto New Blitz.
- **From Windows:** the Apple Devices app (or iTunes) → *your iPhone* → Files / File Sharing → New Blitz.

Open New Blitz again: it starts with your ROM and the computer's setup. Or skip the copying and use **Load ROM** in the app.

### What isn't known yet [HW]
- **Free accounts and the memory entitlements.** The `.ipa` asks for extended virtual addressing, which Apple grants paid accounts only (the box at the top). Whether each tool drops it for a free account and signs anyway, or refuses, hasn't been tried; nor whether the game then starts (the probe question at the top). With a paid account the tools should keep both [I].
- **SideStore drops increased-memory-limit on free accounts** (SideStore #1616, below), so the game would get less memory there [I].
- The `.ipa` route as a whole has not been tried on a phone yet. The app inside is the same build `make ios-modern BLITZ_CONTENT=public` makes, which Route A signs and installs.

### Making one by hand
`make ios-modern BLITZ_CONTENT=public && make device-kits BLITZ_CONTENT=public` leaves it in `build/devices/ios/NewBlitz.ipa`. Don't run `tools/ios/install.sh` on that build first: its signature and profile would name the person who ran it (the step refuses such an app).

### The tools

None of them puts the maintainer's name on anything: each signs with the user's own account, so §1's limits apply (7 days, 3 apps, and the tool itself may take a slot).

| Tool (version, date) | What it needs | Notes |
|---|---|---|
| **AltStore Classic** ([v2.3, 2026-09-30](https://github.com/altstoreio/AltStore/releases/tag/v2.3)) | iOS 17.4 or later. AltServer on a Mac or Windows PC for the first install. 2.3 adds **Remote AltServer**: install and refresh "from anywhere with a WiFi connection". | Signs with your Apple ID. AltStore counts as one of your 3 apps. Reported to **keep the increased-memory-limit entitlement** with a free account (SideStore #1616, 2026-09-24). |
| **SideStore** ([0.7.0-alpha, 2026-09-15](https://github.com/SideStore/SideStore/releases)), installed with **iloader** ([v2.4.1, 2026-10-28](https://github.com/nab138/iloader/releases)) | A computer once, for iloader (it handles the pairing file and certificate), a passcode, and the free **LocalDevVPN** app from the App Store kept connected while installing or refreshing. | After that, it refreshes on the phone with no computer. **Known bug, open as of 2026-10-23: SideStore drops increased-memory-limit on free accounts** (#1616), so New Blitz would get about 3.3 GB instead of about 6. One guide reported that iOS 26.4 broke SideStore's refresh method ([gist, edited 2026-03-30](https://gist.github.com/sinceohsix/688637ac04695d1ff38f844acc8ba7f3)); check the current release before relying on it. iloader 2.4.1 itself turns on the increased-memory entitlement by default. |
| **Sideloadly** ([v0.71.0](https://sideloadly.io/)) | macOS 10.12 or later, or Windows 7 or later. On Windows it needs Apple's web (non-Store) iTunes and iCloud. | Signs with your Apple ID over USB, or Wi-Fi after the first pairing. It can change the bundle ID and has a background re-sign daemon. Its handling of entitlements is undocumented [I]. |

Every route still needs Developer Mode (§2, step 6.3) and the Trust step (step 8.1). The ROM step is the same, too.

---

## 4. Can the iPhone app go to a group (for example, Patreon supporters) without the team publishing it?

Short answer: **technically yes, in a few ways. But every way that's easy for the recipients puts the team's legal name in the signature, and charging money for it is the riskiest thing this project could do.** Details, then a recommendation.

### The options

| Route | How it works | Name in it? | Limits and risk |
|---|---|---|---|
| **Ad hoc** (what `tools/ios/adhoc.sh` + `publish.sh` already do for the team's own devices) | A paid account registers each recipient's iPhone UDID, then signs one IPA for those devices. They install it from a link. | **Yes.** The distribution certificate and profile carry the account holder's name. An individual account must be enrolled under a **legal name** ([enrollment](https://developer.apple.com/programs/enroll/)). | **100 iPhones per membership year.** Removing a device frees its slot only at renewal ([Devices overview](https://developer.apple.com/help/account/devices/devices-overview/)). Every new member means collecting a UDID and re-signing. Apple can revoke the certificate, which kills every copy at once. |
| **Ad hoc from an organization account** | As above, but enrolled as a company. | The **organization's** name, not a person's. It needs a real legal entity (no DBAs) and a D-U-N-S number (same page). | An LLC's registration is often public record, so it is weak cover [I]. The same 100-device cap applies. |
| **TestFlight** | Upload to App Store Connect; up to **10,000 external testers** by email or **public link** ([TestFlight](https://developer.apple.com/testflight/)). | Yes: the developer account's name. | **Builds for external testers go through App Review first.** NFL marks, current rosters and ESPN headshots run into the review guidelines' rules on others' intellectual property, so rejection is likely [I]. A public link also leaks easily. |
| **Enterprise program** | In-house signing with no device list. | An organization's name. | **Forbidden for this.** It requires 100+ employees and a legal entity, and it's for "internal-use apps to their employees" only ([Enterprise program](https://developer.apple.com/programs/enterprise/)). Apple revokes certificates used to distribute to the public. |
| **EU / Japan alternative marketplaces (AltStore PAL and others)** | A paid account, an addendum in App Store Connect, **Apple notarization**, then a marketplace hosts it. | Yes: the developer account. | EU (and Japan since 2025-12, [MacRumors](https://www.macrumors.com/2025/12/18/altstore-japan-launch/)) users only. Since 2026-10-01 there is a 5% Core Technology Commission on digital sales ([Apple, apps in the EU](https://developer.apple.com/support/apps-in-the-eu/); [iClarified, 2026-08-18](https://www.iclarified.com/101814/apple-revises-eu-app-store-terms-replaces-core-technology-fee-with-5-commission)). Apple can revoke a notarization afterwards, which blocks installs (the iTorrent case, [itc.ua](https://itc.ua/en/news/apple-blocks-apps-in-european-alternatives-to-the-app-store-how-does-it-work)). |
| **Unsigned IPA, each person signs it** (§3) | the team (or a friend) shares an unsigned IPA privately (the public release now carries one, §3); each recipient signs it with their own Apple ID using AltStore, SideStore or Sideloadly. An AltStore "source" (a JSON file listing the IPA) can even give them in-app updates [I]. | **No signature of the team's.** But whoever hosts the file is still the distributor (docs/OUTREACH.md), and the binary has to be scrubbed of build paths and names first (OUTREACH, "Staying pseudonymous"). | Free accounts get **no Extended Virtual Addressing**, so it may not start at all until the probe passes (top of this doc). Plus the 7-day refresh and the 3-app limit. A determined recipient can pass the IPA on: there's no control once it's out. |
| **Jailbreak repositories** | A package in a jailbreak repo. | No. | No public jailbreak is known for current iOS versions on recent iPhones (TrollStore stopped at iOS 17.0) [I: no authoritative 2026 status found]. So it reaches almost nobody. |
| **Give friends the source** | Friends with Macs build it themselves (§2). | No: each signs it with their own ID. | The most work for them. It needs the public cut of the repo, which doesn't exist yet (OUTREACH: `tools/make_public_tree.sh`), or access to the private repo. |

### Money changes the risk

- **docs/OUTREACH.md (2026-10-25) already decided "show first, no donations".** "Money is what most often turns 'fan preservation' into 'someone's profiting off our IP.'"
- **Charging for access is the pattern rights holders sue over.** In *Nintendo v. Tropic Haze* (Yuzu, 2024), Nintendo's complaint leaned on the Patreon: about **$30,000 a month** from over 7,000 patrons for early-access builds ([Wololo, 2024-02-28](https://wololo.net/2024/02/28/nintendo-are-suing-the-developers-of-the-yuzu-switch-emulator/); [TorrentFreak](https://torrentfreak.com/nintendos-yuzu-lawsuit-aims-to-pour-banana-peels-over-all-emulators-240228/)). The case settled within a week for **$2.4 million**, and the project shut down ([Engadget](https://www.engadget.com/makers-of-switch-emulator-yuzu-quickly-settle-with-nintendo-for-24-million-203204698.html)). That case was mainly DMCA anti-circumvention. Ours would be trademarks (the NFL), player publicity rights (the NFLPA), photo copyright (ESPN) and the game itself (Midway's successors; see the OUTREACH table). A paywall in front of those makes damages and willfulness easier to argue, and it makes this project worth a rights holder's time [I: not legal advice].
- **Patreon itself bars it.** Creators "may not post creations that infringe others' intellectual property rights", and repeat infringers' accounts are closed ([Patreon guidelines](https://patreon.com/policy/guidelines); [copyright and trademark policy](https://patreon.com/policy/copyright)).
- **Patreon ends the pseudonym.** A creator who earns money must give tax details (a W-9 at $600 a year, checked against the IRS) and pass **identity verification** with Persona; payouts lock until it's done ([W-9](https://support.patreon.com/hc/en-us/articles/360020687292); [identity verification](https://support.patreon.com/hc/en-us/articles/34582075437069-Identity-Verification-FAQ)). The pseudonym would then rest on a payment company's records, and those are handed over under subpoena (OUTREACH).
- **What's low risk and what isn't.** In OUTREACH's own split, the engine plus Original 1999, bring your own cartridge, is the low-risk part. New Blitz's 2026 logos, rosters and headshots are the high-risk part. Free, private and small doesn't make the high-risk part safe; it only makes a takedown less likely and less costly.

### Recommendation

1. **Don't put New Blitz behind Patreon or any payment.** If a Patreon ever exists, it should be for something else entirely. Even then, OUTREACH's "a personal support link elsewhere, later, at most" stands.
2. **For a handful of close friends:**
   - **Best: their own builds.** Each friend builds and signs with their own Apple ID from the source (§2), or a friend with a Mac does it for them on their phone. Nothing is distributed, and no name of the team's is in it.
   - **For friends the team is willing to put his name next to: ad hoc.** the team's ad hoc link (`adhoc.sh`/`publish.sh`) can take their UDIDs. It works today and has no 7-day limit, but the team's legal name is in the signature they install. Keep it to people who'd never post it.
3. **Before anything reaches a group beyond that:**
   - Run the free-account probe (§2, step 5).
   - Make the public cut: the engine, Original 1999 and the pack format, with no 2026 content.
   - Pass around only an **unsigned** IPA of that cut, for people to sign themselves (§3), and never charge for it.
   - New Blitz's 2026 content stays as OUTREACH says: the team's private build, plus the demo reel.
