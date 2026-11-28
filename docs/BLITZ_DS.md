# BLITZ DS

The game on two screens, built 2026-10-31 on branch `blitz-ds`. One Android build for every device: it looks for a second
screen at startup and uses it. It has run on an RG DS Plus and, as a stacked test, a Retroid Pocket 5; no device has shown it
meets the bar yet. See "Tested so far".

## What it does

| Moment | Lower screen (first window) | Upper screen (second window) |
|---|---|---|
| Live play | The lower share of the 2:3 tall picture | The upper share. The hinge hides nothing |
| Play select | The game's own tiles, tappable | The last play looping, beside the score, what's next and the drive |
| The fly-in, cut scenes, kickoffs | The game, whole | The score and the drive |
| Menus | The game, whole | The Blitz '27 logo and BLITZ DS |

- **Camera:** the following camera (`src/game/tall_view.cpp`): low and close at the line, higher and wider while the
  quarterback reads, low and close on the ball and a carrier. It widens, up to its limit, to keep any player a controller
  is on in frame, so a defender doesn't get lost.
- **Taps:** a tile walks the game's own cursor there and presses A (`src/game/ds_view.cpp`). The cursor is read from its
  brackets (`BRCKTTOP.WMS`) as they're drawn, since its place in memory isn't stable. If the help page is up, the tap
  presses A once and waits for the grid. Only player 1's half responds. The tile header turns the page. During a live
  play, a receiver's icon on either screen throws to him.
- **Stylus (Settings → DS → DS Screens):** the game on the second window and big tiles on the first, for the right hand. Receiver
  Buttons stays off by default, and Stylus has it on for player 1 whatever the setting says: the fan's three wedges, each wearing
  its button's icon, throw with those buttons (Jump and Pass sit under it; two-tile moments stack them, narrow, with room on the
  right), and the icons over the receivers, on the game's window, say who's who. On one screen a saved Stylus is
  nothing, and the icons stay off.
- **Settings:** the SETTINGS button at the second screen's bottom right, since a handheld may have no key for it.
- **Instant Replay (docs/REPLAY.md):** the REPLAY button beside it, or a tap on the looping last play: the play full size across both
  screens, with the game stopped.
