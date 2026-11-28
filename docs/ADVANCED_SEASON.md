# Advanced mode: season engine and save architecture (research, 2026-10-16)

> **Status (2026-10-19):** Modern Blitz now runs on the original v1.1 game, not the Team Extension (docs/BLITZ2026.md). What follows describes the Team Extension build as it was planned or researched; it still applies to that optional build.

Scope: the team's three modes are
- **Classic:** old rosters, classic seasons, old saves work.
- **Modern:** new rosters and teams, classic seasons and save format, compatible with similar ROM hacks (team+, Resurrection on an emulator).
- **Advanced:** new rosters and teams, updated seasons, its own save architecture, works only in our app.

This note covers what Advanced needs. All addresses are team+ VRAM (`build/teamplus.z64`; segment map in `tools/gen_syms.py`).

Confidence marks:
- **S:** verified statically (disassembly or reference scan) in this pass.
- **V:** verified on real RDRAM dumps.
- **U:** unverified.

Ground rules for this pass: no tracked files were edited, nothing was built or run. The only writes are this file and the scratch tools next to it (§12).

## Status (2026-10-16): implemented

Advanced is a setting in Modern Blitz, the 2026 app (Settings → Season → Season Mode, Standard or Advanced; applied at the next start). What was built differs from the staged plan below. Stage A (six old divisions) was built and tested, then replaced: Stages B and C were done together as one host engine.

**League:** from the content pack's `season` section (`packs/README.md`). The built-in pack holds the 2026 divisions and the real 18-week, 272-game schedule.

**Engine:** `src/game/adv_engine.cpp` plus the pure model `adv_league.cpp`, which `make test-adv` unit-tests. Entry hooks replace three game functions:

| Function | Replaced by |
|---|---|
| week driver 802FBBE4 | host driver |
| week builder 802FDEF4 | host builder (the screens read its output) |
| division standings 802FEA74 | host standings |

The host code also does:
- **results:** recording and the running stat averages;
- **seeding:** 7 per conference, NFL tiebreakers;
- **bracket:** the 7-seed bracket;
- **rating:** the Super Bowl rating.

The game's own sim and match (802FD094) and its message screens are kept.

**Record layout (Advanced):**

| Offset | Content |
|---|---|
| +0 | user team |
| +1 | week: 0-17 regular, 18 wild card, 19 divisional, 20 conference, 21 Super Bowl, 22 over |
| +4 | stats[31][7]; team 31's are at +0x704 |
| +0x368 | highs (nothing displays them) |
| +0x37F | results [22][16] {home, away}, 0xFF = unplayed |
| +0x758 | seeds [2][7] |

**Screens:** about 25 hooks on the standings (8 divisions × 4 rows), schedule browser, week results (16 rows, names from an RDRAM string table at 0x80400100), team schedule (11 + 11 weeks), VS banner and season header. Leaders and the team-select count come from the Stage A hooks.

**Saves:** §3 as designed (sidecar + pak stub, hooks HL1/HL2/HS1/HS2). The sidecar stage tag is 'B'; Stage A test saves are refused.

**Tested** (simulated, separate data folder):
- Houston's real schedule; standings for all 8 divisions;
- a full season with 17 games per team;
- seeds: 4 division winners + 3 wild cards per conference, with tiebreaks (e.g. HOU over TEN at 11-6);
- the bracket (1 v lowest remaining seed) through the Super Bowl; week 22 shown as "season over";
- save, restart and load gave a byte-identical record.

**Also tested:** a played user game inside a week (1-minute quarters; result, stats, next week).

**Tested by the team (2026-10-16):** a played Super Bowl win (SEA 41-38 TEN) end to end: the Super Bowl intro message, the win banner, the rating screen ("RATING: 50.0 ... leaves room for much improvement") and the schedule with all four playoff rounds.

**The rating is the game's formula** (ovl_b 80300FA0): 400 − 100 × losses (losses capped at 4) + 20 × min(avg winning margin, 14) + 20 + 300 − 20 × clamp(avg weighted points allowed − 21, 0, 15), times (completion % clamped 40-80 + 900) / 1000, floored at 500 (50.0). A 13-4 season has already lost the whole loss term, so 50.0 is expected; 90.0+ ("greatest of all time") needs about 0 losses, 14-point average wins and 21 or fewer points allowed.

**Corrections to this document, from the decompilation** (`build/analysis/advanced/decomp/`):
- the builder buffer is 0x802E9508, not 0x802F9508;
- the abort and sim-week flags are 0x8028BFC4 and 0x8028BFD8, not 0x8029…;
- s2 in the driver is the quarter length (1/2/4/6), not difficulty;
- message ids run 0-10;
- builder entry +0 is the home team (the second-listed one);
- the standings' division and conference records are never counted in vanilla;
- 0x803011E4 and 0x803012E8 are font ids, not week constants.

---

## 0. Verdict

**Feasible.** The pieces are:
- Advanced's season record lives host-side, with a stub in the pak.
- The season "model" functions are re-implemented in C++ and enabled at runtime by entry hooks.
- The season screens get about 40 small hooks.

Nothing needs a ROM change, and nothing needs the in-RAM record to move.

**Recommended direction: extend, don't move.**
- Keep the 0x774-byte core record at main bss 0x802EF6F0, where 288 game instructions address it.
- Keep everything new in a host-owned "ADV" state, saved to `<app folder>/saves/advanced/...`.
- Optionally mirror that state into unused RDRAM at 0x80400000.

**Effort, in agent-days, excluding the team's play-testing:**

