# PS5 homebrew: running BlitzRecomp natively on a jailbroken PS5

**October 21, 2026: both games build as PS5 title folders; not yet run on a console.** Scope: our port as native PS5
homebrew (a home-screen title on a jailbroken console), the way other static recompilations already run there. Sony's
official route (partner program, NDA SDK, dev kits) isn't available to this project and isn't considered.

## Status

- **Title folders (2026-10-21):** `make ps5-classic` → `build/ps5/app_classic/PPSA99960`, `make ps5-modern` →
  `build/ps5/app_modern/PPSA99961` (Modern's content from `APP_FOLDER`, as `make win-modern`). Each holds `eboot.bin`
  (fake-signed SELF), `sce_module/libc.prx`, `sce_sys/param.json` + `icon0.png`, and the app's resources.
- **How it's linked** (`tools/ps5/link_title.sh`, the app's link launcher on the PS5): UnleashedRecomp-Prospero's
  recipe, used from its checkout in `build/ps5/deps/unleashed-prospero` (GPL-3.0, not copied into this repository):
  its `_start`, Mesa RADV linked whole, the SDK's libc++ and platform layer with its libc wraps, AGC link stubs, then
  `ps5-native-tool link` and `self --sign`. `platform_ps5.cpp` supplies the few libc functions neither side has.
- **Console specifics in the code:** SDL on its dummy video driver (RADV owns the TV output, through Plume's
  `VK_KHR_display` surface: plume patch 0002); a 3840x2160 UI; resources from `/app0`; the app folder in
  `/data/blitzrecomp/<program id>`; RDRAM in direct memory from the platform's shared memory (runtime patch 0007);
  the ROM imported from `/data/blitzrecomp/` at startup (no file picker); no mods (no runtime code loading).
- **Wave 2 (2026-10-21, built, untested):**
  - weekly roster downloads through the console's HTTP/2 library (`platform_ps5.cpp`, as the SDK's
    `samples/http2_get`);
  - phone controllers (the same server as on a Mac);
  - the Roster Room served on the local network, its address shown, for a phone or computer to open;
  - Switch to Classic/Modern, which asks the system to start the other title (both must be installed);
  - the home screen's background and launch splash (`tools/ps5/make_title_art.py`, BC7 DDS via UnleashedRecomp-
    Prospero's `png_to_bc7_dds`).

  Screenshots and clips are the console's own (the Create button).
