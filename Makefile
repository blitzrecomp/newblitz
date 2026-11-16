PY ?= python3
VENV := .venv

.PHONY: psp-recomp regress ios-classic ios-modern win win-classic win-modern ps5-classic ps5-modern bundle bundle-classic bundle-teamplus recomp-modern houston team-art app-modern env venv test test-adv rom-scan rom-import rom-verify syms recomp n64recomp rsp patches app dist notarize rom2026 syms2026 recomp2026 app2026 dist2026 logo27

env:            ## Check the toolchain
	@./tools/check_env.sh

venv:           ## Python tools: ROM analysis (splat, spimdisasm), Pillow, dmgbuild
	$(PY) -m venv $(VENV)
	$(VENV)/bin/pip install -q --upgrade pip
	$(VENV)/bin/pip install -q -r requirements.txt

test: test-adv
	$(VENV)/bin/python -m unittest discover -s tests -v

test-adv:       ## Unit tests for Advanced mode's league model (standings, tiebreakers, playoffs)
	@mkdir -p build/tests
	clang++ -std=c++20 -O1 -g -Wall -isystem lib/N64ModernRuntime/thirdparty tests/adv_league_test.cpp src/game/adv_league.cpp -o build/tests/adv_league_test
	@if [ -f packs/blitz2026/pack.json ]; then build/tests/adv_league_test packs/blitz2026/pack.json; else echo "adv_league: skipped (no packs/blitz2026)"; fi

test-ps5-send:  ## Send to PS5 without a console (src/main/ps5_send.h): connect timeout, progress, manifest, whole sends to a local FTP server (pyftpdlib in .venv)
	@mkdir -p build/tests
	clang++ -std=c++20 -O1 -g -Wall tests/ps5_send_test.cpp src/common/sha256.cpp -o build/tests/ps5_send_test
	@V=$(VENV)/bin/python; [ -x $$V ] || V=$(PY); \
	if $$V -c 'import pyftpdlib' 2>/dev/null; then \
		R=$$(mktemp -d)/console; mkdir -p $$R; P=$$((20000 + RANDOM % 20000)); \
		$$V -m pyftpdlib -i 127.0.0.1 -p $$P -w -d $$R > $$R/../ftp.log 2>&1 & S=$$!; sleep 1; \
		build/tests/ps5_send_test $$P $$R; rc=$$?; kill $$S; exit $$rc; \
	else echo "ps5_send: no pyftpdlib (.venv/bin/pip install pyftpdlib): the FTP sends skipped"; build/tests/ps5_send_test; fi

test-adb-parse: ## Send to Android's reading of adb's answers (src/main/adb_parse.h), from captured output
	@mkdir -p build/tests
	clang++ -std=c++20 -O1 -g -Wall tests/adb_parse_test.cpp -o build/tests/adb_parse_test
	build/tests/adb_parse_test

rom-scan:       ## Identify every ROM in rom/
	@$(PY) tools/romtool.py scan

rom-import:     ## Normalize the ROM to build/baserom.z64 and pin its hash (TARGET=<id> if several)
	@$(PY) tools/romtool.py import $(if $(TARGET),--target $(TARGET))

rom-verify:     ## Check build/baserom.z64 against the pinned hash
	@$(PY) tools/romtool.py verify

build/n64recomp/N64Recomp:
	cmake -S lib/N64Recomp -B build/n64recomp -G Ninja -DCMAKE_BUILD_TYPE=Release
	cmake --build build/n64recomp --target N64Recomp RSPRecomp

n64recomp: build/n64recomp/N64Recomp   ## Build N64Recomp + RSPRecomp from the submodule

syms: rom-verify   ## Generate build/syms/blitz2000.syms.toml and the hazard report
	$(VENV)/bin/python tools/gen_syms.py
	$(VENV)/bin/python tools/hazards.py

recomp: syms build/n64recomp/N64Recomp   ## Recompile the game to C in build/RecompiledFuncs
	rm -rf build/RecompiledFuncs
	build/n64recomp/N64Recomp blitz2000.toml

rsp: rom-verify build/n64recomp/N64Recomp   ## Recompile the audio microcode (n_aspMain) to C++
	mkdir -p build/rsp
	build/n64recomp/RSPRecomp n_aspMain.toml

patches:   ## Apply our patches to submodules (platform/*-patches)
	tools/apply_patches.sh

# Save states (F5/F7): the apps are built from tools/savestate/resumable.py's copy of the recompiled
# code, in which every function that can sleep keeps a shadow stack of call sites (no measurable CPU
# cost). SAVESTATES=0 builds from the plain recompiled code instead.
SAVESTATES ?= 1
ifeq ($(SAVESTATES),1)
SS_FLAG := -DBLITZ_SAVESTATES=ON
else
SS_FLAG := -DBLITZ_SAVESTATES=OFF
endif

# Release builds (make release; tools/release/make_release.sh): VERSION=x.y.z sets the apps' version (CMakeLists.txt's
# BLITZ_VERSION otherwise; -U drops a release's number from a build folder's cache), RELEASE=1 builds with no
# build-machine paths inside (BLITZ_RELEASE). Every app's CMake gets them.
REL_FLAGS = $(if $(VERSION),-DBLITZ_VERSION=$(VERSION),-UBLITZ_VERSION) -DBLITZ_RELEASE=$(if $(filter 1,$(RELEASE)),ON,OFF)

app: recomp rsp patches   ## Configure and build build/app/BlitzRecompiled.app
	@if [ "$(SAVESTATES)" = 1 ]; then $(PY) tools/savestate/resumable.py --in build/RecompiledFuncs --out build/savestate/RecompiledFuncs --rom build/baserom.z64 --sinks sleep,recv > /dev/null; fi
	cmake -S . -B build/app -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
		-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DBLITZ_FLAVOUR=classic $(SS_FLAG) $(REL_FLAGS)
	cmake --build build/app --target BlitzRecompiled


# Signing for build/dist: ad hoc by default (runs here; Gatekeeper rejects a downloaded copy). For a public
# release, SIGN_ID="Developer ID Application: Name (TEAMID)" (security find-identity -v -p codesigning), then
# `make notarize NOTARY_PROFILE=<profile>` (the profile is stored once with `xcrun notarytool store-credentials`).
SIGN_ID ?= -
SIGN_FLAGS := --options=runtime --sign "$(SIGN_ID)" $(if $(filter -,$(SIGN_ID)),,--timestamp)

dist: app   ## Copy the app to build/dist, signed (SIGN_ID, default ad hoc) without the debug entitlement, and zip it
	mkdir -p build/dist && rm -rf "build/dist/Classic Blitz.app" build/dist/ClassicBlitz-macos-arm64.zip
	ditto build/app/BlitzRecompiled.app "build/dist/Classic Blitz.app"
	codesign $(SIGN_FLAGS) --entitlements platform/macos/entitlements_dist.plist --deep --force "build/dist/Classic Blitz.app"
	codesign --verify --deep --strict "build/dist/Classic Blitz.app"
	cd build/dist && ditto -c -k --keepParent "Classic Blitz.app" ClassicBlitz-macos-arm64.zip
	@echo "build/dist/Classic Blitz.app (and .zip): no ROM inside; the app asks for it on first launch"

