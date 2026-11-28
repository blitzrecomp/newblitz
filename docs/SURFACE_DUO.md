# Surface Duo

Branch `surface-duo`, started 2026-11-26. Not in a release: it's on hold for 1.0.5 or later.

The Duo's two panels are one display, not two. The app's window covers one panel, or, spanned (dragged onto the hinge),
both, with a band in the middle that shows nothing: 84 pixels on the Duo (panels 1350x1800), 66 on the Duo 2
(1344x1892). So BLITZ DS's second-display path never fires there; this is the DualStack layout with a real hinge instead.

## What it does

| How it's held | What the game does |
|---|---|
| Spanned, hinge across (like a DS) | Two screens: the upper panel is BLITZ DS's upper screen, the lower panel the game's window (play select, on-screen controls) |
| Spanned like a book | Turns itself to the DS hold (spanned is always two screens, the team 2026-11-26) |
| One panel | Landscape, the whole panel (1800x1350 is 4:3); the other panel stays free for another app |
| A safe start, spanned | One screen, above the hinge |

DS Screens → **Controller** (any two-screen device): the game on the upper screen throughout, the lower one the touch
controller, up whether or not a pad is connected.

It lays itself out again whenever the window changes: spanning, unspanning, and turning the device all happen mid-game.

## How it's built

- `BlitzHinge.java` finds the hinge: Microsoft's `com.microsoft.device.display.DisplayMask` by reflection (the manifest's
  `uses-library` isn't required, so every other device installs as before), else the Duos' known spanned sizes.
  `BLITZ_HINGE=left,top,right,bottom` in `test_env.txt` stands in for it on any device.
- `BlitzActivity.layoutForHinge()` sizes the game's SDL surface and the upper screen's `SurfaceView` around the hinge. The upper
  view is BLITZ DS's second surface (`makeSecondView`), so the renderer, taps and the drive panel are the same code as on two
  displays. Unspanning removes it, and its surface's end tells the renderer.
- On a Duo the orientation is layoutForHinge's: sensor landscape on one panel, sensor portrait spanned (the DS hold, taller
  than wide). SDL's own request is ignored the way a still-tall surface frees it (`orientationFreed`).
- Both panels take touch, so `BLITZ_ONE_TOUCH` isn't set: the upper screen keeps its own SETTINGS and REPLAY.
- `[duo]` lines in `blitz.log` give each layout: the window, where it sits, the hinge and where the hinge came from.
- The probe (`tools/android/probe`) logs a "hinge" section at launch and after every change: the model, the DisplayMask
  feature, the display, the window and DisplayMask's rectangles for all four rotations.

## The graphics driver (2026-11-26)

The Duo's driver is Qualcomm 512.502 (Android 11/12L era, Vulkan 1.1, no descriptor indexing). It crashes CPU-side in
`vkCmdDispatch` (null+0xf0) at RT64's frame copy back to RAM (`NativeTarget::copyToNative`), on the second write to the
game's 512x255 framebuffers, a few frames into a game. Proven not the cause, each by a run: the shader (an empty one crashes
too), texel buffer formats, descriptor-indexing flags, out-of-area clears, descriptor-set rewrites, RAM uploads. The Khronos
validation layer (`BLITZ_VK_VALIDATION=1` with the layer packed in a test APK) finds nothing on that dispatch. Fixed along the
way, for every device: plume 0027 (no indexing flags without indexing), 0028 (clears inside the render area), rt64 0039
(frame texel buffers declared Unknown format, a SPIR-V fixup), 0040 (write sets not rewritten).

So on Qualcomm 512.5xx the frame copy is off (rt64 0042, `[rt64] Qualcomm driver ...` in the log; `BLITZ_RAM_MODE` overrides).
Effects that read the frame back go without it. A real fix candidate: Mesa Turnip through libadrenotools, not tried yet.

Also found here: SDL listed the accelerometer as a joystick ("Android Accelerometer"), so two screens hid the touch
controls as if a pad were in hand. `SDL_HINT_ACCELEROMETER_AS_JOYSTICK` is now 0 everywhere.

## Playtest rounds, 2026-11-26/27 (build 882, commit d32a4d1)

Everything the team asked for in the first Duo playtest, where each piece lives, and the live dials (`TALL <name> <value>` through
the live input file; `remote_input.txt` on Android) that move it without a rebuild. Checked on the Mac's two-window test
(`BLITZ_DS=1`, below) unless it says otherwise.