| Stage | What it delivers | Effort | Main cost |
|---|---|---|---|
| A | Save architecture + 32 teams in the 6 old divisions | 3-4 days | ~6 save hooks, ~450 lines of host code, the existing Season-32 patch list (corrected), schedule data |
| B | Modern 8×4 divisions, 18 weeks / 17 games, 2002-2019 bracket (6 seeds, 2 byes) | 8-12 days | ~11 whole-function host replacements (~1,500 lines C++), ~40 screen hooks, parity harness |
| C | Modern 7-seed playoffs (#1 bye only, 6 wildcard games) | 2-4 days | host bracket logic (~300 lines), week name/message data |

**Top risks:**
1. Faithfully porting about 3,700 bytes of season MIPS to C++. This covers float averages, qsort order, the sim-score fudge and RNG seeding. Mitigation: a shadow/parity mode (§10).
2. Two-phase commit between the pak note and the sidecar (§3.5).
3. Fixed-size buffers that the old formats assume:
   - 18-entry week builder output;
   - a 21-int stack array in the season rating;
   - a 6×6 division grid with live data right after it.
4. Keeping Modern byte-compatible while Advanced is a runtime mode of the same binary (§6).

---

## 1. Corrections and additions to the prior Season-32 research

The prior research is `build/analysis/next2026/_plan.md` and `_details.md`.

### 1.1 The per-game index byte is not write-only (S)
- `ovl_b_func_802FBAAC` runs at every week start and after each user game.
- When week ≠ 0, it calls CRC-32 `func_8027B400(0x802EFA6D, 0x2F)`. That covers record +0x37D..+0x3AB: the 2 bytes before the results plus the first 45 bytes of row 0 (week 1). Those 45 bytes include the index bytes of week 1's 15 games.
- The CRC is stored at 0x8032E91C and nothing reads it there. It seeds `srand` (0x802FBB1C).
- `rand` then drives:
  - the 4 CPU-skill flips (`rand_mod(31)` ×4);
  - every sim that follows, because the sim calls `rand_mod`.

**Consequences:**
- Dropping the write at 0x802FE30C does not break any data. It does change week 1's CRC input from game indices to 0xFF, so the RNG stream differs from team+'s. That is cosmetic, but a 1:1 replay against an emulator is impossible.
- Quirk: srand is re-seeded with the same value at every call, so the random sequence restarts each week. Advanced should seed explicitly: a stored season seed plus the week. That is a free gameplay improvement.

### 1.2 The tail of row 19 is reset to 0xFF, not zero (S)
`func_80263984` (main) runs in this order:
1. memset 0x774 to 0;
2. sets team and week to 0xFF;
3. fills **18 games × 3 bytes = 54 bytes per row** for 21 rows at a stride of 47.

So every row's fill spills 7 bytes into the next row:
- Row 19's fill covers 0x802EFDEC..0x802EFE21. That includes the "free" 0x802EFDF4..0x802EFE1A.
- Row 20's fill covers 0x802EFE1B..0x802EFE50, which runs over the playoff seeds at 0x802EFE48..0x802EFE50.
- The seeds are then re-set to -1 by the following loops (0x80263A04..0x80263A9C), so vanilla is consistent.

Then the stats loop clears `stats[0..29][0..6]` (it bounds on 0x1E, not 31) and the highs. Team 30 is zero only because of the memset.

**Consequences for Season-32:**
- Team 31's stats placed at 0x802EFDF4 start as 0xFFFFFFFF, which is NaN. The running average in `802FE29C` then propagates NaN.
- The S32 plan's H5 hook (before 0x80263AEC, after all reset loops) therefore **must** zero 0x802EFDF4..0x802EFE0F and write the marker. It is mandatory, not optional.
- The same applies to any Advanced layout that puts data at record offsets ≥ +0x37F: re-initialise it in a post-reset hook.

### 1.3 Other findings that matter for Advanced (S)

**Overlay bss is shared.** ovl_a and ovl_b both load at 0x802EFE70, so ovl_b's season bss (0x8033B690.. standings scratch, 0x8033B988 CPU skill, 0x8033BB00 grid) is overwritten during gameplay. It is rebuilt after each game; for example, `802FD094` re-runs the grid loop after the user's game. Host state must never live there.

**The user's game runs inside the sim.** `ovl_b_func_802FD094`:
1. calls main `func_8026376C`, which plays the match;
2. then calls `802FCE70` to collect the result.

ovl_b is re-DMA'd in between, so ovl_b data patches must be re-applied on every ovl_b DMA (the existing `blitz_on_dma_done` path). Host replacements of `802FBBE4` and similar functions simply sit on the host stack across this.

**Week builder output is 18 entries.** `802FDEF4` writes 18 × 6 bytes: games plus bye entries (flag 0x80). With 32 teams and b teams on bye, there are 16 + b/2 entries, so **b ≤ 4 per week**. A schedule with 6 byes in a week needs a host builder that drops bye rows.

**Season rating overflows with 22 weeks.** `80300BA4` (season rating) keeps a per-week int array on its stack at sp+0x18. It holds 18 entries plus 4 copied entries. 22 weeks would overwrite the saved s0 at sp+0x70.

**The standings comparator** is `802FE880`, used by qsort `func_800269B0`. Its order is:
1. win %;
2. division %;
3. conference %;
4. home %;
5. point differential, compared **unsigned** (`sltu`), a vanilla bug.

**The standings entry** is 0x28 bytes:

| Offset | Field |
|---|---|
| +0 | team |
| +1 / +2 | W / L |
| +3 / +4 | home W / L |
| +5 / +6 | away W / L |
| +7 / +8 | division W / L |
| +9 / +A | conference W / L |
| +C | points for, s32 |
| +10 | points against, s32 |
| +14 | win %, f32 |
| +18 | home %, f32 |
| +1C | away %, f32 |
| +20 | division %, f32 |
| +24 | conference %, f32 |

**There is no dedicated playoff-bracket screen.** Playoffs appear in four places:
- the week browser `803010C0` → `80301604`, which uses schedule weeks 17-20 whose entries name seeds via divisions 6/7;
- the user schedule `803019A0`;
- the VS banner (main `func_8024B784`: "WEEK n" / PLAYOFFS / SUPER BOWL by week thresholds 17/20);
- message screens `8030006C(id)` through the wrappers `803003AC..8030050C` (ids 0-9).

Week names are **text**: a 21-pointer table at 0x8032EC50 ("Week 1".."Week 17", "Wildcard", "Playoffs", "Championship", "SuperBowl").

**The standings screen menu is text and data.** `802FF270` uses:
- a name table at 0x8032E978 (6 pointers to "EAST", "CENTRAL", "WEST");
- a position table at 0x8032E930 (6 × {x, y, scale} f32, y = 220/200/180 and 100/80/60);
- AFC and NFC header images 0x0A00039E / 0x0A00039F.

**Sim result buffer.** The game result for sim slot k is at 0x802EA1D0 + 0x90·k, with two 0x48-byte team blocks:
- score at +0x1C;
- 7 stat ints at +0x2C..+0x44.

`802FE29C` turns these into running per-game averages, f32 `stats[team][7]` at record +4, plus season highs.

---

## 2. Memory: where Advanced data can live

| Region | Status | Use for Advanced |
|---|---|---|
| main bss after the record (0x802EFE64) | **live variable** (lw/sw at 0x802EFE64 in 14 ovl_b sites) | no growth in place (S) |
| ovl_b bss 0x8033BA04..0x8033BA0F | free but overwritten by ovl_a during games | scratch only |
| **0x80400000..0x807FFFFF (4 MB)** | see below | **ADV mirror (recommended 0x80400000, 64 KB reserved)** |
| 0x80800000.. | runtime: PI handles at 0x80800000, patch data at 0x80801000 (`librecomp/addresses.hpp`) | avoid |
| 0x81000000.. | runtime: mods + recomp heap (`init_heap(mod_rdram_start + mod_ram_used)`) | avoid |
| host memory (C++) | always safe, survives overlay loads | **primary home of ADV state** |

Why 0x80400000..0x807FFFFF is free:
- **The runtime maps it (S).** It maps 512 MB of KSEG0 read-write (`mem_size = 512 MB`, mprotect in `recomp.cpp`).
- **The game is told 8 MB (S).** `osGetMemSize_recomp` returns 8 MB and `osMemSize` (0x80000318) = 8 MB.
- **The game never asks or touches it (S).**
  - No instruction in any segment references 0x80000318.
  - The only `lui 0x8040..0x80FF` hits are:
    - the framebuffer-clear end bound at boot 0x8000FE64 (0x80400000);
    - two colour constants (ovl_a 0x80324100, ovl_b 0x80301A34).
  - The framebuffers are 0x80380000 and 0x803C0000, ending at 0x80400000.
- **The dumps agree (V).** All 12 existing 8 MB dumps in `build/dumps/` (attract, team select, gameplay) have zero non-zero 4 KB pages in 0x80400000..0x807FFFFF.

When the mirror is needed:
- Only for data that **MIPS code** must read through a pointer: the relocated week-name table, division name/position tables, new strings.
- Optionally for debugging: `tools/live.sh dump` already writes 8 MB, so it captures the mirror.

The host zero-fills the region at `on_init` and re-writes the tables on every ovl_b DMA (idempotent).

---

## 3. Save architecture

### 3.1 Who reads and writes the season record (S, full scan)
- 288 instruction sites in 20 functions address 0x802EF6F0..0x802EFE63. Raw list: `record_refs.txt`.
- The scan covers both lui+addiu/load/store pairs and `lui at; addu at, idx; op off(at)` indexed forms.

| Function | Role | What it touches |
|---|---|---|
| main `func_80263640` | returns &record (leaf, 3 insns) | used only by the save copy-out |
| main `func_8026364C(src)` | **copy-in:** 0x774 bytes slot → record; sets 0x8028BFD0 = 2 | whole record |
| main `func_802679B8` 0x8026802C..0x80268078 | **copy-out:** record → slot buffer (inline loop) | whole record |
| main `func_80263984` | reset (see §1.2) | whole record |
| main `func_80263AFC` | New Season process (called by pointer): reset, ovl_b, team select, week = 0, team = selected | +0, +1 |
| main `func_8024B784` | VS banner "WEEK n / PLAYOFFS / SUPER BOWL" | +1 |
| main `func_80258B1C`, `func_8025992C` | season menu / header | +0, +1 |
| main `func_8026837C` | load/save slot list | **slot buffer +0 (team), +1 (week) only** |
| ovl_b `802FBAAC` | CPU skill, CRC/srand, playoff skill ramp | +1, results row 0 |
| ovl_b `802FBBE4` | week driver (137 sites) | +0, +1, seeds/bracket |
| ovl_b `802FD094` | play-or-sim one game | +0 |
| ovl_b `802FDEF4` | week builder | results, seeds/bracket |
| ovl_b `802FE29C` | record a result: results, stats, highs | results, +4 stats, +0x368 highs |
| ovl_b `802FE75C`, `802FE950` | team schedule (17 entries) | results |
| ovl_b `802FEEC4` | playoff seeding | seeds |
| ovl_b `802FF270`, `802FF8F0` | standings / leaders screens | +0 (highlight) |
| ovl_b `802FFEC0` | leaders calc | +4 stats (31 teams) |
| ovl_b `80300BA4` | season rating | +0, stats |
| ovl_b `803010C0`, `80301604`, `803019A0` | week browser, week results, user schedule | +0, +1, **results (direct reads)** |

**Pak path (S).** Each operation reads the whole note into a malloc'd 0x7AB8-byte image. The file descriptor is at 0x802C8A40; `func_80263C20` sets its size to 0x7AB8 at 0x80263C74.

| Function | What it does |
|---|---|
| `func_80263D74(fd, hdr8, slots)` | copies the 8-byte header at file 0x2CD0 and the 4 slots (0xF00..0x2CD0 = 0x1DD0) **out** of the image |
| `func_80263E40(fd, hdr8, slots)` | patches them **into** the image and writes the whole note (`func_80003894`); returns its status (0 = ok) |
| `func_80263C90` | creates a fresh note: slots +0/+1 = 0xFF |

**The UI is `func_802679B8`:**
- s5 = mode: 2 is user records (0x238 slots); 3 is season.
- s2 = slot index.
- The season slot buffer pointer is at sp+0x34 and the header at sp+0x3C.
- Slot k's pointer is sp+0x34 + s2·0x774, saved at sp+0x64.

The pak note uses all 123 free pages, with about 72 bytes of slack, so **no growth on the pak** (prior research).

### 3.2 Why not relocate or enlarge the in-RAM record
- **Relocating** means rewriting about 288 offset words plus their luis. Many luis are shared with other 0x802E/0x802F globals in the same function (team indices 0x802EA434, 0x802EFE64, ...). That is high-risk for no benefit.
- **Enlarging in place** is impossible: 0x802EFE64 is live.
- **Instead:** the record stays the "core" that the copy-in, copy-out, reset and slot-list code move around untouched. All extra state is host-side. In Stage B the host also owns the core layout above +0x384 (§5.2).

### 3.3 Design

**Folders:**

| Mode | Pak folder |
|---|---|
| Classic | `saves/` |
| Modern | `saves/packs/<pack-id>/` (current behaviour) |
| Advanced | `saves/advanced/<pack-id>/` |

Advanced's folder holds `blitz2000_pak1.mpk` (user records/PINs plus season **stubs**) and `season/slot<N>-<seasonid>-<gen>.adv` sidecars. The pak folder is chosen in `controller_pak.cpp` (`dir = saves[/packs/<id>]`); add the advanced branch there.

**Stub (what goes on the pak, per Advanced slot; 0x774 bytes, mostly zero):**

| Offset | Content |
|---|---|
| +0x000, +0x001 | 0xFF, 0xFF: any non-Advanced reader (Classic, Modern, team+ on an emulator) sees an empty slot and never loads Advanced data into an incompatible engine |
| +0x004 | magic "BZ26ADV\0" |
| +0x00C | season_id u64 |
| +0x014 | generation u32 |
| +0x018 | display team |
| +0x019 | display week |
| +0x01C | CRC-32 of the sidecar payload |

The 8-byte file header (0x2CD0) is left as the game writes it.

**Sidecar (`.adv`, little-endian, versioned):**
- header {magic "BLZADV", format version, engine stage (A/B/C), season_id, generation, pack id, **franchise per team index [32]**, rng seed};
- **the full 0x774 core record**;
- the ADV block: schedule, results, extended stats, highs, seeds and bracket, plus anything the stage adds;
- a trailing CRC-32.

Size is about 6 KB per slot.

**Host state:** `struct AdvSeason` in C++. It is loaded and saved only by the hooks below and reset at New Season. It is optionally mirrored to 0x80400000.

### 3.4 Hooks (all in main `func_802679B8` / `func_80263984`; none is in a delay slot or is a branch target, checked by `hooksite.py`)

| ID | before_vram | Live registers / stack | Host action |
|---|---|---|---|
| HL1 | **0x80267B84** (`addu s0,v0,zero`, right after `jal func_80263D74`; reached only on the slot-read path, mode ≠ 2) | v0 = read status, r21 = mode, slots = MEM_W(sp+0x34) | If mode == 3 and status == 0, for each slot: stub with a matching sidecar (id+gen, or the previous gen) → write display team/week into +0/+1. Stub with no sidecar → stays 0xFF ("orphan", logged). Non-stub season (foreign or Classic format) → remember its original 0x774 bytes, set +0 = 0xFF (unloadable here). |
| HL2 | **0x80267F48** (`jal func_8026364C`; the LOAD copy-in) | s2 = slot, s3 = slot pointer | Copy the sidecar's core record over the slot buffer at s3, so the vanilla copy-in loads the real record. Load `AdvSeason`. Remap team indices if the pack's franchise order changed (or refuse with a notice in Stage A). |
| HS1 | **0x8026807C** (`jal func_8025F4F8`, right after the copy-out loop) | s2 = slot, slot pointer = MEM_W(sp+0x64) + MEM_W(sp+0x34), r21 = mode, s7 = 0 | If mode == 3: generation + 1. Write `slotN-<id>-<gen>.adv.tmp`, fsync, rename to `.adv` (the old gen is kept). Overwrite the slot buffer with the stub. Re-stub the other Advanced slots (+0/+1 = 0xFF). Restore remembered foreign slots byte-exact. |
| HS2 | **0x8026809C** (`j 0x80268120` after `jal func_80263E40` and its delay slot) | v0 = write status | v0 == 0: prune the slot's sidecars to {new, previous}. v0 ≠ 0: delete the new gen (the pak still holds the old stub). Either way, put display bytes back in the buffer. |
| HN1 | **0x80263AEC** (end of `func_80263984`, after all reset loops) | none | New season: fresh season_id, gen 0, new seed. Re-initialise the ADV state and any core bytes the reset filled with 0xFF that the engine uses (§1.2). |

**Hook form:** `if (blitz_adv_on()) blitz_adv_xxx(rdram, ctx);`. Modern and Classic pay one flag test.

Alternatively, HL1/HS1/HS2 can be entry hooks inside `func_80263D74` / `func_80263E40`: each has exactly one caller (0x80267B7C / 0x80268094). The call-site versions are preferred because s2 (slot) and s5 (mode) are live there.

### 3.5 Commit order and failure modes
- **Sidecar first, then pak.** A slot is valid when its stub's (season_id, gen) matches a sidecar on disk, and two generations are kept.

| Case | Result |
|---|---|
| crash or kill between HS1 and the pak write | pak still has the old stub, and the old sidecar is still on disk → loads the previous save |
| crash after the pak write, before HS2 | new stub and new sidecar both exist → loads the new save |
| pak write error (F1 path in `controller_pak.cpp`: the game shows the error) | HS2 deletes the new gen; the stub on disk is whichever version the pak holds; matches either kept gen |
| partially written note (F1 undoes only the failing block) | the stub may be torn → no match → slot shown empty; logged, sidecars untouched for recovery |

- **Pack order changes between sessions:**
  - **Classic and Modern** use pak saves, which carry only team indices, so changing the pack order silently relabels teams. That hazard remains.
  - **Advanced** sidecars record the franchise of each team index, so the load can remap every team index (results, seeds, stats rows, user team) or refuse. That is a real gain over the pak format.
- **Classic or Modern with an Advanced pak:** only if the user copies files between folders. They then see empty slots and can't crash on Advanced data. If they save over a stub slot, HL1 reports the sidecar as an orphan.

### 3.6 The stub guard and the pak history (1.0.4, after 2026-11-26)

**What happened.** On the team's iPhone (1.0.3, Original engine, `saves/original/advanced/blitz2026/`) the stub of season
`0EE109E6D63E3BF3` gen 5 in slot 0 (pak 0x1400) was cleared in one session (13:53-13:59): the slot became an empty
one (0xFF 0xFF then zeros), the season vanished from Load, and its `.adv` files (gens 4 and 5) were untouched. It was
recovered from `.bak2`. What the evidence shows:

- **Not a season Save.** There's no gen 6 file, no `[adv] saved` line and no notice, and the one game function that
  writes the slots (`func_80263E40`) is only called between the two write hooks.
- **Not the record and option writers** (`func_80263FF8`, `802640E4`, `802641F0`, `80264340`, `80264590`): each
  reads the note fresh, patches its own area (all outside 0xF00-0x2CD0) and writes it back.
- **Every note write sends 0x7B00 bytes from a 0x7AB8 buffer** (`pak_read_write_a` rounds to pages), so pak
  0x7FB8-0x7FFF is the 0x48 bytes of heap after the game's buffer at that moment. The wiped image's tail matches the
  next session's ordinary pre-kickoff write exactly, and the heap of save state slot 1 (Nov 14, an exhibition game;
  free block 0x800CDB90), not the heap of slot 3 (the Nov 23 season state, its note buffer at 0x800D59B0). So the bad
  write most likely came after the team loaded slot 1, in the front end, not after "resumed week 3 game 7".
