# Announcer speech banks

This document covers how NFL Blitz 2000 stores the announcer's speech and how to build a new team bank. Findings are from 2026-10-16, using vanilla v1.1 addresses. team+ has byte-identical sound files and bank code.

Full research notes, scripts and decoded clips are local only, in `build/analysis/announcer/` (they are game data).

## Files

The speech is standard libmus. Each bank is three files in the asset file system:
- **`.wbk`** ("N64 WaveTables"): a 16-byte header followed by raw VADPCM. It is stored uncompressed and streamed from ROM, never loaded into RAM.
- **`.ptr`** ("N64 PtrTablesV2"): a wave count and pointers to basenotes (1 byte each), detunes (4 bytes each) and a wave list. Each wave entry is {base, len, type 0 = ADPCM, loop 0, book}, followed by its codebook {order 2, 4 predictors, 2×8 s16 each}. All pointers are file-relative.
- **`.bfx`**: an effect bank. It holds counts, a wave table (u16 per wave) and per-effect {offset, priority}. Every team effect is the same script, `81 ww 84 01 7f 01 7f 01 7f 10 9c 7f a6 VV 30 LL 80`: wave, envelope, pan, volume VV, note 0x30 for LL ticks (about 1/96 s each; bit 7 set means a 2-byte length), stop. An empty effect is a single `80`.

**Playback rate:** 22050 × 2^(signed basenote / 12). Team banks use basenote 0xF0, which gives 8751 Hz.

## Team banks

**Naming:** a team bank is named after the first 8 characters of the team's nickname (team record +0x10), lowercased: `cardinal`, `buccanee`, …

**Shape:** all 31 vanilla team banks have 15 waves and 21 effects, all at 8751 Hz.

| effect | content | played by |
|---|---|---|
| 0 | city (probable) | in-play calls (ovl_a 0x80304E04, 0x80305858, 0x80305DE8) |
| 1 | nickname (probable) | score or lead calls (ovl_a 0x8030456C / 0x80304598) |
| 2 | full name | VS screen, first team (main 0x8024C1C4) |
| 3 | full name, the longer reading | VS screen, second team (main 0x8024C24C); also the id+3 path for id 0 |
| 4-9 | empty | the player-id path (ids 4-7) plays these, so it never speaks |
| 10 | short call (a player?) | long-run countdown (ovl_a 0x8030575C); id 7 + 3 |
| 11-14 | player surnames | the id+7 path for ids 4-7 (ovl_a 0x803054DC) |
| 15-20 | short calls | nothing in v1.1 |

**Player call ids** (player record: vanilla +0x18 u32, team+ +7 u8):
- vanilla sets 4 for the QB, 5-7 for slots 4-6, and 0 for everyone else;
- id 0 plays the team's full name (effect 3) or nothing (effect 7), never another player's name;
- content packs set renamed players to 0.

## Loading

1. `func_8027BF50(name)` looks the name up in an alias table of 13 entries (tkbank/taunts sets, songs whose voices live in generic, players or gameover). No team is aliased.
2. `func_80005318` opens `<name>.ptr`, `.wbk` and `.bfx` through the file system, by name.
3. If the `.ptr` or `.wbk` is missing, the handle is −1 and every play of that bank is silently skipped. That is why Texans ("texans") and Commanders ("commande") are silent unless you install your own calls (below).
4. A loaded bank is matched to its slot by the `.wbk`'s ROM address. There are 16 slots.

**Memory:** the `.ptr` and `.bfx` come from the heap. Keep a new bank to 15 waves or fewer (2584 / 486 bytes).

## A new team bank

**Clips:**
- **Required:**
  - effect 0: city;
  - effect 1: nickname;
  - effect 2: full name;
  - effect 3: full name with emphasis.
- **Optional:** effects 11-14, the surnames for player ids 4-7.

**Rate:** 8751 Hz matches the rest of the announcer.

**Encoding:** VADPCM, order 2, 4 predictors, one codebook per wave. The codebook is designed from the clip itself.

**VS screen:** after team 1's full name, the screen waits a per-team time before saying team 2's, on the same voice: the byte at team block +0x1D6, about 0.54 frames per tick of effect 2 in every original bank. A longer call would be cut off, so Modern Blitz sets that byte for each replaced bank from its .bfx (0.56 × ticks + 2; `announcer_vs_pause` in art_dropin.cpp).

**Installing:** the bank files are added to the asset file system by name, at the ROM end. This works the same way as the art drop-in's single-file overrides (`src/game/art_dropin.cpp`), plus a lookup fallback for names the game lacks (below).

## Making your own calls (Modern Blitz)

**Easiest: Announcer Studio.** `.venv/bin/python tools/announcer_studio.py` opens a local page (127.0.0.1 only, so the browser allows the microphone). It plays the game's own four calls for any team as examples (decoded from your ROM, never saved), records takes, keeps the one you pick, builds the bank with the steps below, plays your calls back as the game will, and installs them into the dev app's folder (`--app-folder` for another). Takes and banks stay in `build/announcer/`.

**By hand:**

1. **Record** your own takes as WAV, AIFF or M4A files, at any rate, mono or stereo:
   - the city ("Houston");
   - the nickname ("Texans");
   - the full name twice: once neutral, once hyped.

   Use short, punchy shouts. Use only your own voice, or one you have the rights to; never the game's announcer.
2. **Build** the bank. The name is the nickname's first 8 letters, lowercase:
   ```
   .venv/bin/python tools/announcer.py build texans out/ --city city.wav --nickname nick.wav \
       --full full.wav --full2 full_hyped.wav --rom <your team+ ROM>
   ```
   With `--rom`, each clip is trimmed, band-limited and compressed, then matched to the tone and loudness of the game's own calls. `--like <bank>` picks which team's calls to match.
3. **Compare** your call with the game's: `tools/announcer.py preview <ROM> texans out/`.
4. **Install:** copy `texans.wbk`, `.ptr` and `.bfx` into the app's `announcer` folder (launcher → Custom Content opens the app folder). They load at the next start, either as a new team's calls or replacing an existing team's. Export ROM carries them too: a bank the game already has is replaced, and a new one (texans, commande) is added to the cart's file list with a small lookup routine so a real N64 finds it (`tools/rom_picker/lookup.asm`; verified 2026-10-18 in mupen64plus, both banks loaded at the VS screen).

**How the app installs them:**
- Files go after the ROM end, as with the art drop-in. Existing names are repointed.
- New names stay out of the game's table of contents, because images are found by their position in it.
- Hooks in the game's lookup (boot func_80001C2C) find them.

**Verified:** a test `texans` bank loads (bank slot ROM address past 16 MB), and a game runs with it.