- **Review fixes (2026-10-21):** every exit goes through the system shell (`sceSystemServiceLoadExec("exit")`:
  the kernel's exit raises SIGSYS in a title), from main's return and wrapped exit/_Exit/quick_exit; the launch
  splash is hidden first thing; no audio device is no longer fatal; RT64 uses the app's SDL window instead of making a
  second one; Switch starts the other title before quitting and stays if the system refuses; the network address
  comes from the console's network control library.
- **Not yet:** home-screen music (snd0), text entry (no on-screen keyboard under SDL's dummy video driver).
- **Target console:** the team's PS5, firmware 13.60 with etaHEN.
- **Toolchain** (once: `brew install llvm make meson glslang bison`):
  - `tools/ps5/setup_sdk.sh`: the [PS5_PayloadSDK fork](https://github.com/mihawk-99/PS5_PayloadSDK) UnleashedRecomp-Prospero
    pins (`95c08f2`: upstream v0.42 plus ps5platform) into `build/ps5/sdk-m`; SDL2's PS5 port (joystick, audio,
    events) with `platform/sdl-ps5-patches` on top (`setup_sdk.sh sdl` rebuilds it alone; `make ps5-*` does that
    when the patches changed), FreeType and libsamplerate into `build/ps5/prefix`; the UnleashedRecomp-Prospero checkout (pinned, its
    `runtime/libc.prx` checked against its published hash) and `ps5-native-tool` built for the Mac.
  - `tools/ps5/build_radv.sh`: Mesa RADV for the PS5 from [PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa) `0b2d6d1`,
    with their options and OpenCL cache, into `build/ps5/radv` (`libvulkan_radeon.ps5.a`). Needs mako/ply/pyyaml in
    `build/ps5/pydeps`.

## Where we left off (2026-10-21, evening)

**Both games run on the team's PS5** (13.60, etaHEN, ShadowMountPlus; the console at 192.168.0.90).
- **Classic (PPSA99960):** boots to the launcher, plays to kickoff, and Quit closes cleanly
  (`[ps5] closing (status 0)`).
- **Modern (PPSA99961):** boots, plays, and closes cleanly. RADV reports "PlayStation 5 GPU (RADV NAVI21)" and its
  submission timing is steady. No crashes in any log.

Learned bringing it up:
- **Startup crash:** the throwing `std::filesystem::exists` aborted in the sandbox (frontend patch 0014). Boot
  breadcrumbs in `/data/blitzrecomp/boot.log` found it (`platform_ps5.cpp`, from `.preinit_array`).
- **Registering titles:** ShadowMountPlus does it, not `register.c` (that payload hung inside the system's install
  call). It registers at its startup scan, and sometimes on its 15 s scan. It stalls while another title is open
  or "awaiting same-app handoff", which a PS5 restart plus etaHEN clears.
- **Deploying:** etaHEN's FTP (port 1337) runs about 0.4 MB/s with pauses (a 41 MB eboot takes about 2 minutes)
  and drops a second connection. `deploy.sh` resumes, skipping files already there.
- **Controls:** the DualSense Create button belongs to the system: the touchpad click opens the menu (frontend patch
  0015).

Next, in order:
1. **Season Mode:** Modern on the PS5 saved "Standard" before Advanced became the default: switch it once in
   Settings (or edit `season.json` while the game is closed). Then rebuild and deploy Modern's eboot so fresh
   installs default to Advanced.
2. **Weekly lineups:** a read-only token for the roster job's repo in
   `/data/blitzrecomp/Blitz2026Recompiled/rosters/token` (made by hand). Also treat an empty roster cache as
   absent, which quiets the parse errors.
3. **deploy.sh:** trigger ShadowMountPlus properly (rewriting an empty `manual.lst` doesn't change its time
   here). Then test phone controllers, the Roster Room from a phone, and Switch between titles on the console.
4. **The rest of the Port Test Night checks** (sound, a full drive, home screen art), then performance at 4K.

## Testing on the console

1. **Console side:** etaHEN running (it provides the FTP server on port 1337 and the ELF loader on 9021), the PS5 and
   the Mac on the same network.
2. **Copy and start:** `tools/ps5/deploy.sh <PS5 IP> classic path/to/blitz.z64` (or `modern`). It uploads the title
   folder to `/data/homebrew/PPSA99960/` (Modern: `PPSA99961`) in one FTP session, each file renamed into place
   when complete, the ROM to `/data/blitzrecomp/blitz.z64`, and sends a small launch payload to port 9021. The ROM only
   needs copying once; both games use it (at startup, loose ROMs before zips, each tried until one is the game).
3. **If the launch payload can't start it,** the title isn't registered with the system yet. Try
   `REGISTER=1 tools/ps5/deploy.sh …` (an experimental payload asking the system to install the folder:
   `tools/ps5/register.c`), or register it with the console's homebrew tools (etaHEN's toolbox / Itemzflow /
   ShadowMountPlus), then start it from the home screen.
4. **What to look for:** the launcher on the TV, the DualSense moving through it, Start Game reaching the title screen,
   sound. Then `/data/blitzrecomp/BlitzRecompiled/blitz.log` (Modern: `Blitz2026Recompiled/blitz.log`) over FTP has
   what happened, including `[ps5] ROM …`, `[ps5] rdram in direct memory` and any crash; the same lines go to the
   kernel log (klogsrv, TCP 3232, if one is running). A title can't dup2, so the log takes over the stdout and stderr
   streams instead (app_log.cpp). Export ROM saves to `/data/blitzrecomp/exports/`.
5. **A hang or a freeze:** `/data/blitzrecomp/boot.log` (the run before's: `boot.prev.log`) has crumbs written straight to
   the file, past the log pipe: the start-up steps, Save & Quit's and a resume's steps, `stall:` when the UI thread or
   the game's frames stop for 10 s mid-run, and `quit: ...` lines. A quit that hasn't closed the title 8 s after the
   game was told to stop is closed through the system shell anyway (platform_ps5.cpp `ps5_watch`).
6. **Driving it from the Mac (1.0.2):** put an empty `/data/blitzrecomp/live.txt` there before the title starts, and
   lines appended to it over FTP (`APPE`) are dev_input.cpp's live commands (presses, `QUITPROMPT`, `KEY Return`,
   `REPLAY open`, `CONFIG ...`), read from the file's end each run. Fake phones (the websocket protocol in
   phone_pads.cpp) play as controllers. Delete live.txt afterwards: without it nothing is read.
