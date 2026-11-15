# Save states and per-play checkpoints (feasibility, 2026-10-16)

> **Update, 2026-10-16: true save states are built** (F5 save, F7 load; both apps). This is design A2 below, done as the plan in `build/analysis/savestates/PLAN.md` describes, with these differences:
> - **Resumable code:** `tools/savestate/resumable.py` makes every function that can reach `process_sleep` or `osRecvMesg` keep a shadow stack of call sites (`--sinks sleep,recv`). `make app` / `make app2026` build from it (`SAVESTATES=0` opts out). CPU time is unchanged (12.55 s vs 12.54 s over 70 s of attract).
> - **When:** snapshots are taken and restored at the dispatcher's loop head (hook 0x80273D7C), only when:
>   - every process is asleep in `process_sleep` with an unbroken chain, or has never run;
>   - the other threads are in `osRecvMesg` (idle in `pause_self`);
>   - no RSP task is in flight and no must-deliver message is pending (runtime patch 0005).
>
>   That holds on about 45% of frames, so a request is served within a frame or two.
> - **Fixed threads stay alive:** their registers are put back in place, and they must be at the same call sites as in the snapshot. Process threads are destroyed and recreated. Each new one runs the entry function the snapshot recorded (through the site → function table, since overlays share addresses) and walks its chain down to `process_sleep`.
> - **Thread contexts:** every thread is registered through the runtime's `thread_create_callback`. Host context pointers are zeroed in the stored image, so a destroyed thread can't read its own pointer back and keep running.
> - **Files:** `<app folder>/savestates/slot<N>.state` (8.4 MB each) holds RDRAM, registers, stacks, the run queue and the loaded overlay/stadium. Its build id is a hash of the site table, so it loads only in the same build. Loading it in a fresh session works: the fixed threads come up at the same addresses and sites.
> - **Verified:**
>   - attract demo restore, including a process that slept in ovl_b while ovl_a was loaded and resumed after the front end came back;
>   - a real game in both apps, where the clock and score return to the saved values;
>   - 17 restores in one session without leaking threads;
>   - save, quit, relaunch, load.
> - **Controller:** the hotkey (L3 or R3) + R1 saves, + L1 loads (the RetroArch handheld way; L1 saved before 2026-11-02), + □ / ○ (or D-pad left / right) step the slot and + △ (or D-pad up) is Redo Play (Settings → General → Save State Buttons). On an RG DS, where L1/R1 are turbo, + R2 saves and + L2 loads, and its L2/R2 aren't turbo any more (2026-11-03). A combo counts only when its button goes down after the hotkey is held, so holding turbo and then pushing a stick in does nothing. A phone controller's ··· menu sends the same commands.
> - **Statuses** (saved, loaded, the slot, why not) show as a toast at the bottom right for about 2.5 s (`src/main/toast.cpp`), and on phone controllers.
> - **Host-side state** (`include/blitz_savestate.h`): modules register what follows the game, and it is stored and restored with each state:
>   - the resume checkpoint's in-progress flags;
>   - the opponent-select screen's state;
>   - the Advanced season's id and seed.
>
>   Season Mode is latched at launch, so a state from the other mode is refused with a message.
> - **Advanced Season games** (2026-11-03; before, every save in one gave "broken chain"): its week driver is host code (`adv_engine.cpp`), which calls the match (`802FD094`) itself. `play()` pushes the vanilla driver's own call-site entry (`802FBBE4` → `802FD094` at `802FBD80`) so a sleeping match reads as an unbroken chain; on a restore the generated `802FBBE4` hands the thread to `blitz_adv_week_driver` (resumable.py keeps a teamplus ENTRY hook ahead of the resume switch), which resumes the match, records it and finishes the week from `adv_live` (week, game, quarter length, pad; kept with the state), skipping the week's CPU-skill pass and Super Bowl intro, which already ran. The Season message screens between games stay unsavable and say so at once ("Season screens can't be saved: save once the game starts").
> - **Tournaments need nothing:** the bracket lives in RDRAM and `tournament.bin` is rewritten from it after each result. Replaying a game after a load replaces that result rather than adding a second one.
> - **Redo Play:** built on the same machinery. The checkpoint hook at the top of the per-play loop asks for a snapshot, which is taken at the next quiescent loop head and kept in memory (the last 8). A play's end (the plays.log hook) and play select's header being drawn (PLYTOP.WMS, gfx_ext.cpp) decide whether a redo goes back to this play or the one before. Game init and game over clear the history. Snapshots stay in memory, about 67 MB at most.
> - **Slots:** three, at `savestates/slot<N>.state` (the old `quick.state` becomes slot 1). F6 (Shift+F6 for the previous one) or R3 + D-pad right/left picks the slot.
> - **Save & Quit:** quitting during a game asks Save this game?; Save & Quit writes `savestates/quit.state` plus a one-line `quit.txt` (score and situation), then quits; Don't Save asks once more. The next start asks Pick up your game? (Resume loads it and deletes both files; Discard deletes them).
> - **Option B below is built too:** Settings → General → Resume Interrupted Games (Experimental, off by default; `src/game/checkpoint.cpp`).