notarize: dist   ## Notarize and staple build/dist/Classic Blitz.app (needs SIGN_ID and NOTARY_PROFILE), then re-zip
	@test "$(SIGN_ID)" != "-" || { echo "SIGN_ID must be a Developer ID Application identity"; exit 1; }
	@test -n "$(NOTARY_PROFILE)" || { echo "NOTARY_PROFILE missing: xcrun notarytool store-credentials <profile> first"; exit 1; }
	xcrun notarytool submit build/dist/ClassicBlitz-macos-arm64.zip --keychain-profile "$(NOTARY_PROFILE)" --wait
	xcrun stapler staple "build/dist/Classic Blitz.app"
	rm -f build/dist/ClassicBlitz-macos-arm64.zip && cd build/dist && ditto -c -k --keepParent "Classic Blitz.app" ClassicBlitz-macos-arm64.zip
	spctl -a -vv -t exec "build/dist/Classic Blitz.app"

# ---- 2026 flavour (32 teams: Zoinkity's Team Extension on v1.1; docs/BLITZ2026.md) ----------
# TEAMPLUS_BPS=<path to Blitz2k-11_team+.bps> (user-supplied; never committed or bundled).

rom2026: rom-verify   ## Build build/teamplus.z64 from the v1.1 ROM and TEAMPLUS_BPS
	@test -n "$(TEAMPLUS_BPS)" -o -f build/teamplus.z64 || { echo "set TEAMPLUS_BPS=<path to Blitz2k-11_team+.bps>"; exit 1; }
	@if [ -n "$(TEAMPLUS_BPS)" ]; then $(VENV)/bin/python tools/teamplus.py rom --bps "$(TEAMPLUS_BPS)"; \
	else $(VENV)/bin/python tools/teamplus.py verify; fi

syms2026: rom2026   ## Symbols and hazard report for the 2026 flavour
	rm -rf build/tp/dis   # spimdisasm cache is per ROM image
	$(VENV)/bin/python tools/gen_syms.py --rom build/teamplus.z64 --out build/tp/syms/blitz2000_teamplus.syms.toml \
		--dis build/tp/dis --names syms/names_teamplus.toml
	$(VENV)/bin/python tools/teamplus.py toml
	$(VENV)/bin/python tools/hazards.py --rom build/teamplus.z64 --syms build/tp/syms/blitz2000_teamplus.syms.toml \
		--config build/tp/blitz2000_teamplus.toml

recomp2026: syms2026 build/n64recomp/N64Recomp   ## Recompile the 2026 flavour to C in build/tp/RecompiledFuncs
	rm -rf build/tp/RecompiledFuncs
	build/n64recomp/N64Recomp build/tp/blitz2000_teamplus.toml
	@! grep -rqs '^    0 = ' build/tp/RecompiledFuncs || { echo "zero-destination writes left in the 2026 output"; exit 1; }

app2026: recomp2026 rsp patches   ## Configure and build build/app2026/BlitzRecompiled.app (bundle name "Modern Blitz")
	@if [ "$(SAVESTATES)" = 1 ]; then $(PY) tools/savestate/resumable.py --in build/tp/RecompiledFuncs --out build/savestate/tp/RecompiledFuncs --rom build/teamplus.z64 --sinks sleep,recv > /dev/null; fi
	cmake -S . -B build/app2026 -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
		-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DBLITZ_FLAVOUR=teamplus $(SS_FLAG)
	cmake --build build/app2026 --target BlitzRecompiled

dist2026: app2026   ## build/dist/Modern Blitz.app (+ .zip): personal use only (contains team+'s recompiled code)
	mkdir -p build/dist && rm -rf "build/dist/Modern Blitz.app" build/dist/ModernBlitz-macos-arm64.zip
	ditto build/app2026/BlitzRecompiled.app "build/dist/Modern Blitz.app"
	codesign $(SIGN_FLAGS) --entitlements platform/macos/entitlements_dist.plist --deep --force "build/dist/Modern Blitz.app"
	codesign --verify --deep --strict "build/dist/Modern Blitz.app"
	cd build/dist && ditto -c -k --keepParent "Modern Blitz.app" ModernBlitz-macos-arm64.zip
	@echo "build/dist/Modern Blitz.app: on first launch, Load ROM takes build/teamplus.z64 or another ROM with team+'s code"

# ---- Modern on the original game (32 teams on v1.1's own code, nothing of the Team Extension's; docs/BLITZ2026.md
# "The original-game engine"). Houston is ours: `make houston` (art and uniforms from build/, personal use).

recomp-modern: syms build/n64recomp/N64Recomp   ## build/engine32/RecompiledFuncs: v1.1 with the team tables moved to 32-team copies and Modern's hooks
	rm -rf build/engine32/RecompiledFuncs
	$(PY) tools/engine32_toml.py
	build/n64recomp/N64Recomp build/engine32/blitz2000.toml

houston:   ## Houston from our own art and uniforms (build/team_art_out, <app folder>/uniforms/HOU) -> build/engine32/houston and <app folder>/houston
	$(VENV)/bin/python tools/make_houston.py --app-folder $(or $(APP_FOLDER),build/appdata2026)

team-art: houston   ## Our team art (rounds 5 and 6) into <app folder>/art: the Commanders, every team, the league's marks, the credits page
	$(VENV)/bin/python tools/import_team_art.py --app-folder $(or $(APP_FOLDER),build/appdata2026)
	$(VENV)/bin/python tools/import_team_art.py --round6 --art build/team_art_out_r6 --app-folder $(or $(APP_FOLDER),build/appdata2026)

# ---- Content: private builds and the public one (docs/design/SEASON_PAK.md) ----------------------------------------
# BLITZ_CONTENT=public builds the Modern apps (and so the device kits made from them) with no licensed content inside:
# no 2026 roster pack, roster snapshot, team art, uniforms, VS helmets, announcer calls, Houston or Pro Bowl art, and no
# live-rosters address. The 2026 Season pak brings them (make pak-2026; the app downloads it from the season server).
# The public Mac app builds in build/app_modern_public, so a private build next to it is left alone. The default,
# private, bundles everything as before. The season server's address goes in from build/pak_server (one line, never
# committed; public and private builds alike), as build/rosters_hub does for the live rosters.
BLITZ_CONTENT ?= private
PUBLIC := $(filter public,$(BLITZ_CONTENT))
APP_MODERN_DIR ?= $(if $(PUBLIC),build/app_modern_public,build/app_modern)
# The season's content, one list for a private build's Resources and the pak (tools/make_season_pak.py), so the two
# can't drift: the roster pack (CMake bundles it), the roster snapshot (build/blitz-rosters), Houston (make houston) and
# these parts of <app folder>.
SEASON_APP_FOLDER = $(or $(APP_FOLDER),build/appdata2026)
SEASON_PARTS = art/files art/vs_right uniforms announcer probowl