7. **Seeing the screen with no capture:** the live command `DUMP` writes the game's RAM to
   `Blitz2026Recompiled/dumps/rdram_<t>.bin` (8 MB; the log line names the display list), and
   `tools/gbi_dump.py <dump> <dlist> --render out.png` draws that frame on the Mac (the game's picture only, not the app's
   menus over it). Delete the dump afterwards. **Never abort an FTP download part way:** on 2026-11-17 etaHEN's FTP
   stopped answering for good soon after one was cut off, and only hands at the console bring it back.
8. **Save & Quit's resume guard** (savestate.cpp, every platform): `savestates/quit.resuming` is on the disk while
   the "Pick up your last game?" toast is first up and while a Resume runs, until the game's frames show. A start that finds it moves
   quit.state aside as `quit.state.failed` (and `quit.txt.failed`) and doesn't offer it. 2026-11-15: a Save & Quit on
   the console froze, and every start after it hung until quit.state was moved aside by hand.

## The precedent: a static recomp already runs on PS5 through Plume

[UnleashedRecomp-Prospero](https://github.com/Nazky/UnleashedRecomp-Prospero) is the Xbox 360 Sonic Unleashed static
recompilation running natively on PS5 at 4K. Its stack is close to ours:

| Layer | UnleashedRecomp-Prospero | BlitzRecomp today | Gap |
|---|---|---|---|
| Game code | Recompiled C++ (PowerPC → C++) | Recompiled C (MIPS → C, N64Recomp) | none in principle: static code, no JIT |
| Renderer | Plume, PS5 Vulkan backend ([Nazky/plume-ps5](https://github.com/Nazky/plume-ps5)) | RT64 on Plume (`lib/rt64/src/contrib/plume`) | RT64 + the plume-ps5 changes (all behind `#if defined(__PROSPERO__)` in `plume_vulkan.cpp/.h`) |
| Vulkan driver | Mesa RADV linked statically (`libvulkan_radeon.ps5.a`, PS5_Mesa, `-Dradv-winsys=ps5`) | MoltenVK/Metal on Mac | link the same static RADV |
| Display | `VK_KHR_display` on SceVideoOut planes (`createPs5DisplaySurface`, 3840x2160) | SDL window + Metal | a surface path instead of an SDL window |
| Input | `scePad` (DualSense), SDL2 for events | SDL2 (`controllers.cpp`) | SDL2 PS5 port (PacBrew / [ps5-payload-dev/SDL](https://github.com/ps5-payload-dev/SDL)) |
| Audio | `sceAudioOut`, 48 kHz S16 | SDL audio (`audio.cpp`) | an `sceAudioOut` backend (PacBrew SDL2 has no audio) |
| Toolchain | clang/lld 18+, `x86_64-sie-ps5`, `-femulated-tls` + `ps5-emutls-cxa.o`, [PS5 payload SDK](https://github.com/KongaTime/PS5_PayloadSDK) libc/libc++ | Homebrew clang, macOS | a cross build (CMake toolchain file) |
| Package | `eboot.bin` + `sce_module/libc.prx` + `sce_sys/`, FSELF-signed (`ps5-native-tool`), `make deploy` over FTP to `/data/homebrew/<TITLE_ID>`, launch via port 9021 | `.app` | a staging step like `platform/windows/windows_stage.cmake` |

Other native homebrew shows the same building blocks: [ps5link-sdk](https://github.com/Rufidj/ps5link-sdk) (native
titles, AGC access, tested on 9.00 with etaHEN, kstuff, ShadowMountPlus), [ps5-opengl](https://github.com/blackbearreloaded/ps5-opengl)
(Mesa/Gallium OpenGL with an SDL2 bridge, used by a Half-Life port), and an N64 *emulator* (mupen64plus-ps5). No RT64 or
N64Recomp game is on PS5 yet that we found, so this would be the first.

## Why it's plausible

- **No JIT needed.** The game is already C; the runtime only needs ordinary threads and memory. (Mods patch code at
  runtime: leave mods out on PS5.)
- **The OS is FreeBSD-based**, so much of the POSIX code that the Windows port must replace (sockets, threads, mmap,
  file I/O) has close equivalents in the payload SDK's libc. Fewer rewrites than Windows in some areas.
- **The renderer layer already has a PS5 backend** in the same library RT64 is built on.

## Risks to check first (in order)

1. **RT64 shader compilation.** RT64 builds SPIR-V at build time, and on desktop it also loads DXC at runtime
   (`rt64_dynamic_libraries.cpp` requires `dxcompiler.dll`). The console can't load unsigned dynamic libraries. Find
   whether RT64 runs with build-time shaders only (its ubershader path), or what its runtime specialisation needs.
2. **Vulkan features RT64 needs vs RADV-on-PS5.** Plume's instance asks for Vulkan 1.2; RT64 uses descriptor indexing
   and other features. RADV is a full Vulkan driver, but the PS5 winsys is new: check RT64's device requirements.
3. **Memory.** N64ModernRuntime reserves guest memory with large mappings and changes protections; check those calls
   and the sizes against what homebrew may map.
4. **Thread-local storage.** Our code uses `thread_local` (e.g. `team_store_original.cpp`). The PS5 needs
   `-femulated-tls` with the emutls shim Unleashed links.
5. **Plume version.** plume-ps5 is a fork of `renderbag/plume`; RT64 pins its own Plume (`d890ac8`, plus our patch).
   Port the `__PROSPERO__` changes onto RT64's copy rather than swapping libraries.
6. **The ROM and files.** No file picker: read the ROM and app folder from fixed paths under `/data/` (e.g.
   `/data/blitzrecomp/`), copied over FTP.

## Original first milestones (done through the title folder)

1. A CMake toolchain file for `x86_64-sie-ps5` (payload SDK sysroot, `-femulated-tls`), building the runtime and
   Classic's recompiled code with graphics and audio stubbed: proves the code compiles and links for the console.
2. A test title that opens RT64 through plume-ps5 and clears the screen on the TV (launcher UI next).
3. Classic booting to the title screen with input; then audio; then Modern with its content folder.

The service boundaries the Windows port introduces (paths, files, HTTP, sockets, process) should be shared with the
PS5 build: one interface, a Mac, a Windows and a PS5 implementation.

## What testing needs

A jailbroken PS5 on a supported firmware with a homebrew launcher (etaHEN or equivalent), reachable on the network
from the Mac for FTP deploy and the port 9021 loader, and kernel logs (klogsrv, TCP 3232) for debugging. Builds run on
the Mac (cross-compile); testing happens on the console.

## Sources

- [UnleashedRecomp-Prospero](https://github.com/Nazky/UnleashedRecomp-Prospero), [plume-ps5](https://github.com/Nazky/plume-ps5)
- [ps5link-sdk](https://github.com/Rufidj/ps5link-sdk), [PS5 payload SDK](https://github.com/KongaTime/PS5_PayloadSDK), [ps5-payload-dev/SDL](https://github.com/ps5-payload-dev/SDL), [ps5-sdl2](https://github.com/cybervisi0n/ps5-sdl2)
- [ps5-opengl](https://github.com/blackbearreloaded/ps5-opengl), [xash3d-ps5](https://github.com/GordonProsperoMan/xash3d-ps5), [GitHub ps5-homebrew topic](https://github.com/topics/ps5-homebrew)
