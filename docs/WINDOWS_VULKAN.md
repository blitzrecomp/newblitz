# Windows/Vulkan from the current BlitzRecomp repo

**Status (2026-10-21, branch `windows-port`): Classic and Modern build and link as Windows x64 executables on the
Mac** (`make win-classic`, `make win-modern`, or `make win` for both side by side in `build/win/BlitzRecomp.zip`;
clang-cl and lld-link with Microsoft's CRT/SDK from xwin, linked statically; Mac-run shader tools via rt64 patch
0004). Not yet run: that needs a Windows PC with a GPU. In the Windows build:
- phone controllers and the Roster Room (Winsock, `src/main/net_compat.h`; the Roster Room opens in the default
  browser);
- weekly roster downloads (WinHTTP, `platform_windows.cpp`);
- screenshots (F12 or a controller's capture button: the window's area to Pictures\BlitzRecomp);
- Switch to Classic/Modern when both games are in the one folder (`make win`).

Clips point to the Xbox Game Bar (Windows key + Alt + R). A crash log has addresses but no function names.

**Graphics: Vulkan.** The Mac's DXC can't sign DXIL (no validator), and D3D12 refuses unsigned shaders, so a
cross-built Windows app treats Auto as Vulkan (frontend patch 0013). Picking D3D12 by hand won't draw until the
shaders are signed (Windows' own dxc.exe and dxil.dll, e.g. under Wine, at build time). DXC itself still loads at
startup and needs the Visual C++ runtime, which ships beside the .exe (`tools/windows/fetch_vc_runtime.sh`, Microsoft's
redistributable package from the same Visual Studio manifest xwin uses). The .exe carries a manifest making UTF-8 its
code page (paths with any characters) and SDL makes it per-monitor DPI aware.

The audit below is the original note.

## First run on a real PC (2026-10-22): parked here

The team's desktop: AMD Radeon R9 380, Vulkan, Windows x64 (Classic from `make win`'s BlitzRecomp.zip, downloaded over the
LAN).
- **Works:** start-up, the launcher, Load ROM, menus, team select with uniforms, kickoff and play; saving (a little
  slower than on the Mac or iPhone, likely that PC's disk: saves are flushed to disk before replacing the old file).
- **Crash:** an access violation (0xC0000005 in the .exe) right after the window was resized or maximized
  (`[window] size changed (1280, 720)`), as play went back to the menus (`loaded ovl_b`). The log then had only a raw
  address. Since 1eb48fa a crash logs each stack frame as module+offset and writes `dumps\crash-<time>.dmp`.
  Symbolize with `llvm-symbolizer --obj="build/win_classic/Classic Blitz.exe" 0x140000000+<offset>` (the PDB beside it)
  from the same build. Next: reproduce with that build and read the stack.
  To reach it without anyone dragging the window: start the game with `BLITZ_RESIZE_STRESS=3` set (PowerShell:
  `$env:BLITZ_RESIZE_STRESS=3; & ".\Classic Blitz.exe"`), which flips the window between 1600x900 and 960x540 every
  3 seconds; play a down and let it go back to the menus. Then send `blitz.log` and the newest `dumps\crash-*.dmp`.
  The same stress on the Mac (Metal, 2026-10-28) survived 108 resizes over the attract demo's play-to-menus loads
  (ovl_a to ovl_b), so the crash is likely in the Vulkan path or Windows' swapchain handling.
- **Not yet seen:** Modern's log (`%LOCALAPPDATA%\Blitz2026Recompiled\blitz.log`).
- **Wanted (team):**
  - **Mouse in our own screens** (launcher, Settings, the in-game menu): the frontend ignores mouse movement while a
    controller is the active input (`ui_state.cpp`, `get_cont_active()`); only a click brings the cursor back.
  - **Arrow keys on play select:** on the keyboard the arrows are the C-buttons (WASD is the stick, IJKL the D-pad).
    First check which inputs the game's play select reads, then make the arrows page the plays without losing a
    play-picking key.
  - Madden-style keyboard as the fallback; clicking plays on the game's own play select is a bigger, later job.

## Testing on a Windows PC

1. On the Mac: `make win` → `build/win/BlitzRecomp.zip` (BlitzRecomp\Classic and BlitzRecomp\Modern, the app
   files only). The C runtime is linked in: no Visual C++ Redistributable needed.
2. On the PC (Windows 10/11 x64, current GPU drivers): unzip anywhere and run `Classic\Classic Blitz.exe` or
   `Modern\Modern Blitz.exe`. Windows SmartScreen will warn about an unsigned app: More info → Run anyway. The first
   phone-controller or Roster Room use brings up Windows Firewall's prompt: allow private networks.
3. Load ROM in the launcher (the USA v1.1 `.z64`, or a zip holding it). Settings and saves go to
   `%LOCALAPPDATA%\<program id>`, with `blitz.log` there for anything that goes wrong.
4. Graphics: Auto is Vulkan in this build (see above). If the window stays black or the app closes, send
   `blitz.log` (all of stdout and stderr is in it, plus crash notes).

**Exploratory note — October 21, 2026. Suggestive, not a committed plan.** Scope: a native Windows x64 build of Classic and Modern Blitz using RT64's Vulkan renderer, with a path toward the current app's features. This is a source/build audit, not a successful Windows build. No game code, dependency code, build settings or running applications were changed for this note.

## Feasibility and likely effort

**This looks like a practical platform port.** Windows support already exists in the runtime, frontend and renderer. The largest body of work is moving Blitz's Mac-specific application services behind platform boundaries, correcting the root build/staging rules, and validating behavior on Windows. The original gameplay, engine32 teams, roster/art logic and season implementations should mostly stay shared.

For one engineer familiar with this repo, with a Windows x64 machine and working GPU drivers available:

| Milestone | Indicative effort from starting the port | What it would establish |
| --- | --- | --- |
| Configure, compile, show the launcher through Vulkan, import a ROM | **3–7 engineering days** | Build/toolchain, renderer, resources and basic platform services work together. |
| Playable Classic + Modern, ordinary saves, controllers and local content | **1–2 weeks total** | Useful Windows build with a clearly identified set of unfinished extras. |
| Dependable package approaching current app feature parity | **3–5 weeks total** | Save states/Redo, HTTP rosters, phone controllers/Roster Room, process switching, diagnostics and clean-machine validation. |
| Screenshot/clip parity if substantial new capture work is needed | Potentially **another week**, or part of the range above | A capture backend with acceptable quality, audio and lifecycle behavior. |

These are planning ranges, not dates or measured completion estimates. Initial compiler/linker failures, save-state behavior and access to test GPUs could change them. A first playable build and a feature-complete Windows release are different acceptance points. A PS5 port would remain separate work; this would provide a useful shared Vulkan baseline.

An appealing route would be: preserve the current Mac behavior; add a Windows x64 configure/build/staging path; boot Classic with Vulkan explicitly selected; then enable Modern and restore platform-dependent extras. A small set of shared service interfaces seems more maintainable than scattering Windows alternatives through gameplay files.

## Inspected baseline and what is reusable

Root checkout: **`8e49d16c548e38ce6b85d56dea6738335ad44260`**. Dependencies at inspection:

| Dependency | Checked-out commit | Existing support relevant to the port |
| --- | --- | --- |
| `lib/N64ModernRuntime` | `ca568b6ad79b9029d14077f0c3ffa757727c5559` | Windows threads/task paths, `VirtualAlloc`/`VirtualProtect` guest memory, Win32 window-handle contract. |
| `lib/RecompFrontend` | `d0d90ba49f46f4896aaeda362056c21b1e342561` | Windows dialogs/app-data lookup, graphics API setting, SDL input and shared UI renderer. |
| `lib/rt64` | `6f1c2d99a4ea571c139f449c326fd176ba8f3496` | Windows/Vulkan and D3D12 backends, Windows shader tools/dependencies. |
| Nested `lib/rt64/src/contrib/plume` | `d890ac899e505fb30040e037a4037cdeca68f033` | Vulkan Win32 surface/swapchain, GPU allocation and command infrastructure. |
| Standalone `lib/N64Recomp` | `2b6f05688de2abc7d86da5b4a89b84c2c6acbabe` | Generates portable C and RSP C++ from the same ROM/symbol inputs. |

**The first three submodule working trees and nested Plume contain local changes.** Preserve those changes. The patch directories currently hold **5 runtime, 11 frontend, 3 RT64 and 1 Plume patches**. [`tools/apply_patches.sh`](../tools/apply_patches.sh) is a Bash script that checks/applies them and uses temporary Git worktrees. A Windows build needs a deliberate way to reproduce this state; a clean upstream checkout alone is not the current Blitz app. Updating all dependencies is unnecessary scope for an initial port.

Strong reuse candidates include:

- [`engine32.cpp`](../src/game/engine32.cpp), the team store, content packs, uniform/art registration and the existing cart exporter logic. Windows changes their host services and staging, not the game's 32-team architecture.
- [`audio.cpp`](../src/main/audio.cpp): already SDL device/stream based, with resampling and output-device recovery. It needs Windows device testing rather than an entirely new mixer.
- [`controllers.cpp`](../src/main/controllers.cpp) and the patched frontend input layer: SDL mapping, controller assignments, glyphs and rumble are mostly common. Windows device enumeration and reconnect behavior still need testing.
- Runtime allocation/thread/mod-code protection implementations already have `_WIN32` paths; the Mac `ld64` writable-text wrapper should not be carried into the Windows link.
- Shared RmlUi/frontend UI, shaders and native file-dialog dependency. A new Windows UI framework would add work without helping this port.

Upstream [BanjoRecomp's Windows build guide](https://github.com/BanjoRecomp/BanjoRecomp/blob/main/BUILDING.md) is useful toolchain precedent for this family of projects. Its game-specific generation instructions are not Blitz instructions, and it does not prove this modified checkout builds unchanged.

## 1. Build system and dependency work

[`CMakeLists.txt`](../CMakeLists.txt) is currently an Apple Silicon application build, despite the portability of its dependencies. It unconditionally enables Objective-C/Objective-C++, selects arm64/macOS deployment settings, adds `macos_support.mm`, passes the custom Mac linker, includes Apple packaging and stages resources into `.app/Contents/Resources`. Root shader-tool variables also explicitly select Mac executables and `DYLD_LIBRARY_PATH`.

| Current obstacle | Location/evidence | Possible adjustment |
| --- | --- | --- |
| Objective-C language enablement on every platform | Root `project(... C CXX OBJC OBJCXX)` | Start the root project with C/C++; enable Objective-C and its source only for Apple. |
| Global Mac compiler/linker assumptions | Root `-include cstdlib`, `-fno-strict-aliasing`, `-fms-extensions`, `-fuse-ld=.../platform/macos/ld64` | Keep meaningful options per compiler/target; gate the linker and Apple workarounds. Check generated-C flags with the chosen Clang frontend. |
| Homebrew / `.dylib` dependency discovery | Root `BLITZ_DEPS_PREFIX`, SDL/FreeType overrides | Give Windows its own dependency prefix and consistent x64 ABI/CRT. Keep the existing Mac prefix intact. |
| Apple-only resource expressions | Root `BLITZ_BUNDLED_PACK_COMMAND`; [`apple_bundle.cmake`](../platform/macos/apple_bundle.cmake) | Separate a shared resource manifest from platform-specific destinations; bundle expressions belong only in Apple staging. |
| Root shader tools overwrite dependency defaults with Mac paths | Root `DXC` / `SPIRVCROSS` before adding RecompFrontend | Choose tools for the build host and produce SPIR-V for the target. RT64 already selects `dxc.exe` on Windows. |
| No Windows build/staging entry point | [`Makefile`](../Makefile), `tools/build_deps.sh`, `tools/check_env.sh`, `tools/make_bundle.sh` | Add a Windows CMake preset plus a small native build/staging script, or equivalent documented commands. Existing `make app-modern` includes Mac signing/copy steps. |
| Windows executable starter is also Mac code | [`src/starter/starter.c`](../src/starter/starter.c) | Build a single engine directly first; add a Windows starter when the dual-engine layout is settled. |

A reasonable first toolchain would be **Clang/clang-cl with the MSVC ABI and Windows SDK, CMake/Ninja, and one consistent C++20 library/CRT setup**. The dependencies already contain Clang-with-MSVC-frontend handling. Selecting a specific version is an implementation choice to pin after a configure/build experiment; MinGW and Windows ARM would be additional configurations to validate.

There is a concrete SDL integration issue to resolve: RT64's Windows CMake points at **`src/contrib/mupen64plus-win32-deps/SDL2-2.26.3`**, while the root uses `find_package(SDL2)`/`SDL2::SDL2`, and both frontend libraries refer to **`${sdl2_SOURCE_DIR}/include`** on Windows. That variable is not established by the root's package lookup. Pick one SDL2 installation/version for the whole build; verify include paths, import libraries, runtime DLL and `SDL_MAIN_HANDLED`/entry-point behavior agree. FreeType discovery and any transitive runtime libraries need the same treatment.

The pinned RT64 tree already contains Windows **`dxc.exe`, `dxcompiler.dll`, `dxil.dll`, `dxcompiler.lib`**, and its bundled SDL2 x64 import library. They are useful starting inputs, not evidence that every binary needed by a staged Windows app has been collected.

### Code generation can be separated from the first Windows build

The useful generated inputs already exist locally:

- Classic: `build/RecompiledFuncs/`.
- Modern original-game engine: `build/engine32/RecompiledFuncs/`, plus `engine32_sites.inc` in that generated tree's parent.
- Save-state-transformed variants: `build/savestate/RecompiledFuncs/` and `build/engine32/savestate/RecompiledFuncs/`.
- RSP/audio: `build/rsp/n_aspMain.cpp`.
- Existing committed ROM-picker/cart byte arrays can be compiled without rebuilding their MIPS tools on the first Windows app attempt.

For an initial experiment, generating these privately on the current Mac and compiling the resulting C/C++ on Windows could isolate the platform port from the build-tool port. Record the ROM/config/tool hashes and keep the generated files outside public source/artifacts, following the project's existing handling. The same generation options must feed both platforms.

For reproducibility afterward, adapt generation scripts for Python's Windows environment paths, `.exe` tool names, directory handling and shell invocations. The existing `.venv/bin/python`, Unix Make recipes and Bash patch application are not native PowerShell commands. A host-generation stage and native Windows CMake stage are also a viable long-term split. None of these steps requires translating the game afresh into hand-written C++.

## 2. Bringing up Vulkan explicitly

The necessary renderer layers are already present:

1. [`ultramodern/renderer_context.hpp`](../lib/N64ModernRuntime/ultramodern/include/ultramodern/renderer_context.hpp) defines the Windows handle as **`{ HWND window; DWORD thread_id; }`**. The Mac equivalent is a Cocoa window plus Metal view.
2. [`main.cpp::create_window()`](../src/main/main.cpp) currently requests `SDL_WINDOW_METAL` and calls `SDL_Metal_CreateView()` unconditionally. A Windows path can create an SDL window and obtain **`SDL_SysWMinfo.info.win.window`**, passing the correct window-owning thread ID into the existing runtime contract. [SDL window information](https://wiki.libsdl.org/SDL2/SDL_SysWMinfo)
3. [`rt64_render_context.cpp`](../lib/RecompFrontend/recompui/src/renderer/rt64_render_context.cpp) already maps that handle into RT64 and maps `GraphicsApi::Vulkan` into its user configuration.
4. [`plume_vulkan.cpp`](../lib/rt64/src/contrib/plume/plume_vulkan.cpp) already supports the Win32 surface extension. Its instance requests **Vulkan 1.2**; usable device features, formats and driver behavior also matter. Vulkan 1.2 by itself is not a complete minimum-hardware specification.

The relevant Windows path is **SDL → HWND → RT64/Plume Win32 Vulkan surface**. The existing `RT64_SDL_WINDOW_VULKAN` option is explicitly for platforms **outside Windows**; Plume's SDL/Vulkan integration is Linux-dependent in this pin. Blindly enabling that option does not supply a Windows window adapter. Let the existing Win32 surface code own surface creation rather than creating another SDL Vulkan surface with competing ownership.

**Explicit Vulkan selection is part of acceptance.** [`rt64_user_configuration.cpp::resolveGraphicsAPI()`](../lib/rt64/src/common/rt64_user_configuration.cpp) selects **D3D12 for Auto on native Windows**, Vulkan when Wine is detected, Metal on Apple. RT64 also has automatic fallback behavior. For this work, select Vulkan through the existing graphics configuration before renderer creation, persist the choice appropriately, and record the **actual chosen API, device and driver** in the log/debug report. A working D3D12 window would not validate this milestone. The existing frontend setting can remain the normal user control; a dev override could help a fresh-install test if needed.

Choosing Vulkan at runtime does not currently eliminate D3D dependencies at build/package time. On Windows, RT64 builds D3D12-related code and DXIL shaders, links D3D12/DXGI/DXC libraries, and **`rt64_dynamic_libraries.cpp` marks `dxcompiler.dll` and `dxil.dll` required** before graphics initialization. It would be simpler to keep that proven dependency layout for the first port than to add a Vulkan-only compile configuration simultaneously. Bundle both DLLs beside the executable, along with SDL and any other required non-system libraries. The Vulkan SDK is useful for developer diagnostics; users need a usable Vulkan loader/driver, not the whole SDK. [Khronos loader overview](https://docs.vulkan.org/guide/latest/loader.html)

One visible error-path issue deserves a focused Windows check: `RT64::Application::setup()` logs a failed `DynamicLibraries::load()` and currently returns **`SetupResult::Success`**. That could lead to later code treating an incomplete setup as valid. This is a source finding, not a reproduced Windows crash. Missing-DLL tests should produce a clear startup failure; any correction would belong in a recorded dependency patch, not an unrelated gameplay change.

Initial graphics acceptance could cover the launcher UI/fonts first, then title/attract/team select, both VS helmets, the field and effects, resizing/fullscreen/Alt-Tab, and exit/restart. Run developer validation on a real Windows GPU and normal packaged builds without developer tooling. The existing aspect-ratio, new-target clear and scheduling patches remain relevant; review their Windows behavior rather than dropping them as “Mac changes.”

## 3. Application services: concrete file map

The following files prevent a straightforward native Windows compile or assume Mac behavior after compiling. Some can receive small adapters; others warrant a bounded follow-up feature.

| Area | Files / current coupling | Suggested Windows direction |
| --- | --- | --- |
| Entry, window, restart | [`main.cpp`](../src/main/main.cpp): `<mach-o/dyld.h>`, `<unistd.h>`, `_NSGetExecutablePath`, `setenv`, `execv`, Cocoa/Metal window | Windows window adapter; executable-path/process helper; keep SDL event/game logic shared. Apple accent/menu setup can remain Apple-only. |
| System integration / live HTTP | [`macos_support.mm`](../src/main/macos_support.mm) implements `disable_press_and_hold`, `install_app_menu`, `window_number`, **`http_get` via NSURLSession** | Separate platform functions. HTTP is a real Modern dependency, not just a Mac menu stub. WinHTTP or a shared HTTP library are possible replacements. |
| Classic/Modern switch and ROM handover | [`experience.cpp`](../src/main/experience.cpp), [`starter.c`](../src/starter/starter.c): bundle-relative sibling paths, POSIX spawn/wait/exec and environment | Windows process launch/wait/exit with explicit executable/resource/data paths; one-engine packages can precede this. Preserve distinct Classic/Modern state and ROM identity. |
| Config/debug info | [`config.cpp`](../src/game/config.cpp): unconditional `<sys/sysctl.h>`, `sysctlbyname`, Mac OS/model strings | Platform-neutral debug-info collector with Windows OS/CPU and actual GPU/API information. Gameplay settings remain common. |
| Logging / crash diagnostics | [`app_log.cpp`](../src/main/app_log.cpp): POSIX pipe/dup/read/write, `O_CLOEXEC`, `execinfo.h`, signal backtraces | Windows/CRT logging backend and later Windows exception/minidump integration. Preserve logs before graphics starts and flush/drain on exit. |
| Save durability and paths | [`controller_pak.cpp`](../src/game/controller_pak.cpp), [`adv_save.cpp`](../src/game/adv_save.cpp), [`savestate.cpp`](../src/game/savestate.cpp): `fsync`, POSIX descriptors, some narrow-path I/O | Shared binary file/sync/replace service with Windows implementation, error propagation and repeat-write tests. Keep existing guest/pak and explicit save formats. |
| Phone controllers | [`phone_pads.cpp`](../src/main/phone_pads.cpp): BSD sockets, `SO_NOSIGPIPE`, `fcntl/FD_CLOEXEC`, `getifaddrs`, Mac `en*` preference | Winsock initialization, `SOCKET`/`INVALID_SOCKET`, `closesocket`, proper error/shutdown/timeouts and Windows adapter enumeration. Preserve HTTP/WebSocket/QR behavior. |
| Roster Room | [`roster_room.cpp`](../src/game/roster_room.cpp): POSIX HTTP listener and `open` browser command | Reuse the same Windows socket/browser helpers; retain its current loopback exposure. |
| Export / open folders / Rules / Add-Ons | [`export_rom.cpp`](../src/game/export_rom.cpp), [`house_rules.cpp`](../src/game/house_rules.cpp), [`addons.cpp`](../src/game/addons.cpp): `HOME/Downloads`, shell `open` / `open -R` | Windows known Downloads folder, browser/folder/file-reveal helper and the existing native dialog path. Honor `BLITZ_EXPORT_DIR`; keep ROM patching/CRC logic common. |
| Screenshots/clips and hotkeys | [`capture.cpp`](../src/main/capture.cpp): Cocoa window IDs, `/usr/sbin/screencapture`, POSIX spawn/kill | Separate shared input actions from capture backend. Initial disabled capture can be explicit; a Windows capture implementation is a separate task. |

### Services worth sharing rather than reimplementing at each call site

A small interface family could cover executable/resource/data directories, durable file writes, opening a URL/path/revealing a file, starting a child process/restarting, HTTP GET, socket lifecycle/adapter enumeration, diagnostics, and capture. Names and directory organization are implementation choices. Mac implementations can keep their current behavior while Windows implements the same contract; future Linux/PS5 work could reuse the boundary.

Some details matter beyond changing `#include`s:

- **Windows native filesystem paths are wide strings.** `::open(tmp.c_str(), ...)` in Advanced saves/save states and `app_log.cpp` does not port directly; `fopen(path.string().c_str())` can lose Unicode filenames. Keep `std::filesystem::path` at filesystem boundaries, use native wide-path APIs/overloads where appropriate, and format log/UI text as UTF-8 deliberately. Do not solve compilation with a pointer cast. Test names with spaces and non-ASCII characters.
- **Preserve binary and replacement semantics.** ROMs, paks, raw audio and states must not acquire text-mode newline conversion. `fsync` needs a suitable Windows equivalent. Existing `std::filesystem::rename` should be tested for its required replace-existing behavior, open-handle sharing and errors, rather than assumed broken merely because it is Windows. A helper may use platform primitives such as [ReplaceFileW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-replacefilew) for an existing destination and handle the first-save case separately. Repeated writes, not just a first save, are the important acceptance test.
- **Winsock is not a file-descriptor shim.** It needs [process initialization](https://learn.microsoft.com/en-us/windows/win32/api/winsock/nf-winsock-wsastartup); sockets have their own type, close/error functions and timeout conventions. Avoid narrowing a 64-bit `SOCKET` into the current `int` fields. LAN phone binding and loopback Roster Room binding should preserve their different intended reachability. Verify discovery/QR on Wi-Fi, reconnect and listener release on restart.
- **HTTP downloads are already asynchronous at the app level.** `rosters_live.cpp` calls `http_get()` from background work, with headers/token/status and cached/local-folder fallback. A Windows backend can preserve that contract; it should preserve timeouts, TLS/status/error handling and avoid credentials in logs. [WinHTTP](https://learn.microsoft.com/en-us/windows/win32/api/winhttp/nf-winhttp-winhttpopen) is one option. Offline/local roster fixtures can support the first Modern milestone without requiring a network account.
- **Removing `capture.cpp` wholesale would also remove F5/F6/F7/F8 actions**: it contains save/load, slot selection and Redo Play event handling as well as F12 capture. Keep those shared controls even if capture is deferred. Controller capture-button handling should accurately reflect feature availability.
- **Windows restart starts a new process.** Reproduce the current flush/save/stop order before launching and exiting, with correct argument quoting and handle inheritance. Test a path with spaces, an import handed to the other engine, repeat restart, and a running phone listener; the old process should not keep the port or a save file open.

## 4. Resources, configuration and package layout

The root build currently creates Mac bundles, and the frontend's `get_program_path()` returns **an empty path on ordinary Windows builds**, unless `BLITZ_RESOURCES` is set. That makes resource lookup depend on the launch working directory. A Windows implementation should resolve resources independently of that directory; [SDL_GetBasePath](https://wiki.libsdl.org/SDL2/SDL_GetBasePath) is a possible common helper.

`get_app_folder_path()` already uses **`FOLDERID_LocalAppData / program_id`** on Windows, so there is a sound starting point for separate `BlitzRecompiled` and `Blitz2026Recompiled` data. However, **`APP_FOLDER_PATH` is checked only in the Linux/Apple branch**, and `portable.txt` is checked in the current working directory. Test overrides and portable behavior deliberately; the README's “next to the app” behavior should also hold when launched from a shortcut or terminal elsewhere.

A simple initial staged folder could contain one engine executable alongside:

```text
ModernBlitz.exe
SDL2.dll
dxcompiler.dll
dxil.dll
assets/                     UI resources and fonts
recompcontrollerdb.txt
packs/                      available bundled packs
houston/                    Modern's generated independent team data/art
art/                        title/art overrides, VS-right images, credits as applicable
uniforms/
announcer/                  if supplied for that build
```

This is an illustrative inventory, not a final manifest. Include any discovered transitive DLLs, runtime requirements and third-party notices. Initial Classic and Modern folders can be separate; merging them behind a Windows starter should follow a defined executable/resource layout. All required runtime DLLs must be in locations the existing loader actually uses for each executable.

The content copy should be driven by the same inventory as the Mac build: `assets/`, fonts, controller DB, optional roster/league packs, Houston, team art, uniforms and announcer files. `make app-modern` currently adds much of this **after** CMake's build step from `build/appdata2026`; compiling the `.exe` would not automatically include it. A clean checkout also lacks locally generated personal art. Inventory and stage available content explicitly, and report meaningful fallbacks when optional content is missing. No need to regenerate finished images or depend on Mac font paths just to package Windows.

For a combined package, [`experience.cpp::modern_original()`](../src/main/experience.cpp) also reads the bundle's **`engine-original` marker**. Account for that engine identity check in the new layout instead of treating the starter as just a renamed `.exe`. Both current default engines use the plain USA v1.1 ROM; the optional Team Extension flavor remains a separate target.

## 5. Saves, runtime compatibility and feature scope

**Ordinary saves and save states need different treatment.** The standard Controller Pak is a 32 KiB guest image; Advanced sidecars use explicit fields/CRC/version encoding; Party tournaments store the game's bracket image. These are good candidates for cross-platform copy tests using separate test folders. Their filesystem writing still needs the Windows backend above.

Save states in [`savestate.cpp`](../src/game/savestate.cpp) are currently **file version 3**, writing raw `ThreadImage`/context/POD data and registered module blobs, with a host context address used to rebase pointers. `build_id()` hashes site-table entries/count and flavor; it does **not** encode the host OS/ABI, compiler packing or all serialized type sizes. A Mac state and a Windows state could therefore share the site hash without being proven layout-compatible. Both machines being 64-bit little-endian is insufficient evidence.

For a first port, same-build Windows state round trips are a useful requirement. Cross-platform state import should be explicitly accepted only after a layout/format audit, or refused using an appropriate architecture/ABI/format discriminator. A portable explicit state format would be additional work. Redo Play and Save & Quit depend on this same machinery, and restoring threads/message queues needs Windows testing with the repo's race/counter patches retained.

An initial feature boundary could look like this:

| First playable milestone | Follow-up before claiming current-app parity |
| --- | --- |
| Vulkan launcher/game, ROM import, keyboard/gamepad, SDL audio, core Modern 32-team/art/roster data, ordinary season/pak saves and Export ROM | Windows restart/switch/handover, live roster HTTP, phone controllers, Roster Room, same-build save states/Redo/Save & Quit, full diagnostics, screenshots/clips |

This is sequencing, not a recommendation to remove features from the eventual Windows version. Stubbed services would need clear availability handling and all referenced functions still defined. Several platform-dependent files are compiled for Classic too; selecting Classic or disabling an in-game toggle does not eliminate their compile-time dependencies. A small number of explicit build capabilities would be clearer than silent no-op features.

## 6. Suggested sequence and evidence to collect

These are possible bounded milestones, not approved implementation steps:

1. **Capture the working inputs.** Record root/submodule commits, applied patch state, generated source provenance, resource inventory and a Mac reference run. Use separate Windows build/staging/data directories. Avoid upgrading the libraries while establishing the first baseline.
2. **Make Windows configure/build/staging possible.** Platform-gate Apple settings and sources; resolve one SDL2/FreeType/CRT toolchain; select Windows shader tools; provide the minimum platform service implementations and clear optional stubs. Save states may start OFF to isolate basic runtime bring-up, while remaining a later acceptance item. Add a repeatable preset rather than requiring IDE-only state.
3. **Boot a packaged Classic app in explicit Vulkan.** Verify UI fonts, native ROM dialog/import, keyboard/controller and sound; then play a game. Log actual API/device/driver and all startup failures. Confirm a fresh run without a Mac-generated settings file.
4. **Enable Modern with a complete resource manifest.** Check Houston/all 32 teams, ordering, both VS orientations, uniforms and skin tones, Standard/Advanced season progression, roster choices, league pak behavior and Export ROM. Port ordinary save I/O before relying on a long season run.
5. **Restore the remaining host services.** Introduce HTTP, local/LAN sockets, shell/process helpers and state persistence in small pieces. Test restart/switch and save-state thread restoration under stress before introducing capture complexity.
6. **Validate and stage for another PC.** Test a folder with spaces/non-ASCII characters, different working directory, normal/portable data, missing optional resources, no Vulkan SDK on the runtime machine, and available AMD/NVIDIA/Intel GPUs. Re-run the relevant Mac behavior after shared changes. Finish a manifest of known Windows omissions and measured minimum requirements.

### Focused acceptance checks

| Risk | Evidence that would resolve it |
| --- | --- |
| “Vulkan build” actually ran D3D12 or inherited Metal config | Log/report `get_chosen_api() == Vulkan`, GPU/driver, persisted Vulkan selection; test fresh config, explicit selection and unsupported-API settings. |
| Missing runtime DLLs appear only away from the developer PC | Launch the staged folder without developer PATH/Vulkan SDK; remove each required dependency in a disposable copy and verify a clear error. In particular test the RT64 setup error path noted above. |
| Resources depend on working directory | Launch by double-click, shortcut, and terminal from another directory; verify fonts/controller DB and all Modern content load. Test `BLITZ_RESOURCES`, data overrides and portable mode. |
| Render/synchronization differences | Compare representative scenes with Mac references: HUD/aspect ratio, team selection/VS, transparency, end zones, smoke/fire, replays, halftime, fullscreen, resize, Alt-Tab, monitor changes and long play. Use validation messages to investigate hazards rather than requiring byte-identical screenshots across APIs. |
| Audio or controller backend differs | Four-port assignments, USB/Bluetooth reconnect, Xbox/DualSense glyphs, rumble, background focus; 44.1/48 kHz devices, audio output changes and a long game without drift. The existing `BLITZ_AUDIO_DROP_TEST` can exercise recovery. |
| Saves work once but fail on overwrite/restore | Repeated season/pak/bracket/state writes; close/relaunch/reload; interrupted/error writes leave the prior valid save; Unicode paths. Keep test copies of original Mac saves and compare the formats intended to be portable. |
| Save-state ABI or thread restoration is wrong | Same Windows build save/load after kickoff, during a play, at halftime and after repeated Redo; Save & Quit/restart. Explicitly test rejection or validated import of states from a different platform/build. |
| Network/process lifetime leaks | Phones reconnect after sleep, browser Roster Room edits persist, cached/offline roster startup works, repeated restart/switch releases listeners and logs, ROM handover exits with the correct result. |
| Exported ROM differs unexpectedly | Compare ROM bytes/table ranges against a matching Mac export using the same settings/assets/rosters; account for filename timestamps separately. Use fixed local inputs so a live roster update cannot change the comparison. |
| Shared changes regress macOS | Relevant Classic/Modern gameplay and launcher/save tests on Mac, with existing Apple window/menu/capture behavior retained. |

[`tools/regress/engine32.sh`](../tools/regress/engine32.sh) already describes useful data checks: moved team tables, rosters, kickoff, deterministic exports and cart season comparisons. Its current launcher/build automation assumes Mac `.app` paths and opens windows. Port or reuse its extraction/comparison pieces rather than running it unchanged on Windows. Synthetic host tests can run in ordinary CI; private ROM-derived fixtures/generated sources and a real Windows GPU session are separate inputs. A cross-compile or a VM-only launch is not sufficient renderer/performance acceptance.

## Likely file boundaries for a later implementation

The natural change set would span root CMake/build/staging, a new Windows platform source area, `main.cpp`, `experience.cpp`/starter, capture/input actions, logging, `config.cpp`, durable file-I/O callers, socket/HTTP callers, and the frontend path helper. Dependency adjustments, if necessary, should be small recorded patches against the existing pins. No evidence from this audit suggests rewriting engine32, team selection rules, rosters, season algorithms or N64 recompilation as the foundation of the Windows port.

A desktop Windows/Vulkan build would give future PS5 work a tested x64 game/runtime and shader baseline, portable resource/services boundaries and desktop debugging. PS5-specific toolchain, memory, presentation, packaging and driver compatibility would still need separate validation; the Windows executable is not the PS5 artifact.

## Handoff note

The most informative next artifact would be **a staged Windows x64 Classic executable that logs Vulkan as the actual renderer and reaches a played game**, followed by the same target with Modern and a content inventory. Keep the current Mac build intact, preserve the local dependency patches, and treat durable saves/resource paths as early port requirements. Once that exists, revise the effort estimate around observed compiler/driver/runtime issues, then restore the remaining current-app services. This note authorizes no implementation and commits to no release scope.