- **The written note is exactly a freshly created Blitz note** for this pak (`func_80263C90`: header and slots
  0xFF 0xFF, the rest zero; the team has no user records). A create can't overwrite an existing note (`osPfsAllocateFile`
  answers EXIST), so the note was deleted first and made again in the same directory entry and pages, which leaves
  the directory and inode pages as they were.

**Root cause (confirmed, 2026-11-26): the save state's copy of the pak's page table.** libultra keeps the last page
table it read or wrote (the inode cache of `__osPfsRWInode`: bank 0x8002CDD4, port 0x8002CDD0, 256 bytes at
0x8005E850) and reads it instead of the pak when the bank and port match. libultra's `__osPfsGetStatus`, the first
thing `osPfsInitPak` does, empties it (bank = 250), but ours (`blitz_PfsGetStatus`, `controller_pak.cpp`) replaced
it and didn't. Within a session that never mattered; a save state brings the copy back.

- In slots 1, 2 (Nov 14) and the Auto slot (taken after loading slot 1), the copy is the page table of this pak (same
  ID) from before it had a Blitz note: every page free (0x0003). In slot 3 (Nov 23) it equals the pak's.
- The game's "pak ready" flag (0x8002B080) is 0 in all of them, so after a load the game's next pak screen runs
  `osPfsInitPak` → `osPfsChecker`. The checker walked the Blitz note's chain through the stale copy: page 5 → 0x0003,
  not EOF, so it took the note for broken, zeroed its directory entry and wrote the copy (all pages free) back as the
  page table: the note was deleted.