app-modern: recomp-modern rsp patches   ## Configure and build build/app_modern/BlitzRecompiled.app (Modern on the original game), with Modern's art from <app folder> (APP_FOLDER=<dir>, default build/appdata2026); BLITZ_CONTENT=public: build/app_modern_public, without it
	@if [ "$(SAVESTATES)" = 1 ]; then $(PY) tools/savestate/resumable.py --in build/engine32/RecompiledFuncs --out build/engine32/savestate/RecompiledFuncs --rom build/baserom.z64 --sinks sleep,recv > /dev/null; fi
	cmake -S . -B $(APP_MODERN_DIR) -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
		-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DBLITZ_FLAVOUR=modern -DBLITZ_CONTENT=$(BLITZ_CONTENT) $(SS_FLAG) $(REL_FLAGS)
	@rm -rf $(APP_MODERN_DIR)/BlitzRecompiled.app/Contents/Resources/devices  # added back below: CMake's bundle fixup mustn't see them
	cmake --build $(APP_MODERN_DIR) --target BlitzRecompiled
	@R=$(APP_MODERN_DIR)/BlitzRecompiled.app/Contents/Resources; touch $$R/engine-original; \
	rm -rf $$R/houston $$R/art/files $$R/art/vs_right $$R/art/credits.wms $$R/uniforms $$R/announcer $$R/probowl $$R/rosters $$R/paks; \
	ditto art/blitz2026/files $$R/art/files; mkdir -p $$R/art && cp art/credits/credits.wms $$R/art/credits.wms; \
	$(call pak_server_file,$$R); \
	if [ -n "$(PUBLIC)" ]; then rm -rf $$R/packs/blitz2026 $$R/packs/pac12; echo "public: no licensed content in the app (the 2026 Season pak brings it)"; \
	else if [ -d build/engine32/houston ]; then ditto build/engine32/houston $$R/houston; \
	else echo "no build/engine32/houston: Houston needs \`make houston\` (or <app folder>/houston)"; fi; \
	C=$(SEASON_APP_FOLDER); \
	for d in $(SEASON_PARTS); do if [ -d $$C/$$d ]; then ditto $$C/$$d $$R/$$d; \
	else echo "no $$C/$$d: the app goes without it (make team-art, tools/import_uniform_sets.py, tools/announcer_studio)"; fi; done; \
	ditto art/blitz2026/files $$R/art/files; \
	for f in $$(ls art/blitz2026/files); do if [ -e $$C/art/files/$$f ]; then \
	echo "warning: $$C/art/files/$$f covers New Blitz's own $$f at run time (an older art round): move it out"; fi; done; \
	$(call roster_snapshot,$$R); fi
	@R=$(APP_MODERN_DIR)/BlitzRecompiled.app/Contents/Resources; rm -rf $$R/devices; \
	if [ -n "$(PUBLIC)" ] && [ "$$(cat build/devices/content.txt 2>/dev/null)" != public ]; then echo "device kits: left out (not built with BLITZ_CONTENT=public)"; \
	elif [ -d build/devices ]; then ditto build/devices $$R/devices; \
		if [ -n "$(PUBLIC)" ]; then rm -f $$R/devices/ios/NewBlitz.app.zip; elif [ "$(DEVICE_KITS_IOS)" != 1 ]; then rm -rf $$R/devices/ios; fi; echo "device kits: $$(ls $$R/devices | tr '\n' ' ')"; fi
	codesign --options=runtime --no-strict --sign - --entitlements platform/macos/entitlements.plist --deep --force $(APP_MODERN_DIR)/BlitzRecompiled.app

pak-2026:   ## build/paks/season2026.nbpak (+ its versioned name and catalog.json for the season server) from the content a private app-modern bundles (APP_FOLDER=<dir>)
	@test -f build/blitz-rosters/manifest.json || { echo "no build/blitz-rosters (tools/rosters_publish.sh clones it)"; exit 1; }
	@test -f build/engine32/houston/houston.json || { echo "no build/engine32/houston (make houston)"; exit 1; }
	$(PY) tools/make_season_pak.py build --pack packs/blitz2026 --rosters build/blitz-rosters --houston build/engine32/houston \
		--app-folder $(SEASON_APP_FOLDER) --parts "$(SEASON_PARTS)" --out build/paks

.PHONY: pak-2026

# The device kits Blitz Builder sends from the Mac app (src/main/device_send.cpp): the PS5 title and its launch payload
# and the Android APK. Build those first (make ps5-modern, make android-modern), then `make device-kits app-modern`.
# Kits that aren't built are skipped. The iPhone app only with DEVICE_KITS_IOS=1 (here and for app-modern): it's signed
# for your team (tools/ios/install.sh modern; zipped, so the Mac app's deep signing leaves it alone), and Apple's
# signing data in it names the developer, so a Mac app that's shared leaves it out; Blitz Builder then says how to
# build the iPhone app with your own Xcode.
# The public iPhone app: NewBlitz.ipa (Payload/NewBlitz.app) with no one's signature, for each player to sign with their
# own Apple ID (AltStore, SideStore, Sideloadly; docs/IOS_SIDELOAD.md). Signed ad hoc only so its entitlements (the
# memory ones in platform/ios/BlitzRecomp.entitlements) travel with it for those tools to ask for: an ad-hoc signature
# names nobody. Refused if the build was ever signed for a team (a provisioning profile names the developer).
# $(call ios_ipa,<built .app>,<out dir>)
ios_ipa = A=$(1); O=$(2); \
	if [ ! -f $$A/BlitzRecompiled ]; then echo "ios: not built (make ios-modern BLITZ_CONTENT=public)"; \
	else rm -rf $$O && mkdir -p $$O/stage/Payload && ditto $$A $$O/stage/Payload/NewBlitz.app && \
		rm -rf $$O/stage/Payload/NewBlitz.app/_CodeSignature $$O/stage/Payload/NewBlitz.app/embedded.mobileprovision && \
		codesign --force --deep --sign - --entitlements platform/ios/BlitzRecomp.entitlements $$O/stage/Payload/NewBlitz.app && \
		if codesign -dv $$O/stage/Payload/NewBlitz.app 2>&1 | grep -q 'TeamIdentifier=[A-Z0-9]'; then echo "ios: signed for a team"; exit 1; fi && \
		(cd $$O/stage && zip -qry -X ../NewBlitz.ipa Payload) && rm -rf $$O/stage && echo "ios: NewBlitz.ipa (unsigned, for the player's own Apple ID)"; fi