- **A tap shows it arrived:** a ring where the finger came down, white, yellow if it did something, orange if refused
  (`ds_touch.cpp`). It's drawn by the renderer, so in a stall it waits for the next present and then gets its full 350 ms. `[touch raw]` log lines give every finger's position and how far it went, whatever became of it. A tap on
  the opponent's half picks nothing: alone in the game, you're told which side your plays are on, unless it's a finger resting
  on the right edge (x 0.86 and over, with the right half the opponent's).
- **An upper screen with no touch (an RG DS's):** `BLITZ_ONE_TOUCH`, set in Potato Mode or with fewer than two touch panels. At
  play select in a one-player game, FLIP (the plays, as Jump does there) and SETTINGS come to the lower screen, in the header of
  the half that isn't player 1's, and REPLAY between them once there's a replay to open (the small one, 28 MB, from 800 MB of RAM: an RG DS has room since 2026-11-02). R1 at play select opens the replay too, on any controller. The ring a tap used to leave is off
  (`BLITZ_TAP_RINGS=1` brings it back).
- **On-screen touch controls** stay hidden while two screens are on, if a controller is connected. Without one (a Surface
  Duo) they stay, and tile taps give way to them.
- **Drive panel:** the score on the house's bevelled plate, with the team with the ball in yellow; the next down, big,
  between the game's cursor brackets; a strip of the field showing where the drive began (the bar), the ball (white) and the
  line to gain (orange); plays, yards and time; and every play with its gain. It's Blitz '27's own art
  (`header_bevel`, `panel_frame`, `selector_cap_*`, `rule_chrome`). `BLITZ_DS_DEMO=1` fills it with a sample drive.
- **Going through a long drive:** L1 back and R1 forward at play select (N64 L or C-Up, and C-Down), or a tap on the list's
  top or bottom half. It's back at the newest after each play.
- **Drive log:** every play of every drive also goes to `blitz.log` as `[drive]` lines. The score and the next snap are read
  live, so they're right at a game's first play select and after a save state. Time running backwards (a new game, a save
  state, Redo Play) starts the list over.
- **Settings → DS:** Two Screens (on by default), DS Screens (Both, Companion, Stylus, Controller), Swap Screens (Android: from the next launch), the camera presets (Field Ahead, Camera Zoom Out) and Custom Layout. **Settings → General:** Send Logs (Android).

## How it's built

- **RT64 (`platform/rt64-patches/0010`):** a second swap chain on the present thread, drawn and presented right after
  the first, each with its own submit and wait. The emulator's Android stalls when both are drawn in one frame's submits
  but runs fine in turn (the probe's result, 2026-10-31). `blitz_ds_mode`: 0 one screen, 1 the picture over both, 2 the
  picture on the first and only the hook's drawing on the second.
- **The clip:** 15 frames a second of the last 8 seconds at 256x384, kept as GPU textures (about 47 MB), drawn on the left
  of the second screen in play select. Leaving play select starts it over. Off in the low quality profile.
- **Second screen's UI (`platform/recompfrontend-patches/0020`):** its own small RmlUi context of plain RML, which
  `ds_view.cpp` writes only when it changes. The two contexts share a render manager, so each sets the viewport before
  it renders, and the UI's MSAA targets are kept per screen size.
- **Android (`BlitzActivity.java`, `platform_android.cpp`):** a `Presentation` on the first other display, with a
  `SurfaceView`. It starts unable to take focus, so controller keys keep going to the game, and takes it only if the game
  still has none about 3 s in. On GammaOS it takes focus from the start, or the launcher on the other screen keeps the keys.
  Keys that reach it are forwarded to the game. Its surface goes to native code, and `plume_android_second_window`
  (`platform/plume-patches/0005`) keeps that window from following the app's own.
- **The picture's shape** comes from both screens' sizes, so unequal screens are handled: `blitz_tall_ratio` and
  `blitz_ds_split`. The 2D layout (`blitz_tall_sprite_begin`) still halves the sprites, which only suits equal screens.

- **Which window:** a `Presentation` where Android allows one (a display flagged `FLAG_PRESENTATION`), else an activity on
  that display (`BlitzSecondScreen`), which can't take focus. `BlitzLauncher` is the launcher icon: with two screens it starts
  the game on display 0. BLITZ_DS_ACTIVITY=1 forces the activity, for tests.
- **Which screen is on top:** Android doesn't say, so `BlitzLauncher.firstIsUpper` decides at launch: a table of `Build.MODEL`,
  then, with two touch panels, the panel with more pixels, then display 0 is the lower. So a monitor with no touch never takes
  play select from the handheld. Never dpi, which is wrong on both measured devices.
  Swap Screens flips the answer and doesn't move the app. Play select and its taps go to the lower screen, the drive board to
  the upper. Menus stay on the first window, so where display 0 is the upper screen, they're up there too. Only the RG DS's
  answer has been seen on hardware, and only in earlier builds' logs: the rest is unrun.
- **Idle cost:** a panel that hasn't changed isn't redrawn (once a second, in case a compositor lost it), and a play with no
  icon on the second screen skips its UI pass. Icons move by id without reparsing. A second screen that stops taking frames
  times out after 100 ms instead of stopping the game.

## Quality

Chosen once per launch from total RAM (`ds_set_quality`), for that run. Saved settings aren't touched, and Graphics → Apply
can't undo these limits.

| Profile | RAM | Resolution | Clip | MSAA and UI MSAA | Refresh |
|---|---|---|---|---|---|
| low | under 2.5 GB | 2x | off | off | original |
| mid | under 5 GB | 2x | on | off (UI 8x) | original |
| high | 5 GB and up | as saved | on | as saved | as saved |

The two screens share the game's lines, so 1x would be half of them per screen, and the scoreboard can't be read. Auto
counts both screens' heights.

**First launch on Android, away from Potato Mode:** with no `gfx_limits.txt` it runs at 2x with no antialiasing, the RP5's
measured best, as Auto works out 11x or 12x over two unequal panels. Moving the file aside gives that same 2x. To let Auto back
in for a run that measures more, it needs a line for that run's screen count: `1 0 0 2` on one screen, `2 0 0 2` on two, with a
third number of 1 for antialiasing off. An old `0 2` only speaks for one screen, and a two-screen run ignores it. In a test log,
`[gfx] limits from earlier runs` confirms it; `[gfx] first run here` means the run was at the 2x pin, not Auto.

**During play it steps down by itself.** Under 26 fps for 15 seconds (BLITZ_DS_FPS_FLOOR=<fps>): clip off, then antialiasing
off, then Auto to 2x, then 2x to original, with 30 seconds between steps. Never back up in a run. Each step is a
`[ds] too slow` line. `gfx_limits.txt` keeps a step that held for the next run, one line per screen count and Potato Mode or
not (`<screens> <lean> <antialiasing off> <resolution>`), with no expiry. A line of two numbers is from before those keys, and
counts as one screen out of Potato Mode. In Potato Mode two screens never step, and a run that's had them doesn't for the rest of
it; elsewhere they step as above, saved under two screens.

**Potato Mode's two screens** go by Settings → Graphics → Two-Screen Sharpness, exactly: 1x (the original resolution, and the default),
1.25x, 1.5x, 1.75x or 2x. Above, 1x is called too few lines; on an RG DS Plus it's still the setup the team called almost there.
Above 1x it's also what 2x draws at if one screen runs at 2x in Potato Mode (a Retroid forced into it); at 1x, 2x stays 2x.

## Run recipes

An experiment on a Potato Mode test build, with no install and no trip to Settings (`BlitzRecipe.java`). Post a short text to
the log Worker as `res/recipe`, and start the game:

    name=run1 two=1 scale=1.5 frame_copy=0 barriers=full flat1x=0 ram_panels=0 texmode=x evict=120 evictfix=1 depthclamp=legacy

| Key | Values | What it sets, for that run |
|---|---|---|
| `name` | letters, digits, `.`, `_`, `-` | What the log and the toast call it |
| `two` | 0, 1 | Two Screens (Potato) |
| `scale` | 1, 1.25, 1.5, 1.75, 2 | Two-Screen Sharpness; on one screen too |
| `frame_copy` | 0 when needed, 1 always, 2 never | Frame Copy |
| `barriers` | narrow, full | GPU Waits |
| `flat1x` | 0, 1 | With two screens, a flat picture at the original resolution. Only draws less: render targets never shrink, so once a tall frame has drawn it frees no memory. Don't post it yet: flipping scale mid-drive hasn't been checked on the Mac, and a target last drawn at the other scale could be copied with the wrong rectangle |
| `ram_panels` | 0, 1 | The RAM pass always on panels and menus; `frame_copy` on the field |
| `texmode` | `compact`, or anything else for legacy | On a driver with few texture slots and no descriptor indexing, each frame numbers its own textures: at most 202 2D and 50 TMEM on a 256 limit, less if `maxPerStageResources` is under 305. A texture it can't number reads a clear 4x4 one. The `[vulkan] textures:` line gives the real numbers and says which ran |
| `evict` | a number | How many cached textures Potato Mode keeps before evicting early (120 unless set) |
| `evictfix` | 0, 1 | On a driver with few texture slots, a frame's textures count as used when it's recorded, so they can't be evicted before it draws. On (1) unless set; 0 is the eviction of ff61682 and earlier |
| `depthclamp` | `spec`, `legacy` | Without the driver's depth clamp feature: `spec` turns clamping off as the spec asks, `legacy` asks for it anyway, as earlier builds did. Unset, it goes with `texmode` (compact is spec) |

It's read once, by the launcher or, started another way, by the game, waiting at most 1.5 s. Nothing of it is saved, and a
recipe run doesn't step down. An unknown key is logged and skipped. No answer, or a text with a bad value or no `key=value`
shape, means the saved settings. A safe start ignores it. An empty post takes it away. A `[recipe]` line at the start of
`blitz.log` says what happened either way, a `[potato]` line what the run ended up with, and a toast shows the name once a game
is up, or `Recipe: none (no answer)` when the asking failed or timed out, so no toast at all means none is posted. Through the
launcher its answer stands for that launch however long the game takes to get going. A device out of Potato Mode never asks.

## Logs

Everything goes to `blitz.log` in the app's folder and to logcat:
- `[ds]`: screens found, surfaces, swap chains, each state change, taps, and second-screen timings every 5 s.
- `[device]`: the device, its input devices, why the app last exited (out of memory shows here), and free RAM and thermal
  state every 10 s.
- `[fps]`, `[drive]`, plus the crash reports the app already writes.
- `[early +N s]`: what the activity said before the game opened its log. Native logging needs a ready surface, focus and a
  resumed activity, so a launch that never gets there still leaves `blitz.early.log` (the last one is `blitz.early.prev.log`),
  and `[launch]` lines say which of those is missing, at 5, 15 and 30 s and then each minute.
- `[stamp]`: first on every gzipped upload: build, commit (`+` if tracked files differed from it, or a submodule from its
  patches: `tools/apply_patches.sh --check`, which the APK build runs), launch id, whether the game
  has started. Until it has, `blitz.log` is the last run's: it's labelled `STALE`, cut to 256 KB and sent as kind `run-stale-gz`.

In Potato Mode, also:
- `[stall]`: any 250 ms or more with no VI tick, game swap or first-screen present, from a watchdog on its own thread: what
  the Gfx, Present, Workload and Texture threads sat in, kernel state, evictions, uploads, compile, cache save, major faults.
- `[frames]`: every 5 s, swaps and both screens' presents over the same interval, pass times, sizes, swap chain images, barrier
  mode, highest texture drawn.
- `[threads]`, `[swaps]`, `[clocks]`, `[touch]`, `[os]`: per-thread CPU and major faults, `/proc/swaps`, clocks, raw touches per
  window, and the OS build. What this firmware won't let an app read is said once and not asked again.
- `[log]`: past the first 20 s, more than 32 KB in a second is dropped, and this line counts it.

**Safe start, Potato Mode:** a launch counts as drawn once the game has held a frame rate over one of its 5 s windows and 30
frames have really been presented (the game can swap at full rate behind a black screen). After two
launches that didn't, however they ended (a kill or force stop counts), the next runs on one screen at the original resolution
with Full GPU waits and no recipe, and moves `pipeline_cache.bin` and `gfx_limits*` to `<name>.safe1` to `.safe5`. A banner
says so. It repeats until a launch draws; Settings aren't touched, so the launch after that is the usual one. Moved files stay
moved, and aren't put back by themselves. Out of Potato Mode there's no safe start: a Retroid's `gfx_limits.txt` holds its pin.

Getting them off the device:
- **By themselves:** the last 2 MB of `blitz.log` goes up gzipped a minute after launch and every 5 minutes. A run that didn't
  close normally keeps its log and how it ended, and sends them at the next launch, retrying until it gets through. A crash
  at startup is still reported, and so is Android simply killing a backgrounded app.
- **Settings → General → Send Logs:** a zip with logs, crash reports, debug info and settings goes up, and a toast says whether it
  went. No share sheet: on a device without a touch screen nothing could close it, and the game sat stuck behind it.

**Post office:** Cloudflare Worker `blitz-logs` (`tools/blitz_logs_worker`, KV, files expire after 60 days, 20 MB each). The
addresses are in `build/blitz_logs_hub` (never committed): `UPLOAD` ships in test builds: it adds log bundles, reads and
overwrites `res/` files, including `res/recipe`, which every Potato Mode test build obeys at launch, and takes the mailbox's
queued commands. Only list, get, posting commands and `seen` need `READ`, which stays on the Mac.

    . build/blitz_logs_hub; curl -s "$READ/list"; curl -s "$READ/get/<id>" -o log.gz

Remove the upload address from any build that leaves the team's hands: delete `build/blitz_logs_hub` before building.

## The probe

`tools/android/probe/build.sh` makes `build/android/probe/BlitzProbe.apk`: no game, no ROM. It reports the screens, Vulkan
features and limits, each screen alone, the screens in turn, both in one frame, memory, CPU and storage speed, then every
touch and button. It sends the report by itself. Its native tests write a trace as they go, so a driver that hangs or kills
the process still reports where.

Read these first when it comes back:
1. `dualSrcBlend`: if it says NO, RT64 needs a blending fallback before the game can look right.
2. "screens taking turns" and "both screens": which way of drawing works.
3. Which display is which, and each one's rotation: whether Swap Screens needs a default, or a portrait screen needs turning.
4. Free RAM, for the low profile.

## GammaOS on the RG DS: input, the lid, the Mali glitch (2026-11-03)

- **Input dead after a nano launch (Control Center off).** GammaOS boots lean (`ro.gammaos.lean_boot`): ActivityManagerService
  finishes booting before anything's idle, with `mBooted` false, which leaves WindowManagerService's event dispatching off
  (`setEventDispatching(false)`). Only two things turn it on: `checkFinishBootingLocked`, once every non-sleeping display's
  resumed activity has gone idle, and `ensureBootCompleted`, after an app crash, ANR or strict-mode dialog. With the
  Control Center off, display 2 is Android's too and the idle check doesn't pass, so `DispatchEnabled` stays false: no key or
  touch reaches any app. A sleep and wake, or a lid close and open, passes it. Read from `services.jar` (dexdump), confirmed on
  the device: `mBooted=false`, `DispatchEnabled: false` at nano's menu and after launch.
  - **The fix, no root:** a see-through activity of ours (`BlitzNudge`) opens over the game 1 s after its first resume and
    closes at once; the game resuming after it reports idle and Android's boot finishes (`mBooted=true`, dispatch on). Found
    because Magisk's own prompt did the same. On GammaOS, once a process; `BLITZ_NUDGE=0` off.
  - The root way also works: `service call window 15 i32 1` (`IWindowManager.setEventDispatching(true)`, transaction 15)
    turns it on at once; the app tries it through `su` only with `BLITZ_ROOT_INPUT=1` (an unanswered Magisk prompt saves a
    deny). A Magisk boot script running that command would cover every app.
  - Tried and dropped: a throwaway `:kick` process that crashes (the low-memory killer took it before it ran); window focus
    as the sign (the window has focus while dispatch is off).
  - **The jumping out:** a sleep and wake soon after a launch made GammaOS fire its RetroArch handoff
    (`service.bootanim.nano_retroarch=1`) and force-stop the app ("Killing ... due to from pid 807"). The power-twice
    workaround can do the same if it comes too early.
- **The lid** (`SW_LID` on `/dev/input/event6`, gpio-keys): Android stays Awake; both panels go off and the activity goes to
  the background. The game now saves to the Auto slot, then holds at its dispatcher until the app's back (it played on with
  the lid shut before). Reopening brings both screens back, at 28-29 fps.
- **The one-frame junk HUD labels (Mali only).** Not the compact descriptor sets: the workload thread waits for the GPU after
  every workload (rt64 `rt64_workload_queue.cpp`), so 3 sets were already safe (0035 changed nothing). The race: a texture
  upload's copy runs on one queue and the decode / raw-TMEM draw that reads it on another, ordered only by a semaphore wait at
  COLOR_ATTACHMENT_OUTPUT, which on Mali holds back fragment work only. rt64 0036: the CPU waits for the copy
  (`BLITZ_UPLOAD_SERIAL`, on in Potato Mode); plume 0023 logs which virtual queues share a real one (`[queues]`).

## Testing

- **Mac:** `BLITZ_FLAVOUR=2026 BLITZ_APP=build/app_modern_dev BLITZ_WINDOW_SIZE=640x480 BLITZ_DS=1 tools/run_dev.sh 3600`
  opens two windows, and the mouse is the finger on both. `BLITZ_DS_SECOND=WxH` sizes the upper one differently,
  `BLITZ_DS_PROFILE=low|mid|high` forces a profile, and the live command `DSTAP <screen> <x> <y>` taps.
- **Emulator:** `adb emu multidisplay add 1 640 480 160 0` adds a real second display, and
  `adb shell input -d <display> tap x y` touches it. Game graphics are black and white and very slow there (docs/ANDROID.md).
- **Scripted launch:** `adb shell am start -n <package>/com.blitzrecomp.newblitz.BlitzActivity --es blitz_env "NAME=value\;NAME=value"`
  sets the test variables it allows (`BlitzActivity.onCreate`); `BLITZ_INPUT_LIVE=<app folder>/input.txt` then takes presses
  from a file updated with `adb push`. `BLITZ_TALL_LOG=1` adds a `[camera]` line a second.

## Tested so far

- **Mac, through Metal:** two windows, the split, clip, panel, Settings button, a tile tap from a fresh game through the help
  page, the same play picked by tap as by controller, a tile tap on defense, a receiver tap on the upper screen, the camera
  holding a defender running wide, and the low profile. Stylus with Receiver Buttons off (scripted, on a save state at the line):
  icons on the game's window, over the right men before the snap; C-left and C-right throw to them; a tap on an icon throws, and
  overrides the game's pick; a tap on the tiles' window does nothing there. Both Screens with the setting off, and a saved Stylus
  on one screen, stay off. The tiles themselves take fingers, which a Mac test doesn't have, so a finger on one hasn't run.
- **Android emulator, through Vulkan and SwiftShader:** finding the display, the Presentation, the activity fallback, Swap
  Screens as it was before 683fae1 (when it chose the launch display; `firstIsUpper` and today's Swap Screens are unrun), the
  second swap chain, touch on the second display, leaving and coming back, the automatic upload, and a killed
  run's log arriving at the next launch. Game graphics there are black and white and very slow.
- **RG DS Plus, Mali-G52, about 950 MB:** one screen at the original resolution, 26 fps on the field with a clean picture in the
  latest photo (build ff61682), and 28-30 fps on earlier runs. Two screens on earlier builds: 1x at 24-29 fps, 2x at a median of
  14.8 with stalls of several seconds. Tile taps by finger on real glass. Garbled bands, a white card and a striped icon are open.
- **RG DS Plus, GammaOS DualStack (2026-11-02, build with commit 9ffe31a):** Blitz added to `persist.gammaos.dualstack.pkgs`
  (next to DraStic) and launched from GammaOS's own menu. The app sees one display, 1024x1536, and no second one;
  `BlitzActivity.dualStack()` lays it out as two halves (the game's window in the lower, the picture's SurfaceView in the
  upper). SurfaceFlinger puts each SurfaceView on its own panel at 1024x768 (the upper one rotated 180, as that panel is
  mounted). Stylus at 1.5x: 30.0 fps, 0 hitches, the picture shown 30 a second and the lower screen 15; by the screen captures
  (the lower screen's Stylus tiles, the field on top). `screencap -d 0` returns the stacked display as one tall picture;
  `-d 1` the upper panel. A first try, with GammaOS's menu service stopped by my own testing, hung the whole device after a
  minute; launched from the menu as normal it ran without trouble that once. **Later the same day it proved unstable:** of
  five DualStack launches only that one ran clean. Two more `am start` launches hung the whole device (adb gone, power button
  needed); launched by GammaOS's own menu, one hit a GPU hang at kickoff (`work gpu` 10 s, `VK_ERROR_DEVICE_LOST`, a crash in
  libGLES_mali), two hung before the first frame (Gfx Thread and GPU both at 100%, black screens), one of those then hung the
  device; one screen only (no second SurfaceView) hung the same way, so the second surface isn't the cause. Blitz is off the
  DualStack list again. Without DualStack, two screens ran all day with no hang. Open: a finger on the crew screen's GO PLAY reaches the
  game (`[touch raw]` at 0.86,0.86) but doesn't press it (START does); the panel stays with GammaOS's Control Center when
  DualStack is off.
- **RG DS Plus, the team's own two-screen run (2026-11-02, about 15:35-15:53):** Control Center off, launched from GammaOS's menu.
  Both Screens settled at 28-30 fps with both screens shown about 30 a second; Stylus at 1.5x 27-30 fps (the team found Stylus
  rough: a first DS). His picks: Field Ahead 0.50 and Camera Zoom Out 12.5% (now the defaults; 12.5% is a new choice),
  Sharpness 1.75x. Found: (1) Android's input dispatch is off after every launch from GammaOS's menu with the Control Center
  off (reproduced from a clean restart): no pad presses reach the game until a sleep and wake (a power tap, then another);
  an app can't turn it back on itself, so it's GammaOS's to fix; (2) taps on the crew screen and any menu were taken by full-screen overlays that take no mouse
  (`[ui tap] ... hover body#`), fixed in recompfrontend 0028 (`pointer-events: none` on them): on the device a tap on GO PLAY now clicks it (`[ui click] button ...`) and the game goes to kickoff;
  (3) quitting crashed the game (RDRAM unmapped while game threads still ran) and GammaOS's system_server then hung and
  restarted, fixed in n64modernruntime 0013 (Android keeps RDRAM to the end): Save & Quit on the device saves, closes with no crash and system_server stays up; (4) the bottom HUD's
  small labels lose strokes in Both Screens at 1.5x ("TO JOIN IH"): each half renders 383 lines.
- **Retroid Pocket 5, Adreno 650:** one screen at 2x no antialiasing, 29-30 fps. A real second swap chain as a stacked test on
  one panel, 29.9 fps over 10 capped sessions, 0 failures. Never a real second display, focus path or `Presentation`, and
  nothing since 521f4dc has run on it.
- **Commit 683fae1 through c3be6e3:** compiled in the Modern flavour for Android, Mac, Windows, Linux, iOS and PS5 (no Classic
  target built), booted on the Mac, not run on any Android device or a console: safe start's present test, the recipes, the
  watchdog, the tap ring, the lower-screen buttons and Stylus's icons.
- **69ea831, review fixes:** compiled in the Modern flavour for Android, Windows and Mac; Linux, iOS and PS5 not rebuilt. Potato
  Mode's process now ends with its activity, and its remote service isn't restarted by Android; the Retroid keeps SDL's wait. On
  the emulator, networking off, Potato Mode on: removing the task with a game up ended the process in about 2 s, with the service
  gone, the next launch started clean, and with no network the toast said `Recipe: none (no answer)`. With Potato Mode off, the
  same step hung the main thread in SDL's `join` (`SDLActivity.java:605`), so that hang is real, and it's still there off Potato.
  Not run anywhere: a Quit followed by a launch, a Back that closes the activity (Back on the emulator closed nothing), the
  tall-surface change (the emulator letterboxes instead of giving a tall surface), and the compact budget (compact mode needs a
  small limit). `Process ... has died` and `earlier exit: reason 2 (signaled) status 9` after `[launch] the game's activity is
  closing` are this, not a crash. The Worker's stale-line fix isn't deployed.

**Known:**
- A crash in the team copier (ovl_b 0x802F5360), twice on the emulator as a game started, when it ran at under 10 fps, with
  menu buttons pressed blind. Other emulator starts got past it, and it has never happened on the Mac. Unexplained. Crash
  reports now carry the chosen teams and their roster pointers (`[crash]` lines), so the next one says more.
- Clip playback on Vulkan hasn't been seen: the emulator recorded a clip but was never driven back to a play select after a play.
- At the camera's widest, a player far across the field can still be off the edge. That limit is the team's: sideline to sideline.
- Tile taps only work for player 1, in his half. Other players' taps aren't told apart.
- A receiver tap on the lower screen hasn't been exercised live: receivers were on the upper screen from the snap in every run.

**Not tested:** a Retroid Pocket Duo or a Thor, a real second display on Adreno, a monitor, DualStack gameplay, and two screens
on 1 GB of RAM meeting the bar (27 fps presented on both, no gap over 1 s in 10 minutes).