| Asked for | Now | Where / dial |
|---|---|---|
| Down and distance (1ST AND 30) almost on the upper screen's bottom | There, words in their plate, clear of the rules and tip cards | `tall_view.cpp` `panel` -29 (the team's pick in the dev kit, 2026-11-27), `panel_words` nudges the words alone |
| Tip and rules cards (GENERAL HINTS) | Centred on the upper screen | `card` (0) moves them |
| Score panel after a score off the thumbs, clear of the hinge | Upper screen, whole | `score_lift` 8; pieces told apart by image (10j words, SP_ plates) |
| TOUCHDOWN! shouldn't jump; a little higher, clear of the score panel | Placed from the game's layout on its first frame (MSG_ and SPARK in group 0x800 had been measured with the panels) | `msg_up` 10 |
| Play results (12 YARD RUN) cut off at the right | Top of the upper screen, unshifted (the shrunk HUD shows only the middle of its 512: shifts ran them off) | `result_dx` 0 |
| Punt and kick meters missing | Showing (the team's Duo, punt): they were placed from the left edge of the shrunk space, off screen | `meter` 0 upper left, 1 as other 2D, 2 lower left |
| Pause menu broken / off centre | Lifts whole while paused | `pause_up` -3 |
| Tips on the upper screen, dismissable by touch | Any touch on either screen, or any button | `hints.cpp` `hints_touched` |
| START a right-facing triangle | Drawn (touch controls), ▶ on the phone controller page | `touch_controls.cpp` `start_pixels` |
| Controller look (DS Screens → Controller, and menus/team select) | Phone controller's caps, ground, Barlow lettering, knob about half the ring | `touch_controls.cpp` `pad_look` |
| Receiver → button touching the edge | In from the edge | `edge_pull` capped 0.045 |
| Stick set off the system's edge swipe | Gesture exclusion over the thumbs | `BlitzActivity` `setSystemGestureExclusionRects` |
| Grainy two screens | 3x with MSAA (4x dipped below the guard in real play); the guard ignores 10 s after a state load | `BLITZ_DS_RES_MAX`, `GFX dsmax=N` |
| Play art (Madden routes) | On the turf under the players; waits for the camera to stop (no stray lines in the first play's flyover) | `play_art.cpp`, `PLAYART off/first/full`, Rules → Play Art |
| Drive endings | TD, FG, Safety, Punt, Downs, Interception, Fumble, End of half (pass/run guesses INT vs FUMBLE) | `play_log.cpp` |
| End zones in team colours | Commonest strong colour of each logo | `ds_view.cpp` `team_color` |

Test tools added: `MARK save|load <name>` (named save states, `savestates/mark-<name>.state`), `GFX res=… msaa=… dsmax=…`,
`[touch] controls …` lines (why the touch controls are up or down), `[playart] camera moving …`, the Android crash line with
the fault address. On the Mac: `BLITZ_FLAVOUR=2026 tools/live.sh start BLITZ_DS=1 BLITZ_VIRTUAL_PADS=1 BLITZ_WINDOW_SIZE=900x675`
gives both screens as two windows; screenshot them by window id (no touch there, so the controller look is Duo-only).
`tools/ds_harness.sh` does a whole run of it: starts that, drives into an Arcade game (`--stadium N` forces one through
Rules → Stadium and puts it back), shoots both screens stacked as the Duo is held at the opening, kickoff and each play
(`--plays N`), saves marks `ds-kickoff` and `ds-line` (`--from ds-line` reruns from there), takes `--tall K=V` and `--live CMD`
for dials, and writes `build/shots/ds/<time>/` with a contact sheet and the run's `[tall]`/`[board]`/`[playart]` lines.
The dev kit (`tools/devkit/devkit.py`, then http://127.0.0.1:8765) is the same by clicking: start the two-window test or
get into a game, marks to load and save, the pad, every TALL dial as a slider (Copy changed dials gives the TALL lines),
the Rules cheats (stadium, weather, fog, field), BOARD, Play Art, DS Screens, resolution, hints, replay, any live command,
and a stacked screenshot after each click. Its Android target works the USB device: remote.sh for commands, adb for shots.
Two live commands behind it: `DSSTACK [width]` puts the Mac's two windows one above the other at one size that fits the
screen (the upper's swap chain follows), and `HOLD on [frames]` / `HOLD refresh` / `HOLD off` keeps one moment to tune
against: saved as the mark `hold`, frozen that many frames on; each refresh loads it, runs the same frames with what changed
(dials, graphics, settings) and freezes again, its save-state toasts kept quiet (dev_input.cpp `dev_hold`).

Surface AutoSpan (the team approved, 2026-11-27): `com.blitzrecomp.newblitz-personal` added to the system setting
`com.microsoft.surface.autospan.packages` (it held only Microsoft's camera: `null:com.microsoft.surface.camera.v2-personal`,
saved in build/duo_logs/autospan_orig.txt). The Surface shell reads it when it starts, so it takes effect after a reboot.
Undo: `adb shell settings put system com.microsoft.surface.autospan.packages "null:com.microsoft.surface.camera.v2-personal"`.
AutoSpan only spans a new task while both panels and the spanned area are empty (services.jar `TaskPositioningAutoSpan`:
display areas 10001-10003 unoccupied). It spanned BlitzLauncher's task, but that task was still there when the game's own
(singleInstance) task was placed: `TaskPositioningAutoSpan : NOT applied`, then `ConditionalStackingExistingTask` put it on
one panel (TDA 10002). On a Duo, BlitzLauncher now starts the game from onDestroy, 150 ms after it's gone (branch
duo-autospan): `TaskPositioningAutoSpan : APPLIED ... BlitzActivity ... TDA selected: 10003`, and it opens as two screens.

The far stadium board's mirrored writing (2026-11-27, every device, not just two screens): Day, Night and Snow's two boards are
one draw in the stadium model, 8 corners at each end, both running the image the same way along z while facing each other,
so one always read backwards. `board_digits.cpp` `face_boards` runs the +x end's s the other way in the model's vertices
(live `BOARD 1|-1|0` picks the end). Seen on the Mac: the Day stadium's flyover from a saved mark, `BOARD 0` backwards,
`BOARD 1` reading "1 DOWN AND 70  BALL ON 30"; the other end is untouched (it already read right).

Round 4 (2026-11-27, builds 893 to 902): the drive board's Screens button (DS Screens by touch; Controller skipped with a
controller in hand), Game + Companion's receiver icons on their own layer over the upper screen (RecompFrontend 0051,
`recompui::ds_icons_set`; putting them in the panel replaced the drive board), Controller mode's icons over the picture,
`panel` -29 with 1ST AND 30's words in their plate (an escape byte hid their font name, so they took `score_lift`; new
dials `panel_words`, `card`), square logos (the header's drawn size), the upright-panel squish toast (`BlitzActivity`
layoutForHinge, against the Duos' panel size: boxed apps are told the box's display size). Controllers: per-controller
Buttons choice and Button Icons → PlayStation moving a Switch-type pad to positions, applied at the game's reads and in an
SDL event filter chained in front of RT64's (replacing it lost Start). Wi-Fi adb: paired 2026-11-27; use the mDNS entry.
Dev kit (`tools/devkit/devkit.py`): HOLD freezes a moment and every change redraws it; DSSTACK stacks the Mac windows.

FUMBLE in its box (2026-11-27): its box is group 0x802 (a built image at 256,344), not the panels' 0x800, so it went
down with the other 2D while its word (group 0x34, 10jFUMBLE at 353) stayed: word over an empty box. 0x802 is measured and
placed as a call's box now (tall_view.cpp `fumble_frames`). Forcing one: `POKE32 0x8033B994 1` before a tackle (the
game's own debug flag at 0x80322B58: the next tackle's fumble chance is 100000 of 10000); checked on the Mac, marks
`fum-snap`, `fum-box`. Instant Replay's bar keeps each label on one line: under 1500 dp across (4:3, a Duo panel) 1X and
78% instead of FULL SPEED and SAVING 78%, under 1150 without < and >.

## Not known yet

- Known now (the team's Duo, Android 12L): DisplayMask reports the hinge only while spanned (0,1350 to 1800,1434 held like a DS);
  spanning and unspanning mid-game work.
- Frame rate at full fidelity (Auto resolution, MSAA 2x) on two screens. At 2x without antialiasing it held 30.
- With no controller the on-screen controls stay up on the lower panel, and tile taps give way to them (BLITZ_DS.md).
- The launcher and the ROM screen still lock to landscape.