- The game's pak screen then got PFS_ERR_INVALID and its dialog (`func_80268F38` case 5) showed **BLITZ NOTE NOT
  FOUND!** CREATE / ABORT. CREATE ran `func_80263C90`: `osPfsAllocateFile` took the first free entry (0) and the first
  free pages (5-127, in order), exactly the old note's, and wrote the empty note.
- The 1.0.4 stub guard wouldn't have stopped it: the checker's directory write deletes the note, which releases its
  stubs, so the new empty note goes through.

**The fix** (1.0.4): `blitz_PfsGetStatus` empties the cache as libultra does, and every restore (slots, Resume, Redo
Play, menu steps) calls `blitz::pak_state_restored`, which empties it and clears the "pak ready" flags, so the game
reopens the pak as it is now before its next use (docs/SAVE_STATES.md, "The Controller Pak's view"). Each restore
logs `[savestate] pak view refreshed: ...`, naming a stale copy when there was one.

**Reproduced on the Mac** (2026-11-26, dev build and app folder, the team's `.bak2` pak). The team's slot 1 loads there, but
quitting its game ran out of heap at the main menu with or without the fix (that state and the Mac's content
differ), so the test state was a Mac attract-mode state with slot 1's 256-byte page table copy (and its bank and
port) written in. With `BLITZ_PAK_STALE=1` (the old behaviour): leaving the title screen deleted the note (`[pak]
season 0EE109E6D63E3BF3 gen 5: its note was deleted`; directory entry zero, every page free), Season → Load said
BLITZ NOTE NOT FOUND!, and CREATE left an image byte for byte the same as the team's wiped one apart from the 0x48 heap
bytes at 0x7FB8. With the fix: `pak view refreshed: its page table copy was stale (0 pages in use then, 123 now)`, no
pak write at all, and Load lists LOS ANGELES RAMS IN WEEK 6. A state made with another pak reopens that pak cleanly
(no new-pak dialog, no write), and a note can still be created and saved.