Everything below is the original study, written before save states or checkpoints were built. A read-only study of the generated code and the runtime. "Verified" means read in the generated C, the runtime source or `blitz2000.toml`; the rest is inferred. Scratch tools are in `build/analysis/savestates/`.

## Why emulator-style save states don't carry over

Copying RDRAM and writing it back does not work on this runtime (verified):

1. **Every Blitz process is a host thread.** The game's processes (`ball`, `plyrs`, `cambot`, `score`, …) are real `OSThread`s, and in the recomp each is a `std::thread`. Where a process is suspended exists only in that thread's host call stack.
2. **Return addresses aren't in RDRAM.** N64Recomp turns `jal` into a C call and never writes `$ra` (`ctx->r31`), so the guest stack frames in RDRAM hold stale return slots and the call chain can't be rebuilt from them.
3. **Registers aren't in RDRAM.** Each thread's `recomp_context` is a local of `run_thread_function` (`lib/N64ModernRuntime/librecomp/src/recomp.cpp`), on its host stack.
4. **RDRAM holds host pointers.** `OSThread.context` (+0x18) is a host `UltraThreadContext*`, so old RDRAM brings back dangling pointers.

The set of processes also changes every play, so there is no point between plays where every host stack is known.

Capturing is the easy half. ultramodern runs one game thread at a time and parks the others on a semaphore, and external events arrive only when a game thread calls into the OS. Restoring is the hard half.

## Game threads (verified, all 9 `osCreateThread` sites)

| Entry | OSThread | Priority | Waits in |
|---|---|---|---|
| `idle_thread` 0x800012C4 | 0x800313B0 | 0x51, then 0 | `pause_self` |
| `sched_thread` 0x800014D0 | 0x80031200 | 0x50 | `osRecvMesg(0x8005DFD0)`; sends to 0x8005DF20 each retrace |
| `process_dispatcher` 0x80273D48 | 0x8005E5B8 | 0x0A | `osRecvMesg(0x8005DF20)` at the loop head 0x80273D7C |
| `__scMain` 0x80006134 | sc+0xF0 | 0x64 | `osRecvMesg(sc+0x40)`, sc = 0x8005E000 |
| `audio_thread` 0x8000B218 | 0x8003A4B0 | 0x5A (inferred) | `osRecvMesg(*(0x8003A4A4)+8)` |
| `cont_poll` 0x80000490 | 0x8002F030 | 5 | `osRecvMesg(0x8005BD70)` |
| worker `func_80002DE0` | 0x80039940 | 6 | `osRecvMesg(0x8005EA90)` |
| each game process | process+0x30 | about 20 | stopped in `process_sleep` → `blitz_osStopThread(NULL)` |

How processes work (verified):
- **Creation:** `func_8027427C` allocates a 0x1E0-byte struct from a pool and a stack from the default heap, then calls `osCreateThread`. Lists: free 0x802A9B84, live 0x802A9B80, current 0x802A9B88.
- **Sleeping:** `process_sleep` 0x80273CCC is just `current->sleep |= a0; blitz_osStopThread(NULL)`. There is no stack switching.
- **Waking:** each tick (body from 0x80273EF8) the dispatcher wakes due processes with `blitz_osStartThread`. A process runs until it sleeps again.
- **Call depth:** 562 recompiled functions can reach `process_sleep`. Chains from a process entry down to the sleep are mostly 7 to 12 frames, at most 18. Many pass through overlays, and 13 make indirect calls.