device-kits:   ## build/devices: the PS5 and Android builds (+ the iPhone's with DEVICE_KITS_IOS=1) for Blitz Builder to send from the Mac app
	@rm -rf build/devices; mkdir -p build/devices
	@if [ -f build/ps5/app_modern/PPSA99961/eboot.bin ] && [ -f build/ps5/host/launch-PPSA99961.elf ]; then \
		mkdir -p build/devices/ps5 && ditto build/ps5/app_modern/PPSA99961 build/devices/ps5/PPSA99961 && \
		cp build/ps5/host/launch-PPSA99961.elf build/devices/ps5/launch.elf && echo "ps5: kit"; else echo "ps5: not built (make ps5-modern; tools/ps5/deploy.sh once makes the launch payload)"; fi
	@if [ -n "$(PUBLIC)" ]; then \
		if [ "$(DEVICE_KITS_IOS)" = 1 ]; then echo "ios: a public build never carries a team-signed iPhone app (no DEVICE_KITS_IOS=1)"; exit 1; fi; \
		$(call ios_ipa,build/ios/app_modern/BlitzRecompiled.app,build/devices/ios); \
	elif [ "$(DEVICE_KITS_IOS)" != 1 ]; then echo "ios: left out (DEVICE_KITS_IOS=1 includes it)"; \
	elif codesign -dv build/ios/app_modern/BlitzRecompiled.app 2>&1 | grep -q TeamIdentifier=[A-Z0-9]; then \
		mkdir -p build/devices/ios && rm -rf build/devices/ios/tmp && mkdir -p build/devices/ios/tmp && \
		ditto build/ios/app_modern/BlitzRecompiled.app build/devices/ios/tmp/NewBlitz.app && \
		ditto -c -k --keepParent build/devices/ios/tmp/NewBlitz.app build/devices/ios/NewBlitz.app.zip && rm -rf build/devices/ios/tmp && echo "ios: kit"; \
		else echo "ios: not signed for a device (tools/ios/install.sh modern signs it)"; fi
	@a=$$(ls -t build/android/app_modern/NewBlitz.apk build/android/app_modern/*.apk 2>/dev/null | head -1); if [ -n "$$a" ]; then \
		mkdir -p build/devices/android && cp "$$a" build/devices/android/NewBlitz.apk && echo "android: kit"; else echo "android: not built (make android-modern)"; fi
	@find build/devices -type f -perm +111 -exec chmod a-x {} +  # no executables in the Mac app's Resources (CMake's bundle fixup reads them as Mach-O)
	@# What the kits were made from: a public Mac app takes only kits built with BLITZ_CONTENT=public (checked here: no
	@# roster pack or Houston in their resources).
	@if [ -n "$(PUBLIC)" ]; then for r in build/ps5/app_modern/PPSA99961 build/android/app_modern/apk/assets/res build/ios/app_modern/BlitzRecompiled.app; do \
		if [ -e $$r/packs/blitz2026 ] || [ -e $$r/houston ]; then echo "$$r has the 2026 content: build it with BLITZ_CONTENT=public"; exit 1; fi; done; fi
	@echo "$(BLITZ_CONTENT)" > build/devices/content.txt

# Windows x64, cross-compiled on this Mac (docs/WINDOWS_VULKAN.md; platform/windows/toolchain-xwin.cmake): needs
# `brew install xwin` and Microsoft's CRT/SDK in build/xwin (xwin --accept-license ... splat --output build/xwin).
WIN_CMAKE = cmake -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_TOOLCHAIN_FILE=platform/windows/toolchain-xwin.cmake $(REL_FLAGS)

win-classic: recomp rsp patches   ## build/win_classic: Classic Blitz for Windows x64 (a folder with the .exe, DLLs and assets)
	@[ -f build/xwin/redist/vcruntime140.dll ] || bash tools/windows/fetch_vc_runtime.sh
	$(call SS_CODE,build/savestate/RecompiledFuncs,build/RecompiledFuncs)
	$(WIN_CMAKE) -S . -B build/win_classic -DBLITZ_FLAVOUR=classic $(SS_FLAG)
	cmake --build build/win_classic --target BlitzRecompiled

# The latest weekly rosters (build/blitz-rosters, tools/rosters_publish.sh keeps it) and their headshots
# (build/roster_faces, tools/roster_headshots.py) into a built app's resources:
# a device with no download yet (no token, no hub, offline) starts from them (rosters_live.cpp). Also build/rosters_hub
# (a hub's address), or build/rosters_token with build/rosters_repo (a read-only token and the rosters repo's
# owner/name), when there, so every device the build goes to downloads new rosters by itself. build/ is never
# committed. A public build carries none of this (BLITZ_CONTENT=public). $(call roster_snapshot,<folder>)
roster_snapshot = if [ -f build/blitz-rosters/manifest.json ]; then rm -rf $(1)/rosters && mkdir -p $(1)/rosters && \
	cp build/blitz-rosters/manifest.json build/blitz-rosters/players.json $(1)/rosters/ && cp -R build/blitz-rosters/lineups $(1)/rosters/ && \
	if [ -d build/roster_faces ]; then mkdir -p $(1)/rosters/faces && cp build/roster_faces/*.jpg $(1)/rosters/faces/; \
	else echo "no build/roster_faces: no headshots in the app (tools/roster_headshots.py)"; fi; \
	else echo "no build/blitz-rosters: the app ships without a roster snapshot (tools/rosters_publish.sh clones it)"; fi; \
	for f in token hub repo; do if [ -s build/rosters_$$f ]; then mkdir -p $(1)/rosters && cp build/rosters_$$f $(1)/rosters/$$f; fi; done

# The season server's address (build/pak_server, one line; not committed) into a built app's resources: Blitz Builder
# and Add-Ons download the 2026 Season from it (season_pak.cpp). Without it the app offers the season from a file only.
# $(call pak_server_file,<folder>)
pak_server_file = if [ -s build/pak_server ]; then mkdir -p $(1)/paks && cp build/pak_server $(1)/paks/server; \
	else echo "no build/pak_server: no season server in this build (Add-Ons still installs a .nbpak)"; fi

# Modern's content (the engine's houston files; art, uniforms and announcer from <app folder>; the roster snapshot)
# into a built app folder: $(call modern_content,<folder>)
# BLITZ_CONTENT=public: only the app's own (the credits page, the season server's address).
modern_content = R=$(1); C=$(SEASON_APP_FOLDER); \
	rm -rf $$R/houston $$R/art/vs_right $$R/art/credits.wms $$R/uniforms $$R/announcer $$R/probowl $$R/rosters $$R/paks; \
	mkdir -p $$R/art && cp art/credits/credits.wms $$R/art/credits.wms; $(call pak_server_file,$$R); \
	if [ -n "$(PUBLIC)" ]; then rm -rf $$R/packs/blitz2026 $$R/packs/pac12; else $(call roster_snapshot,$(1)); \
	if [ -d build/engine32/houston ]; then cp -R build/engine32/houston $$R/houston; fi; \
	for d in $(SEASON_PARTS); do if [ -d $$C/$$d ]; then mkdir -p $$R/$$d && cp -R $$C/$$d/. $$R/$$d/; fi; done; fi

win-modern: recomp-modern rsp patches   ## build/win_modern: Modern Blitz for Windows x64, with Modern's content from <app folder> (APP_FOLDER=<dir>)
	@[ -f build/xwin/redist/vcruntime140.dll ] || bash tools/windows/fetch_vc_runtime.sh
	$(call SS_CODE,build/engine32/savestate/RecompiledFuncs,build/engine32/RecompiledFuncs)
	$(WIN_CMAKE) -S . -B build/win_modern -DBLITZ_FLAVOUR=modern -DBLITZ_CONTENT=$(BLITZ_CONTENT) $(SS_FLAG)
	cmake --build build/win_modern --target BlitzRecompiled
	@$(call modern_content,build/win_modern); echo "-> $$R/New Blitz.exe"

# Both Windows games in one folder (build/win/BlitzRecomp/Classic, .../Modern; Switch to … in the launcher), the app
# files only, ready to zip: build/win/BlitzRecomp.zip.
WIN_APP_FILES = SDL2.dll dxcompiler.dll dxil.dll msvcp140.dll vcruntime140.dll vcruntime140_1.dll recompcontrollerdb.txt assets packs art houston uniforms announcer paks
win: win-classic win-modern   ## build/win/BlitzRecomp(.zip): Classic and Modern for Windows x64 side by side
	rm -rf build/win/BlitzRecomp build/win/BlitzRecomp.zip
	mkdir -p build/win/BlitzRecomp/Classic build/win/BlitzRecomp/Modern
	cp "build/win_classic/Classic Blitz.exe" build/win/BlitzRecomp/Classic/
	cp "build/win_modern/New Blitz.exe" build/win/BlitzRecomp/Modern/
	@for f in $(WIN_APP_FILES); do \
		if [ -e build/win_classic/$$f ]; then cp -R build/win_classic/$$f build/win/BlitzRecomp/Classic/; fi; \
		if [ -e build/win_modern/$$f ]; then cp -R build/win_modern/$$f build/win/BlitzRecomp/Modern/; fi; \
	done
	cd build/win && zip -qr BlitzRecomp.zip BlitzRecomp
	@echo "-> build/win/BlitzRecomp.zip"

# iPhone/iPad, sideloaded (docs/IOS_FEASIBILITY.md): build/ios/app_*/BlitzRecompiled.app, then tools/ios/install.sh
# signs it with the team's team and installs it on the connected iPhone. Needs tools/ios/build_deps.sh once.
IOS_CMAKE = cmake -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos \
	-DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 $(REL_FLAGS)

ios-classic: recomp rsp patches   ## build/ios/app_classic: Classic Blitz for iPhone (tools/ios/install.sh classic)
	$(call SS_CODE,build/savestate/RecompiledFuncs,build/RecompiledFuncs)
	$(IOS_CMAKE) -S . -B build/ios/app_classic -DBLITZ_FLAVOUR=classic $(SS_FLAG)
	cmake --build build/ios/app_classic --target BlitzRecompiled

ios-modern: recomp-modern rsp patches   ## build/ios/app_modern: Modern Blitz for iPhone, content from <app folder> (APP_FOLDER=<dir>)
	$(call SS_CODE,build/engine32/savestate/RecompiledFuncs,build/engine32/RecompiledFuncs)
	$(IOS_CMAKE) -S . -B build/ios/app_modern -DBLITZ_FLAVOUR=modern -DBLITZ_CONTENT=$(BLITZ_CONTENT) $(SS_FLAG)
	cmake --build build/ios/app_modern --target BlitzRecompiled
	@$(call modern_content,build/ios/app_modern/BlitzRecompiled.app); echo "-> $$R (tools/ios/install.sh modern)"

# Android, sideloaded (docs/ANDROID.md): build/android/app_*/<Name>.apk, built with the NDK and the SDK's tools (no
# Gradle; tools/android/env.sh finds them), then `make android-install` puts it on the connected device or emulator.
# Save states and Redo Play on every platform: the resumable copy of the game's code (tools/savestate/resumable.py),
# made when it's missing or older than the code: $(call SS_CODE,<out>,<in>). SAVESTATES=0 builds without.
SS_CODE = @if [ "$(SAVESTATES)" = 1 ] && { [ ! -f $(1)/recomp_overlays.inl ] || [ $(2)/recomp_overlays.inl -nt $(1)/recomp_overlays.inl ]; }; then $(PY) tools/savestate/resumable.py --in $(2) --out $(1) --rom build/baserom.z64 --sinks sleep,recv > /dev/null; fi

android-classic: recomp rsp patches   ## build/android/app_classic/ClassicBlitz.apk: Classic Blitz for Android (arm64)
	$(call SS_CODE,build/savestate/RecompiledFuncs,build/RecompiledFuncs)
	tools/android/build.sh classic $(SS_FLAG) $(REL_FLAGS)
	tools/android/build_apk.sh build/android/app_classic

android-modern: recomp-modern rsp patches   ## build/android/app_modern/NewBlitz.apk: Modern Blitz for Android, content from <app folder> (APP_FOLDER=<dir>)
	$(call SS_CODE,build/engine32/savestate/RecompiledFuncs,build/engine32/RecompiledFuncs)
	tools/android/build.sh modern $(SS_FLAG) -DBLITZ_CONTENT=$(BLITZ_CONTENT) $(REL_FLAGS)
	@$(call modern_content,build/android/app_modern/apk/assets/res)
	BLITZ_CONTENT=$(BLITZ_CONTENT) tools/android/build_apk.sh build/android/app_modern

android-install:   ## Install the Android builds on the connected device or emulator (FLAVOUR=classic|modern, default both)
	@for f in $(or $(FLAVOUR),classic modern); do a=$$(ls -t build/android/app_$$f/*.apk 2>/dev/null | head -1); \
		if [ -n "$$a" ]; then echo "$$a"; $${ANDROID_HOME:-$$HOME/Library/Android/sdk}/platform-tools/adb install -r "$$a"; fi; done

# Linux x86_64 (Steam Deck, desktops), cross-built on this Mac (docs/PLATFORMS.md): needs tools/linux/make_sysroot.sh
# once. build/linux/app_*: the executable and its resources; running it needs a Linux PC.
LINUX_CMAKE = cmake -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_TOOLCHAIN_FILE=platform/linux/toolchain-x86_64.cmake $(REL_FLAGS)

linux-classic: recomp rsp patches   ## build/linux/app_classic: Classic Blitz for Linux x86_64
	@[ -d build/linux/sysroot-x86_64/usr/include ] || tools/linux/make_sysroot.sh
	$(call SS_CODE,build/savestate/RecompiledFuncs,build/RecompiledFuncs)
	$(LINUX_CMAKE) -S . -B build/linux/app_classic -DBLITZ_FLAVOUR=classic $(SS_FLAG)
	cmake --build build/linux/app_classic --target BlitzRecompiled

linux-modern: recomp-modern rsp patches   ## build/linux/app_modern: Modern Blitz for Linux x86_64, content from <app folder> (APP_FOLDER=<dir>)
	@[ -d build/linux/sysroot-x86_64/usr/include ] || tools/linux/make_sysroot.sh
	$(call SS_CODE,build/engine32/savestate/RecompiledFuncs,build/engine32/RecompiledFuncs)
	$(LINUX_CMAKE) -S . -B build/linux/app_modern -DBLITZ_FLAVOUR=modern -DBLITZ_CONTENT=$(BLITZ_CONTENT) $(SS_FLAG)
	cmake --build build/linux/app_modern --target BlitzRecompiled
	@$(call modern_content,build/linux/app_modern); echo "-> $$R"

# PS5 homebrew (docs/PS5_HOMEBREW.md): title folders to copy to /data/homebrew/<title ID> on a jailbroken console.
# Needs tools/ps5/setup_sdk.sh and tools/ps5/build_radv.sh once.
PS5_ENV = PS5_PAYLOAD_SDK=$(CURDIR)/build/ps5/sdk-m LLVM_CONFIG=/opt/homebrew/opt/llvm/bin/llvm-config
PS5_CMAKE = $(PS5_ENV) cmake -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_TOOLCHAIN_FILE=$(CURDIR)/build/ps5/sdk-m/toolchain/prospero.cmake $(REL_FLAGS)

# The home screen's background and launch splash from the launcher logo: $(call ps5_art,<title folder>,<logo>)
ps5_art = $(PY) tools/ps5/make_title_art.py $(2) $(1)/sce_sys && \
	for p in pic0 pic1; do build/ps5/host/png_to_bc7_dds $(1)/sce_sys/$$p.png $(1)/sce_sys/$$p.dds > /dev/null && rm $(1)/sce_sys/$$p.png; done

# SDL2 is rebuilt with platform/sdl-ps5-patches when they changed since the installed one was built (its stamp).
.PHONY: ps5-sdl
ps5-sdl:
	@want=$$(cat platform/sdl-ps5-patches/*.patch | shasum -a 256 | cut -d' ' -f1); \
	if [ "$$(cat build/ps5/prefix/.sdl-patches 2>/dev/null)" != "$$want" ]; then echo "SDL2 for the PS5: rebuilding with platform/sdl-ps5-patches"; tools/ps5/setup_sdk.sh sdl; fi

ps5-classic: recomp rsp patches ps5-sdl   ## build/ps5/app_classic/PPSA99960: Classic Blitz as a PS5 title folder
	$(call SS_CODE,build/savestate/RecompiledFuncs,build/RecompiledFuncs)
	$(PS5_CMAKE) -S . -B build/ps5/app_classic -DBLITZ_FLAVOUR=classic $(SS_FLAG)
	$(PS5_ENV) cmake --build build/ps5/app_classic --target BlitzRecompiled
	@$(call ps5_art,build/ps5/app_classic/PPSA99960,icons/logo_2026.png)
	@echo "-> build/ps5/app_classic/PPSA99960"

ps5-modern: recomp-modern rsp patches ps5-sdl   ## build/ps5/app_modern/PPSA99961: Modern Blitz as a PS5 title folder, content from <app folder> (APP_FOLDER=<dir>)
	$(call SS_CODE,build/engine32/savestate/RecompiledFuncs,build/engine32/RecompiledFuncs)
	$(PS5_CMAKE) -S . -B build/ps5/app_modern -DBLITZ_FLAVOUR=modern -DBLITZ_CONTENT=$(BLITZ_CONTENT) $(SS_FLAG)
	$(PS5_ENV) cmake --build build/ps5/app_modern --target BlitzRecompiled
	@$(call ps5_art,build/ps5/app_modern/PPSA99961,icons/logo_2026.png)
	@$(call modern_content,build/ps5/app_modern/PPSA99961); echo "-> $$R"

# A public release's downloads (tools/release/make_release.sh): every app public (BLITZ_CONTENT=public) and RELEASE=1, the
# Mac zip, the Windows zip and the Linux tarball, each with the PS5, Android and (unsigned) iPhone kits, SHA256SUMS and
# RELEASE_NOTES.md in build/release/<version>, then tools/release_scan.sh over all of it. VERSION defaults to
# CMakeLists.txt's BLITZ_VERSION. RELEASE_SKIP_BUILD=1 repackages what's built.
release:   ## build/release/<VERSION>/: the public Mac, Windows and Linux downloads with the device kits, checked by tools/release_scan.sh
	tools/release/make_release.sh $(or $(VERSION),$(shell sed -n 's/^ *set(BLITZ_VERSION "\(.*\)")/\1/p' CMakeLists.txt))

.PHONY: release

all-platforms:   ## Build Classic and Modern for every platform set up here (tools/release/build_all.sh; PLATFORMS="ios android" for some)
	tools/release/build_all.sh $(PLATFORMS)

roster-update:   ## Refresh the 2026 pack's players from ESPN's current rosters and depth charts (tools/roster_fetch.py)
	.venv/bin/python tools/roster_fetch.py --apply

pack-edits:   ## Apply Roster Room edits to packs/blitz2026 (EDITS=<folder of team .json files or an exported pack.json>)
	.venv/bin/python tools/roster_pack.py edits $(EDITS) packs/blitz2026

vs-banners:   ## VS screen city banners LAS VEGAS / LOS ANGELES -> build/appdata2026/art/files (APP_FOLDER=<dir> for another)
	.venv/bin/python tools/make_vs_banners.py --app-folder $(or $(APP_FOLDER),build/appdata2026)

logo2027-art:   ## the team's NFL Blitz 2027 logo over the 2000 one in the title, New Features and badges, from your ROM -> <app folder>/art/files (APP_FOLDER=<dir>, default build/appdata2026)
	.venv/bin/python tools/make_logo2027.py --from-rom --app-folder $(or $(APP_FOLDER),build/appdata2026)

logo27:   ## Draw the NFL Blitz '27 logo (our art) -> build/art/blitzlog.wms, ready for <app folder>/art/files/
	$(VENV)/bin/python tools/make_logo27.py
	$(VENV)/bin/python tools/wms.py encode build/art/blitzlog27_232x73.png build/art/blitzlog.wms --origin 108 200 --flags 4

# ---- One app with both games (docs/design/ONE_APP.md) ----------------------------------------------------------
# build/dist/BlitzRecomp.app (+ .zip): the starter and both engines. Modern is the original-game engine (app-modern);
# `make bundle-teamplus` puts the Team Extension engine in its place (for community ROMs built on it), and
# `make bundle-classic` leaves Modern out entirely; see docs/ZOINKITY.md.
bundle: app app-modern   ## build/dist/BlitzRecomp.app with Classic and Modern (SIGN_ID as for dist; BLITZ_CONTENT=public for the public one)
	cmake --build build/app --target BlitzRecompStarter
	mkdir -p build/dist && rm -f build/dist/BlitzRecomp-macos-arm64.zip
	tools/make_bundle.sh build/dist/BlitzRecomp.app build/app/BlitzRecompiled.app $(APP_MODERN_DIR)/BlitzRecompiled.app build/app/BlitzRecomp "$(SIGN_ID)"
	cd build/dist && ditto -c -k --keepParent BlitzRecomp.app BlitzRecomp-macos-arm64.zip

bundle-teamplus: app app2026   ## build/dist/BlitzRecomp.app with Classic and the Team Extension's Modern (personal use)
	cmake --build build/app --target BlitzRecompStarter
	mkdir -p build/dist && rm -f build/dist/BlitzRecomp-macos-arm64.zip
	tools/make_bundle.sh build/dist/BlitzRecomp.app build/app/BlitzRecompiled.app build/app2026/BlitzRecompiled.app build/app/BlitzRecomp "$(SIGN_ID)"
	cd build/dist && ditto -c -k --keepParent BlitzRecomp.app BlitzRecomp-macos-arm64.zip

regress:   ## Regression suite for Modern on the original game (tools/regress/engine32.sh check; `record` for new baselines)
	tools/regress/engine32.sh check

bundle-classic: app   ## build/dist/BlitzRecomp.app with Classic only (nothing of the Team Extension)
	cmake --build build/app --target BlitzRecompStarter
	mkdir -p build/dist && rm -f build/dist/BlitzRecomp-macos-arm64.zip
	tools/make_bundle.sh build/dist/BlitzRecomp.app build/app/BlitzRecompiled.app - build/app/BlitzRecomp "$(SIGN_ID)"
	cd build/dist && ditto -c -k --keepParent BlitzRecomp.app BlitzRecomp-macos-arm64.zip

# ---- PSP (docs/PSP_PORT.md; not in all-platforms until the hardware day) -----------------------------------------
# The game's code with 32-bit registers: tools/psp/gpr32.py over the plain recompiled C (never the save-state copy),
# compiled with RECOMP_GPR32 (platform/n64recomp-patches). The pass fails on any 64-bit integer op or a changed div count.
psp-recomp: recomp recomp-modern   ## build/psp/classic|modern/RecompiledFuncs: the game's code for 32-bit registers (tools/psp/gpr32.py)
	$(PY) tools/psp/gpr32.py --selftest > /dev/null
	$(PY) tools/psp/gpr32.py --in build/RecompiledFuncs --out build/psp/classic/RecompiledFuncs --flavour classic
	$(PY) tools/psp/gpr32.py --in build/engine32/RecompiledFuncs --out build/psp/modern/RecompiledFuncs --flavour modern

# ---- PSP probe, logs and commands (docs/PSP_PORT.md §5, §7, M1/M2) ----------------------------------------------------------
# The SDK is build/psp/sdk/pspdev (psp-gcc 15.2); only these targets put it on PATH.
PSP_SDK = $(CURDIR)/build/psp/sdk/pspdev
PSP_ENV = PSPDEV=$(PSP_SDK) PATH="$(PSP_SDK)/bin:$$PATH"

psp-probe:   ## build/psp/probe/EBOOT.PBP (+ nbprobe.prx for PSPLink, nbprobe for psp-addr2line): the PSP probe (docs/PSP_PORT.md §7)
	$(PSP_ENV) psp-cmake -S platform/psp/probe -B build/psp/probe -DCMAKE_BUILD_TYPE=Release > /dev/null
	$(PSP_ENV) cmake --build build/psp/probe
	@echo "-> build/psp/probe/EBOOT.PBP"

psp-run:   ## Run a PSP build in PPSSPP as a PSP-1000 (APP=probe|<EBOOT path>, SECS=120); output in build/psp/runs/<stamp>/
	tools/psp/run_ppsspp.sh $(or $(APP),probe) $(or $(SECS),120)

psp-nettest:   ## The PSP log/HTTP/command code built for the Mac, checked against the mock Worker (start make psp-mock first)
	cmake -S platform/psp/probe -B build/psp/nettest -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo > /dev/null
	cmake --build build/psp/nettest
	build/psp/nettest/psp-nettest

psp-mock:   ## A local mock of the logs Worker and the rosters hub on 127.0.0.1:8788 (fake keys; Ctrl-C to stop)
	python3 -I tools/psp/mock_worker.py

.PHONY: psp-probe psp-run psp-nettest psp-mock

# ---- NB Remote: the PSP's Wi-Fi remote control (docs/PSP_PORT.md "NB Remote") -----------------------------------------------
NBR_PLUGIN_FORBID = SysclibForKernel,IoFileMgrForKernel,ThreadManForKernel,LoadExecForKernel,SysMemForKernel
psp-remote:   ## NB Remote: build/psp/nbremote/EBOOT.PBP (PSP/GAME/NBREMOTE) + the VSH plugin build/psp/nbremote_plugin/{launch/nbremote,net/nbremote_net}.prx
	$(PSP_ENV) psp-cmake -S platform/psp/nbremote -B build/psp/nbremote -DCMAKE_BUILD_TYPE=Release > /dev/null
	$(PSP_ENV) cmake --build build/psp/nbremote
	@for v in launch net; do mkdir -p build/psp/nbremote_plugin/$$v && rm -f build/psp/nbremote_plugin/$$v/*.o && \
	  $(PSP_ENV) $(MAKE) -s -C build/psp/nbremote_plugin/$$v -f $(CURDIR)/platform/psp/nbremote/plugin/Makefile VARIANT=$$v \
	    BUILD_TAG="$$(git rev-list --count HEAD) $$(git rev-parse --short=7 HEAD)" > /dev/null || exit 1; done
	@python3 -I tools/psp/prx_info.py build/psp/nbremote_plugin/launch/nbremote.elf --forbid $(NBR_PLUGIN_FORBID) | head -1
	@python3 -I tools/psp/prx_info.py build/psp/nbremote_plugin/net/nbremote_net.elf --forbid $(NBR_PLUGIN_FORBID) > /dev/null
	@echo "-> build/psp/nbremote/EBOOT.PBP, build/psp/nbremote_plugin/launch/nbremote.prx, build/psp/nbremote_plugin/net/nbremote_net.prx"

psp-remote-test:   ## NB Remote's core and the plugin's decisions on the Mac, against the mock Worker (tools/psp/nbremote_test.sh)
	cmake -S platform/psp/nbremote -B build/psp/nbremote_host -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo > /dev/null
	cmake --build build/psp/nbremote_host
	tools/psp/nbremote_test.sh

psp-remote-ppsspp:   ## NB Remote in headless PPSSPP: ping, refusals, update + run Classic, Classic's exit back (make psp-remote psp-classic first)
	tools/psp/nbremote_ppsspp_test.sh $(or $(SECS),240)

.PHONY: psp-remote psp-remote-test psp-remote-ppsspp

# ---- M6: the sceGu renderer (src/psp/gfx) and its display-list replay (docs/PSP_PORT.md 4.5) --------------------------------
psp-dlreplay:   ## build/psp/dlreplay/EBOOT.PBP: renders build/dumps through src/psp/gfx (run + compare: tools/psp/dlreplay.sh)
	$(PSP_ENV) psp-cmake -S platform/psp/dlreplay -B build/psp/dlreplay -DCMAKE_BUILD_TYPE=Release > /dev/null
	$(PSP_ENV) cmake --build build/psp/dlreplay
	@echo "-> build/psp/dlreplay/EBOOT.PBP"

psp-gfxtest:   ## The renderer's walker on the Mac (null GE backend), checked against tools/gbi_dump.py's decode of the M6 dumps
	cmake -S platform/psp/dlreplay -B build/psp/gfxtest -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo > /dev/null
	cmake --build build/psp/gfxtest
	$(PY) -I tools/psp/gbi_check.py

.PHONY: psp-dlreplay psp-gfxtest

# ---- psp-host: the PSP's headless core on the Mac (docs/PSP_PORT.md M4) ------------------------------------------------------
# build/psp/host_<flavour>[_memcheck|_stock]/newblitz-host[-stock]: the gpr32 code (default), MEMCHECK=1 (every RDRAM address
# checked), or GPR32=0 (the stock 64-bit code, the other half of tools/psp/lockstep.sh). Null renderer, silent audio, stdout logs.
PSP_HOST_FLAVOUR = $(or $(FLAVOUR),classic)
PSP_HOST_DIR = build/psp/host_$(PSP_HOST_FLAVOUR)$(if $(filter 1,$(MEMCHECK)),_memcheck)$(if $(filter 0,$(GPR32)),_stock)
psp-host:   ## build/psp/host_<flavour>/newblitz-host: the PSP core for the Mac, headless (FLAVOUR=classic, MEMCHECK=1, GPR32=0)
	cmake -S platform/psp -B $(PSP_HOST_DIR) -G Ninja -DCMAKE_BUILD_TYPE=Release -DPSP_TARGET=host -DFLAVOUR=$(PSP_HOST_FLAVOUR) \
		-DMEMCHECK=$(if $(filter 1,$(MEMCHECK)),ON,OFF) -DGPR32=$(if $(filter 0,$(GPR32)),OFF,ON) > /dev/null
	cmake --build $(PSP_HOST_DIR)
	@echo "-> $(PSP_HOST_DIR)  (run: $(PSP_HOST_DIR)/newblitz-host* --rom <rom> --secs 120)"

psp-lockstep:   ## M3(b): stock vs gpr32 psp-host on virtual time, RDRAM CRCs every 100 VIs (VIS=3000)
	$(MAKE) psp-host GPR32=0
	$(MAKE) psp-host
	tools/psp/lockstep.sh build/psp/host_classic_stock/newblitz-host-stock build/psp/host_classic/newblitz-host $(or $(VIS),3000)

.PHONY: psp-host psp-lockstep

# ---- The Classic EBOOT (docs/PSP_PORT.md M5) --------------------------------------------------------------------------------
# build/psp/app_classic/EBOOT.PBP (+ newblitz.prx for PSPLink, newblitz.elf for psp-addr2line): null renderer with the
# debug-screen text, silent audio, sceCtrl + script input, the four log sinks and the remote verbs. MEMCHECK=1 checks every
# RDRAM address (build/psp/app_classic_memcheck). Needs make psp-recomp first (build/psp/classic/RecompiledFuncs).
PSP_APP_DIR = build/psp/app_classic$(if $(filter 1,$(MEMCHECK)),_memcheck)
psp-classic:   ## build/psp/app_classic/EBOOT.PBP: New Blitz Classic for the PSP (MEMCHECK=1 for the checked build)
	$(PSP_ENV) psp-cmake -S platform/psp -B $(PSP_APP_DIR) -G Ninja -DCMAKE_BUILD_TYPE=Release -DPSP_TARGET=psp -DFLAVOUR=classic \
		-DMEMCHECK=$(if $(filter 1,$(MEMCHECK)),ON,OFF) > /dev/null
	$(PSP_ENV) cmake --build $(PSP_APP_DIR)
	@$(PSP_ENV) psp-size $(PSP_APP_DIR)/newblitz.elf
	@echo "-> $(PSP_APP_DIR)/EBOOT.PBP (+ newblitz.prx, newblitz.elf)"

# Modern (docs/PSP_PORT.md M8): build/psp/app_modern/EBOOT.PBP from build/psp/modern/RecompiledFuncs (make psp-recomp). Its ROM
# is the kit's (Modern's art merged on the Mac by psp-host's newblitz-kit: make psp-kit).
PSP_MODERN_DIR = build/psp/app_modern$(if $(filter 1,$(MEMCHECK)),_memcheck)
psp-modern:   ## build/psp/app_modern/EBOOT.PBP: New Blitz (Modern) for the PSP (MEMCHECK=1 for the checked build)
	$(PSP_ENV) psp-cmake -S platform/psp -B $(PSP_MODERN_DIR) -G Ninja -DCMAKE_BUILD_TYPE=Release -DPSP_TARGET=psp -DFLAVOUR=modern \
		-DMEMCHECK=$(if $(filter 1,$(MEMCHECK)),ON,OFF) > /dev/null
	$(PSP_ENV) cmake --build $(PSP_MODERN_DIR)
	@$(PSP_ENV) psp-size $(PSP_MODERN_DIR)/newblitz.elf
	@echo "-> $(PSP_MODERN_DIR)/EBOOT.PBP (+ newblitz.prx, newblitz.elf)"

psp-kit:   ## build/psp/kit/PSP/GAME/{NEWBLITZ,NBCLASSIC,NBPROBE,NBREMOTE} + SEPLUGINS/: EBOOTs, ROMs + sidecars, Modern's merged art, rosters (PPSSPP=1: PPSSPP's memstick)
	$(MAKE) psp-host FLAVOUR=modern
	$(MAKE) psp-remote
	tools/psp/kit.sh $(if $(filter 1,$(PPSSPP)),--ppsspp)

psp-deploy:   ## Copy the kit to a Memory Stick and write data/net.cfg from build/ secrets (MS=/Volumes/<stick>; LINK=1 for PSPLink's host0; PLUGIN=on|off: NB Remote's plugin line)
	tools/psp/deploy.sh $(if $(MS),--ms "$(MS)") $(if $(filter 1,$(LINK)),--link) $(if $(PLUGIN),--plugin $(PLUGIN))

.PHONY: psp-classic psp-modern psp-kit psp-deploy