The guard and the history below stay, for anything else that writes a stub by mistake; their `[pak] kept the season
stub` line says when and in which block.

**The guard** (`src/game/pak_guard.cpp`, used by `controller_pak.cpp`'s block write, Advanced paks only):

- the notes are found through the directory and inode table, and a stub is "0xFF 0xFF, BZ26ADV" at a slot's file
  offset (0xF00 + k·0x774; slot 2's crosses a page);
- a block write that would change any of a stub's 32 bytes gets those bytes back as they are on disk, unless
  `adv_save.cpp` has said a Save to that slot is being written (`pak_season_save_begin` once its sidecar is on disk,
  `pak_season_save_end` in the after-write hook); the rest of the write goes through;
- a stub whose note is deleted (its directory entry cleared, as the Controller Pak manager does) is released for the
  session, so a new note made in its place starts empty;
- Classic and Modern paks aren't guarded: they hold no stubs, and an emulator-style save over a slot that holds one
  must go through. The `.mpk` stays a plain pak image either way (only bytes already on disk are kept).

**The history:** `pak_history/<pak>-YYYYMMDD-HHMMSS.mpk` beside each pak: at each launch and before each burst of
changing writes (one copy, not one per block: a burst ends after 3 s without a write), skipping a copy equal to the
newest. The newest 20 are kept, plus the newest of each of the last 14 days (about 1 MB at most). `.bak`/`.bak2`
stay as they were. Pruning a season (after a Save) keeps every generation a stub in the pak, `.bak`, `.bak2` or any
history copy points at. Settings → Saves → **Backups** (`src/main/pak_backups.cpp`) lists the
copies (the ones equal to the pak now left out) and restores one: at once if the game hasn't opened that pak this run,
otherwise through `pak_restore` in the pak's folder and a restart, applied at the next start's first pak access. The
pak as it was goes into the history first.

Tests: `make test-pak` (synthetic paks; `PAK_GOOD=... PAK_WIPED=...` replays a real wipe block by block).

---

## 4. What is hard-wired, and how each item can change

Mechanisms:
- **D:** ovl_b data rewritten on DMA.
- **P:** instruction patch (static).
- **H:** small conditional hook (register fix-up before a non-delay-slot instruction).
- **R:** whole-function host replacement via entry hook `if (adv) { host(rdram, ctx); return; }`. That form already works: hooks at function entry exist at dma_read 0x800016B8, load_main_segment 0x80004970 and n_alSynAddPlayer 0x8000D940.

| Hard-wired thing | Sites (S) | Can change by | Count |
|---|---|---|---|
| **6 divisions × 6 slots.** Map 0x8032E898 {div, slot} ×31. Grid s32[6][6] at 0x8033BB00 (0x90 bytes; 0x8033BB90 is used by other menus). Stride 24 as `sll/addu/sll` in FDEF4, FE29C, FE75C, FE950, FEA74. | build loops: 802FBA50, inline in 802FBBE4 and 802FD094 ×2; readers: FDEF4, FE29C, FE75C, FE950, FEA74 | **R.** Once those 5 readers are host code, the grid is irrelevant; the build loops stay and write vanilla data harmlessly. A [8][4] grid in place would need about 25 stride patches and still collide with other users. | 5 R (shared with rows below) |
| **Conference = div < 3** (flags 0x400 / 0x600) | 802FDEF4 0x802FE1A4/0x802FE1A8 | R (builder) | – |
| **Schedule format.** 21 week blocks × 0x64 at 0x8032E010, pointer table 0x8032E844, 15 games + bye list at +0x4C; playoff weeks use div 6/7 = AFC/NFC seeds | readers FDEF4, FE29C (via 0x8032E844), FE75C, FE950 | Stage A: D plus 2 P (S32: byes at +0x50). B: R (host schedule; 22 weeks can't fit before 0x8032E844 anyway) | – |
| **Standings** (skip slot 5 of div 0, sort 6 only for div 1, 17 schedule entries) | 802FEA74 | R | – |
| **3 division winners + 3 wildcards; pools of 16/15** | 802FEEC4 | R | – |
| **Comparator / tiebreakers** | 802FE880 | R (only host callers remain) | – |
| **21 weeks / 17 regular** (week constants) | 802FBBE4: 0x802FBCB0 (≤16 regular), 0x802FBFA4 (=16 → seeding), 0x802FC0FC/04/0C/20/28 (17-20 dispatch; **3 in delay slots**). 802FBAAC: 0x802FBB6C, 0x802FBB90. main 0x8024B80C, 0x8024B834 (VS banner), 0x802599BC. 803010C0: 0x80301128, 0x80301134, 0x803011E4 and 0x803012E8 (**delay slots**). 803019A0: 0x80301C3C, 0x80301C9C, 0x80301CE0, column split 0x80301AC0/0x80301ADC. 80300BA4: 0x80300CF0, 0x80300E50 | 802FBBE4 and 802FBAAC: R (B). The rest: H (delay-slot ones handled at the branch target or the callee). 80300BA4: R (stack array, §1.3) | ~13 H, 3 R |
| **Week names (21)** | table 0x8032EC50; readers 0x803016C8/D0, 0x80301C80/88 | H: after the `lw`, `a3 = adv_week_name[week]` (table in the 0x80400000 mirror) | 2 H |
| **15 games per week** | 802FBBE4 0x802FBF70; FE75C 0x802FE854; FE950 0x802FEA48; 80300BA4 0x80300E40; 80301604 0x80301960; 803019A0 0x80301C30 | A: P/H (S32 list). B: driver, schedules and rating are R; 2 screen sites H | 2 H |
| **Results row stride 47** (`(w*3<<4)-w`; `addiu 0x2F`) and direct score reads | record FE29C; builder FDEF4; FE75C/FE950 (`addiu t6,0x2F`); CRC FBAAC (len 0x2F); **week results screen 80301604 reads the scores straight from the record**: 0x803017BC/C4, 0x803017E8/F0, 0x803018C4/CC, 0x803018F0/F8 | A: keep the S32 overflow trick. B: results are host-owned, and the screen is pointed at the builder output it already fetched (0x802F9508 + 6g + 2/3). That is 4 H (one per load) or 8 P | 4 H |
| **Team count 31** (loops, clamp ≤ 30, rand 31) | S32 list: FBA50, FBAAC ×3, FBBE4, FD094 ×6, FFEC0 ×4, tp_team_count_for_mode 0x803015CC | A: as S32 (FBB38 and FD10C are delay slots: R of FBAAC; for FD10C, a hook before 0x802FD110). B: FBAAC is R; clamp and leaders H | A: ~24, B: ~9 H |
| **Stats f32[31][7] at +4; team 31 has no room** | writer FE29C, readers FFEC0 (base 0x802EF6F4), 80300BA4 | A: S32 H4a-d redirect team 31 to 0x802EFDF4 (+ the mandatory H5 zero). B: core layout v2 has stats[32][7] at +4..+0x384 contiguous; highs and results move to the ADV block; leaders unchanged apart from the count | A: 5 H, B: 0 |
| **Standings screen: 6 menu rows, 6th team row only for division 1** | 802FF270: menu loops 0x802FF4AC, 0x802FF6EC; clamp 0x802FF6C0/0x802FF6C8 (**delay slot** → hook at target 0x802FF6CC); row rule 0x802FF538 (s5 = 0 → no skip); rows 0x802FF660 (6 → 4); tables via s4 0x802FF348/4C and positions 0x802FF444/50/60; strings "NORTH"/"SOUTH" | H + D (tables in the mirror). Layout: y positions for 4 + 4 entries must fit around the AFC/NFC headers at y = 285 and 165. Needs a screenshot iteration (U) | ~9 H |
| **Week results screen: 15 rows, 15 px step from y = 264** | 80301604 0x80301960, step 0x8030190C/0x80301954 | H (16 rows); optional 14 px step (S32 optional) | 1-3 H |
| **User schedule: 2 columns (0-9, 10-20), BYE text only for week < 17** | 803019A0 | H (split 10 → 11, 21 → 22, 17 → 18) | 5 H |
| **Playoff bracket** (WC 3v6, 4v5; div [s1, s2, w, w] with re-seed by sim score; conf; SB). Fixed record offsets 0x802EFE48..0x802EFE61 | 802FBBE4 0x802FC13C..0x802FCE3C (inline, 4 + 4 + 2 + 1 sim calls); builder FDEF4 | B (6 seeds): unchanged logic, only the seeding R. **C (7 seeds): R of 802FBBE4** (or of its playoff branches) plus the host builder | C: 1 R |
| **Season rating** (21 weeks, stack array) | 80300BA4 | R | 1 R |
| **Message screens** (ids 0-9) | 8030006C + wrappers | reuse as is. C may want a "first-round bye" variant: text or image (U) | 0-2 |
| **Defeated-teams mask counts 31 bits** (records, Arcade) | main `func_8024A884` | H (optional, any mode) | 1 |

**Data that Advanced adds** (host tables, built from franchises via content_pack_tp's recognition):
- the 8×4 alignment;
- a 2026 schedule (18 weeks, ≤ 4 byes per week; or the real NFL 2026 schedule with the host builder dropping surplus bye rows);
- week name strings;
- division names and positions.

The real NFL schedule is public, factual data; the same sourcing policy as the rosters applies.

---

## 5. Stage design

### 5.1 Stage A: Advanced saves + 32 teams in the old 6 divisions

**Season logic** is the prior Season-32 plan as written, with these corrections:
- **H5 is mandatory** (§1.2): zero 0x802EFDF4..0x802EFE0F after the reset, then write the marker.
- **Dropping the index write at 0x802FE30C** changes the srand input (§1.1). Accept it. In Advanced, replace `802FBAAC` with a host version (R): 32 teams, explicit seed = season seed ⊕ week. That also absorbs 3 of the S32 sites, including the delay-slot `rand_mod(31)` at 0x802FBB38.
- **No S31 legacy mode** inside Advanced: HL1 never lets a foreign save load. Modern keeps team+'s 31-team season, untouched.
- **Schedule:** generated, ≤ 4 byes per week (`gen_schedule.py` already validates 16 games per team, 8 home and 1 bye; add the ≤ 4 rule).

**Save:** §3 hooks HL1, HL2, HS1, HS2, HN1. The sidecar stores the core record plus the season seed. The ADV block is still almost empty.

**Patch count:**
- 26 S32 instruction sites. As runtime hooks that is about 22 H + 1 R (FBAAC). As static patches for a separate build, 26 P.
- 7 S32 hooks (H0-H5, without H6).
- 5 save hooks.
- About 450 lines of host code: `adv_save.cpp` about 300, the S32 data writer about 150.

**Throwaway in B:** about 15 of the S32 sites live in functions that B replaces (FBA50, FDEF4, FE29C, FE75C, FE950, FEA74, FEEC4, FBBE4). The save layer, the screen hooks and the leaders and sim clamp hooks carry over.

### 5.2 Stage B: modern divisions (8×4), 18 weeks / 17 games, 6-seed bracket

The 2002-2019 NFL format (4 division winners + 2 wildcards; seeds 1-2 bye; WC 3v6 and 4v5) is **exactly vanilla Blitz's bracket shape**. So B changes the league structure without touching playoff logic, and C changes only the playoffs.

**Host season engine** (`src/game/adv_season.cpp`, BLITZ_TEAMPLUS only). It is a pure C++ model over `AdvSeason`:
- alignment[8][4] (team indices);
- schedule[18][≤16];
- results[22][16] {scoreA, scoreB, played};
- stats[32][7] mirrored into core +4;
- highs;
- seeds[2][7];
- bracket arrays;
- seed.

**Entry-hook replacements (R)**, each keeping the vanilla output format so the screens keep working:

| Function | Host version | Lines (est.) |
|---|---|---|
| 802FBA50 grid build | no-op | 5 |
| 802FBAAC CPU skill / seed / playoff ramp (week ≥ 18) | port; 32 teams | 40 |
| 802FDEF4 week builder (week, out) | 6-byte entries {A, B, sA, sB, flags 0x600/0x400/0x80}; playoff weeks 18-21 from seeds; ≤ 18 entries | 90 |
| 802FE29C record result (week, game, simbuf) | results, running averages (match float order), highs | 140 |
| 802FE75C, 802FE950 team schedule | 18 entries (17 games + bye) into the caller's buffer. **Only host callers remain**: the 18-entry size is safe only because the stack buffers of 802FEA74 (sp+0x100, 104 bytes) and 802FE29C are no longer used | 50 |
| 802FEA74 standings (out, div) | 4 rows per division, 0x28 entries, sorted | 110 |
| 802FE880 comparator | port (keep the unsigned-diff quirk for parity; real NFL tiebreakers optional later) | 30 |
| 802FEEC4 seeding | 4 division winners + 2 WC per conference → 0x802EFE48 / 0x802EFE4E | 70 |
| 80300BA4 season rating | port with 22 weeks (it draws nothing itself; it calls 80300FA0 for display) (U: confirm) | 120 |
| **802FBBE4 week driver** | port (§5.3). Recommended in B so that week constants, 16 games and the C changes live in one host function | 450 |

**Screens (H), about 40 hooks:**
- standings screen, 9;
- week results, 4 + 1 + 2 (names);
- user schedule, 5 + 2;
- week browser, 4;
- VS banner and menu, 3;
- leaders, 4;
- sim clamp, 4;
- team count, 1.

**Core record layout v2:**

| Offset | Content |
|---|---|
| +0 | team |
| +1 | week (0..21; 0xFF none) |
| +4..+0x384 | stats[32][7] (MIPS readers: leaders, rating) |

The rest is host-defined; results, highs and bracket live in the sidecar. HN1 zeroes stats after the reset (the 0xFF fill hits +0x37F..).

### 5.3 Porting `802FBBE4` (the week driver), needed by C and recommended in B

**Structure (S):**
1. Build the grid.
2. Week builder → 0x802F9508.
3. CPU skill.
4. For week ≤ 16: for each of 15 entries, skip byes and the user's opponent.
5. Call `802FD094(teamA, teamB, simbuf 0x802EA1D0, s2 = difficulty from func_8025A26C / func_8025F098, stack: skillA, skillB from team block +0xE71)`.
6. A difficulty-dependent score fudge (+3 / +7 / +10 / +14, 0x802FBE58..0x802FBF34).
7. `802FE29C`.
8. Abort checks on 0x8029BFC4 (-0x403C) and 0x8029BFD8 (-0x4028).
9. Message screens.
10. At week 16 → seeding, then the eliminated / made-playoffs messages.

Playoff weeks 17-20 are inline blocks. Each block:
- sims the round's games (4 / 4 / 2 / 1);
- writes the next round's arrays from the sim scores in simbuf;
- shows messages;
- recurses (`802FBBE4` calls itself to advance weeks; flag 0x8032E920).

**Host port notes:**
- Calls into recompiled functions are plain C calls (`ovl_b_func_802FD094(rdram, ctx)`; generated code already calls them like this).
- The host must:
  1. set ctx->r4..r7;
  2. reserve a MIPS stack frame (ctx->r29 -= 0x20) for the two stack arguments at sp+0x10/+0x14;
  3. restore ctx->r29 after the call.
- Callee-saved registers are preserved by the MIPS callees.
- The user's game is played inside `802FD094`; the host frame simply waits.

### 5.4 Stage C: 7-seed playoffs
- **Seeding:** 7 per conference (4 division winners by record, 3 wildcards).
- **Wildcard week (18):** 2v7, 3v6, 4v5 per conference (6 games); seed 1 has a bye.
- **Divisional (19):** 1 vs the lowest remaining seed; the other two play each other.
- **Then:** conference (20) and Super Bowl (21).
- **Changes:**
  - host driver playoff section;
  - builder playoff weeks;
  - sidecar format version +1;
  - message ids: reuse "made playoffs" / "bye" (U, look at ids 0-9 in a run).
- The week browser already handles 6 rows. Week names: "Wild Card", "Divisional", "Conference", "Super Bowl".
- About 300 lines of host code, plus data.

---

## 6. Runtime mode vs a separate build

Advanced must not change Modern: Modern is team+-compatible, 31-team season, pak format.

**Option 1, one 2026 binary with a mode setting (recommended):**
- every Advanced change is an R entry hook or a conditional H hook (`if (blitz_adv_on())`);
- no static instruction patches for Advanced.

What the site check (`dslot.py`, `hooksite.py`) shows:
- most sites are not in delay slots;
- the few that are (0x802FBB38, 0x802FD10C, 0x802FC104/0C/28, 0x802FF6C8, 0x803011E4, 0x803012E8) fall inside R functions or get a hook at the branch target or next instruction.

Rule: an H hook goes **before the next non-delay-slot instruction** and recomputes the register the patched instruction produced, e.g. `ctx->r2 = (int32_t)ctx->r20 < 16`. N64Recomp emits a hook ahead of the instruction, including ahead of a branch's condition (U: grep the generated C once, as was done for the dma_read hook).

**Option 2, a third app flavour (BLITZ_ADVANCED, own N64Recomp config):**
- static patches are allowed, and it is simpler to reason about;
- the cost is another 5-minute recompile, another bundle and another config to keep in sync.

Use it if Option 1's hook count becomes unwieldy.

The mode must be fixed at launch (restart to switch). The pak folder and sidecars follow the mode.

---

## 7. Effort per stage (agent-days; the team's play-testing extra)

| Stage | Host code | Hooks / patches | Data / tools | Days |
|---|---|---|---|---|
| A | ~450 lines | 5 save + 7 S32 + ~23 sites | schedule ≤ 4 byes | 3-4 |
| B | ~1,500 lines | ~11 R + ~40 H | 8×4 alignment, 18-week schedule, strings, parity harness | 8-12 |
| C | ~300 lines | 0-2 | week names, messages | 2-4 |

---

## 8. Risks (ranked)

1. **Port fidelity** (802FE29C averages, 802FEA74/FE880 sort order, 802FBBE4 fudge and recursion, 80300BA4).
   - `func_800269B0`'s qsort order with ties must be matched, or standings differ.
   - Mitigation: shadow parity mode (§10.1) before switching anything on.
2. **Save consistency:** two-phase commit, torn pak notes, orphans, generation bookkeeping, pack-order remap.
3. **Buffer limits baked into screens:**
   - 18-entry builder output (byes ≤ 4);
   - 16 week-result rows (step);
   - 22 weeks in two schedule columns of 11;
   - 8 standings menu entries around fixed header images (layout iteration).
4. **Modern regressions** from shared hooks: the flag must gate everything; byte-identical Modern behaviour test.
5. **Host ↔ recompiled calls** (stack frame for stack arguments, re-entrancy across ovl_a/ovl_b reloads, process sleeps). Low risk: vanilla already nests identically.
6. **Hook mechanics:**
   - a hook at a branch runs before the condition;
   - an entry hook with `return` (U until the generated C is grepped).
7. **Unknowns left (U):**
   - schedule flag bits 1/2/4/0x20;
   - record +2/+3;
   - 80300BA4's display path;
   - who displays season highs (only the writer and the reset reference 0x802EFA58, so a screen must take it through a pointer).

---

## 9. What stays out of scope or optional
- **NFL tiebreakers:** head-to-head, common games, strength of victory.
- **Multi-season and franchise carry-over:** the sidecar format allows it.
- **Real 2026 schedule import** (public, factual).
- **Records screen and Arcade** defeated-teams 32-bit fix (`func_8024A884`).
- **Announcer banks** for renamed teams (separate track).

---

## 10. Test plan (muted, separate data folder, never `build/appdata`; check `pgrep -fl BlitzRecompiled` first)

### 10.1 Before enabling anything: shadow parity (Stage B prerequisite)
- `BLITZ_ADV_SHADOW=1` runs the vanilla 31-team season.
- An entry hook on each replaced function:
  1. snapshots the inputs and runs the **host** version into a scratch copy;
  2. lets the MIPS run;
  3. an exit hook (before `jr ra`) compares the outputs: builder entries, standings 0x28 entries, seeds, record bytes, stats floats (bit-exact; or ULP-tolerant, logged).
- The host engine is configured as "vanilla league" (6 divisions, 31 teams, 21 weeks) for this.
- Run a full autosimmed season (S32's dev hook `BLITZ_S32_AUTOSIM`-style at 0x802FD150) with 0 mismatches.

### 10.2 Offline (host unit tests, no game)
- **Schedule validator:**
  - 17 games per team;
  - 8 or 9 home games;
  - 6 division games;
  - 1 bye in weeks 5-14;
  - ≤ 4 byes and ≤ 16 games per week;
  - ≤ 18 builder entries.
- **Seeding and bracket cases:** ties, the unsigned-differential quirk, 7-seed re-seeding.
- **Sidecar:** round-trip and CRC; torn and orphan cases; franchise remap.
- `make test`.

### 10.3 Stage A in game
```
make app2026
BLITZ_FLAVOUR=2026 BLITZ_DATA=$PWD/build/advdata tools/live.sh start
```
(plus the Advanced mode setting)
- Season → NEW: 32 teams; pick Houston.
- The schedule screen shows 16 opponents and BYE.
- Dump: team-31 stats are finite after week 1, which proves H5.
- **Save:**
  - the pak dump has a stub (+0 = FF, magic, gen);
  - `saves/advanced/.../season/slotN-*.adv` exists;
  - sidecar CRC is ok.
- Quit, relaunch, LOAD: the slot list shows "HOUSTON … WEEK 2"; the core record dump is identical to the pre-save dump.
- **Fault injection** (`BLITZ_ADV_FAULT=after_sidecar|pak_fail`): relaunch loads the previous gen.
- **Foreign pak:** copy the Advanced pak into a Classic data folder → Classic shows the slot empty, no crash. Copy a Classic pak into Advanced → its season slot is shown empty and preserved byte-exact after an Advanced save to another slot.
- **Regression:** Modern (mode off) season save/load is byte-identical to today; Classic build unchanged (existing check).

### 10.4 Stage B in game
- Standings show 8 divisions × 4 (screenshot layout check).
- Week results show 16 rows.
- User schedule shows 18 regular weeks + 4 playoff weeks.
- Autosim to week 17 → seeds: 4 division winners + 2 WC per conference, all from the right conference.
- Bracket 3v6 / 4v5 → divisional re-seed → Super Bowl.
- Season rating screen renders.
- Save and load at weeks 0, 9, 17, 19, 21.

### 10.5 Stage C in game
- 7 seeds: 6 WC games in week 18 (week browser shows 6 rows); #1 has a bye.
- Divisional: 1 plays the lowest remaining seed.
- Sidecar version migration (a B save loaded in C: migrate before the playoffs or refuse with a notice).

---

## 11. Questions for the team
1. **Advanced as a runtime mode of the 2026 app, or a third app?** §6 recommends the runtime mode.
2. **Schedule:** generated each new season (seeded), or the real NFL 2026 schedule (fixed)?
3. **Tiebreakers:** keep vanilla Blitz's (with its quirk), or NFL rules?
4. **Should Modern stay team+'s 31-team season** (Houston not in Season) for emulator save compatibility, with the 32-team season only in Advanced? This note assumes yes.

---

## 12. Scratch tools in this folder (analysis only)

| File | What it does |
|---|---|
| `hiscan.py LO HI [LUIHI]` | indexed `lui; addu; op off(at)` references into a range (the lui-pair scanner misses these) |
| `fnof.py` | maps addresses to their containing function (team+ syms) |
| `rd.py SEG VRAM N [w\|s\|b]` | reads team+ ROM data and string tables by VRAM |
| `dslot.py ADDR..` | is a site in a delay slot? |
| `hooksite.py ADDR..` | delay slot and branch-target check for hook sites |
| `imms_season.py FUNCS` | season-relevant immediates per function |
| `record_refs.txt` | all 288 record-access instructions |
| `refs_season_bss.txt` | references to the schedule/division data and the season bss (0x8032E000.., 0x8033B600..) |

The full-text disassembly with refs is reused from `../next2026/verify32/tp_all.txt`.