## Host state outside RDRAM

| State | What a restore needs |
|---|---|
| Thread registers, the running queue (`threadqueue.cpp`) | save and restore |
| Pending external messages (PI/SI/SP/DP completions) | snapshot only when none are pending |
| Timers | nothing: `osSetTimer` is never called |
| Audio (SDL queue, `src/game/ai.cpp` FIFO model; libmus is in RDRAM) | flush and reset |
| Loaded overlays (ovl_a/b/c, stadiums) | re-register the snapshot's set |
| RT64 (reads display lists from RDRAM after submit) | wait for the GPU thread to go idle first |
| Controller Pak file | never roll back across a pak write |
| Advanced mode's host league state | restrict rollback to one game, or save it too |

Blitz uses only the first 4 MB of RDRAM.

## Options, ranked by risk (as studied; B and A2 were both built later)

**B. Per-play situation checkpoint: low risk, a few days.** At each play select, save the game-state block 0x802EA420-0x802EA520 (`src/game/play_log.cpp` names the fields) plus stats, timeouts, turbo/on-fire and the RNG seed (0x8005BAF0); keep the last 3-5 on disk. To resume after a crash, start a game with the same teams and write the block back at its first play select. The result is the same situation, not a bit-exact state. Still to check: which other fields the game keeps per game (stats, fire), kickoff/extra-point states, and whether the HUD re-reads the block.

**A2. True save states through resumable generated code: weeks, moderate risk.**
- **Post-pass:** a script over `build/RecompiledFuncs` (every call already has an `after_N` label) gives each function that can reach `process_sleep` a resume label before each call and a dispatch at its top. A per-thread shadow stack records the call sites, including the real targets of indirect calls.
- **Snapshot:** taken at the dispatcher loop head when every process is stopped in `process_sleep`, the fixed threads are at their usual waits, and no I/O, GPU task or pak write is in flight. Copy 4 MB of RDRAM plus each thread's registers and call sites (about 1-1.5 MB compressed).
- **Restore:**
  1. Wait for the GPU thread to go idle.
  2. Destroy the process threads.
  3. Write RDRAM back and re-register the overlays.
  4. Recreate each process as a new host thread that walks its recorded chain back down to `process_sleep`.
  5. Restore the fixed threads' registers, drain messages and reset audio.

  That's about one dropped tick.
- **Bonus:** the snapshot holds no host addresses, so it would even load in a fresh launch, which makes save states crash-proof.
- **Risks:** tail calls, indirect calls, redoing the pass after each N64Recomp update, and a click from resumed notes.

**A1. Fibers with stack copying: not recommended.** Every game thread would become a fiber on one host thread. This rewrites ultramodern's threading, parked frames hold heap pointers, and nothing survives a relaunch.

## The file table (2026-11-14)

The game keeps its own copy of the file table in RDRAM, with where each file sits in the ROM image, and a state carries
that copy. The ROM image is built at each start: the cartridge, then the art, uniforms and announcer banks laid over it
or added from the app, the 2026 Season and Add-Ons. Change those between saving and loading (a pak update, art added or
removed) and the copy points at the wrong data: the next file the game loads is garbage. A state saved on 2026-11-03
crashed every time at its first touchdown (`sprites_draw_fg` calling 0xF1000000). Now:

- The `files` module (art_dropin.cpp) records a hash of the table's names in order, and a state whose list differs is
  refused: "This state was saved with other Add-Ons (art, uniforms or a different 2026 Season), so it can't be loaded now."
- Every restore (slots, Resume, Redo) then gives each entry whose name matches this run's size and position
  (`fs_sync_ram_toc`). The Nov 3 state moved 935 of 2,765 entries and played on past the touchdown. States from before
  this have no `files` module: they get the resync but not the check.

## Done so far

`<app folder>/plays.log` records each play (clock, score, result, next down and spot). That is the logging half of the team's per-play autosave idea. Since then, B is built as Resume Interrupted Games and A2 as true save states (the update at the top).
