# Instant Replay

The last play, full screen, with the game stopped behind it: forward and backward, slow motion, Save. Every platform, every
game mode, New Blitz and Original 1999, one screen and BLITZ DS. Built 2026-11-01 on branch `replay`.
**Nothing here has been run.** See "What's been checked".

## Using it

- **Open:** the game's pause menu (a new REPLAY row, between OPTIONS and QUIT GAME), **F9**, **R1 at play select**, or on
  BLITZ DS the REPLAY button beside SETTINGS or a tap on the looping last play. `REPLAY open|save|close` is the live command.
- **Controller:** ✕ play/pause, D-pad or left stick scrub (a press steps a frame, held it runs), ○ speed (full, 1/2, 1/4),
  △ save, □ back (as every Back, and the game's own menus), Start HIDE (the controls away, paused or not, until Start again or a tap). By position, as ever.
- **Keyboard:** Space, Left/Right, V, S, H, Backspace. Esc still opens Settings, over the replay.
- **Touch and mouse:** the buttons, a drag along the scrubber, a tap on the picture (pauses, or brings the controls back).
- The controls go away after three seconds of playing untouched, leaving a small REPLAY mark.
- **BLITZ DS:** each frame plays back where it was kept: over both screens, or on the upper one alone (a Stylus play), so a
  play that switched screens mid-way still comes out right. Saved replays go to the gallery (Pictures/New Blitz) on Android.
- **Settings → General → Instant Replay** turns it off, memory and all.

## How it's built

### Frames: a GPU ring, not re-simulating

`lib/rt64/src/hle/rt64_blitz_replay.cpp` (`platform/rt64-patches/0013`): each kept frame is the VI's picture drawn into a small
texture. It's the BLITZ DS clip's method, moved off the second screen so it works on one screen.

Why not run the play again from a save state (Redo Play's snapshots plus recorded input)? It would give free cameras and any
resolution, but:
- it depends on `BLITZ_SAVESTATES`, and on restores that destroy and recreate every process thread, each time the scrubber moves
  backward, at 8 MB a keyframe;
- getting back "exactly where it was" means one more restore, not leaving a loop;
- the game would really run during the replay: its drive log, sounds, Controller Pak and checkpoint hooks would all see it;
- nothing shows the simulation is deterministic across a restore. Redo Play doesn't need it to be.

Cost: one full-screen triangle at low resolution, no readback, for a chosen rate of frames. Same code on Metal, Vulkan and D3D12.

| RAM (`SDL_GetSystemRAM`) | Frames | Rate, length | GPU memory |
|---|---|---|---|
| under 2.5 GB | 320x180 | 15 fps, 8 s | 28 MB |
| under 5 GB | 480x270 | 20 fps, 8 s | 83 MB |
| under 12 GB, or any phone | 640x360 | 25 fps, 10 s | 230 MB |
| 12 GB and up | 960x540 | 30 fps, 10 s | 622 MB |

`BLITZ_REPLAY_TIER=0..3` forces one. The textures are made four a frame, so starting costs no hitch, and freed five seconds after
a game ends. Each frame is stretched into the ring's 16:9 and its real shape kept beside it, so 4:3, widescreen and BLITZ DS's
tall picture all use one ring and a window resize doesn't throw it away.

**What's kept:** live play only, not play select or the pause menu, so waiting at play select doesn't push the play out. The end of
play select marks the next frame as a play's start (the cyan ticks); the replay opens at the last play and loops inside it.

**Dropping it:** under 26 fps for 15 s of play (`BLITZ_REPLAY_FPS_FLOOR`): half the frames, then 30 s later off for the run, with the
memory, and a `[replay] too slow` line. Never back up in a run, as `ds_view.cpp` does.

BLITZ DS keeps its own 47 MB clip next to this ring. They could share: `secondDraw` could show ring frames. Left alone for now to
stay out of that code while its screen modes are changing.

### Stopping the game

`src/game/replay.cpp`. The dispatcher's loop head (the hook `blitz_savestate_tick` shares) is the one point every frame passes with
every process stopped in `process_sleep`. It stays there, taking retrace messages from its own queue in a real `osRecvMesg`. That
matters: threads are cooperative and external messages only arrive when a game thread asks (`threads.cpp`), so a host sleep would
stop the scheduler and audio threads with it.

- No save-state quiescence is needed, because nothing is captured or restored. Coming back is leaving the loop.
- Save states wait, since their tick comes after. A quit prompt closes the replay first (`savestate_quit_prompt`), since Save & Quit
  needs that tick.
- If the dispatcher doesn't come round within a second (loading), it gives up: "not while it's loading".

**Audio:** the audio thread keeps running and the host output is muted (`blitz::audio::set_mute`). Stopping that thread would
starve the game's AI pacing (`ai.cpp`) and needs a stopping point it doesn't have. So game state comes back exactly, but music has
moved on silently by however long the replay was up.

**Presents:** RT64 only presents when the screen changes, and with the game stopped it doesn't. `blitz_replay_keep_presenting`
pushes one per VI (`rt64_state.cpp`), since the replay and the UI are both drawn by presents.

**Input:** the UI context captures input, which also stands down the touch controls, receiver icons and DS taps.
`blitz_replay_input` (`dev_input.cpp`) gives the game nothing while it's up, and keeps a button held at closing away until it's let
go, or ✕ on BACK would press RESUME GAME.

### The pause menu

`func_80262D90` in main, the same in vanilla and team+. It's in the game's code, so it's four instruction patches and five hooks
(`blitz2000.toml`), all explained in `replay.cpp`: the row count and the cursor's wrap go to 4, QUIT GAME moves from row 2 to 3, and
row 2 falls through the code's dispatch to a hook. The row table grows into the label table's place, so the labels come from a
hook. "REPLAY" has no room in game memory, so it's swapped into QUIT GAME's bytes during the text call, as the checkpoint's
"SAVE THIS GAME?" already does.

Rows: RESUME 229, OPTIONS 179, REPLAY 129, QUIT 79. **The 79 is a guess from the numbers: look at it in game.**

### The screen

`src/main/replay_screen.cpp`, a recompui context over the frame RT64 draws in place of the game's. Blitz '27's art: `header_bevel` and
`rule_flame` for the INSTANT REPLAY banner, which slams in over one white frame; `panel_frame` with chrome corners for the controls;
`selector_cap_*` on the scrubber's thumb; Barlow Condensed italic in yellow. Clickable, not Button, so there's nothing for RmlUi's
focus to land on and the controller's keys stay this file's. On BLITZ DS the replay spans both screens (`blitz_ds_mode` forced to 1,
restored on closing) and the controls are on the first window.

### Saving

`src/main/replay_gif.cpp`: an animated GIF of the play on screen, in `<app folder>/replays/Blitz Replay <date> at <time>.gif`. No new
dependencies, about 150 lines, opens anywhere. At most 480x270 at about 15 fps with a palette per frame: expect 3 to 15 MB for a
play. Frames come back one per present on a thread of its own (`blitz_replay_read_start`), so it keeps animating. The ring holds
still until it's done, even if the replay's closed.

Plume had no texture-to-buffer copy on Vulkan or Metal (`platform/plume-patches/0008`); D3D12's generic path should already do it.

On Android and iOS the app folder isn't the photo gallery: a share sheet or MediaStore step would be the next thing there.

## What's been checked

- **GIF writer:** `tests/test_replay_gif.py`, plus read back by Pillow and ffprobe (frame counts, delay, colours).
- **Patch stacks:** applied in order to clean checkouts, they equal the working trees.
- **Compiled, syntax only:** every new and edited file against the Mac build's flags; the new files, `ds_view.cpp` and `plume_vulkan.cpp`
  against Android's. Windows, iOS, PS5 and Linux haven't been compiled.
- **Not built or linked.** The pause menu's patches haven't been through N64Recomp. **Nothing has run.**

## To test

1. `make` regenerates the recompiled code (the toml changed). Both engines, with and without `SAVESTATES`.
2. Pause during a play: is REPLAY there, does the cursor wrap both ways, do OPTIONS and QUIT GAME still work, is QUIT's row on screen?
3. Open it: picture right way up, right shape, colours as the game's? Then 4:3, widescreen, and BLITZ DS across both screens.
4. Scrub, slow motion, loop; each by controller, keyboard, touch. Close: the game exactly where it was, no stray press, sound back.
5. Save on Metal and on Vulkan (the readback's untested): does the GIF look right, is red and blue the right way round?
6. Handheld: does capture cost frames (`[fps]`), does `[replay] too slow` fire and free the memory?
7. F5 or a quit during a replay; Settings over it; a whole game for memory; `blitz_ds_clip` still recording on DS afterward.
