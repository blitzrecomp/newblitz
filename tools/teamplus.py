#!/usr/bin/env python3
"""The 2026 flavour's base ROM and N64Recomp config ("team+": Zoinkity's Team Extension v2.0).

  teamplus.py rom --bps PATH     apply the user's Blitz2k-11_team+.bps to build/baserom.z64
                                 -> build/teamplus.z64 (every CRC/hash checked; nothing is copied
                                 into the repo)
  teamplus.py toml               write build/tp/blitz2000_teamplus.toml: blitz2000.toml with the
                                 2026 paths plus the fixes team+'s new code needs to recompile
  teamplus.py check-code ROM     does ROM run on the 2026 build (same code as team+, any data)?

The fixes (see docs/BLITZ2026.md):
  - mfc0 Count at 0x80249270 / 0x802492A0 (attract-mode team pick seeds rand from it): N64Recomp
    only supports COP0 Status, so the moves become nops and a hook loads the register from
    blitz_cop0_count().
  - tp_count_teams stores the team count through KSEG1 (lui 0xA002); the runtime maps only KSEG0
    RDRAM, so 0x803015F8 becomes lui 0x8002.
  - ovl_a 0x8030656C: team+ retargeted a vanilla announcer table read into ovl_a code (0xE8C);
    restored to the vanilla table (0x796C).
  - main 0x8024C1D0: the VS-screen pause read the high byte of the 32-bit team index, so every
    team used Arizona's value; now the low byte (each team's own +0x1D6).
  - main 0x8024C6B4: team+ computed (right id == left id) but then set the flip flag always; the
    right VS helmet is now mirrored only when the team has no right-facing image of its own.
  - addu/add $zero,$zero,$zero no-ops in team+'s code: N64Recomp emits "0 = ADD32(0, 0)", which
    doesn't compile; replaced with real nops.

Advanced mode (docs/ADVANCED_SEASON.md) and Standard's 32-team Season (src/game/season32.cpp) add
hooks, not instruction patches: Modern must stay team+'s game, so each one does nothing unless
blitz_adv (src/game/adv_mode.cpp) or blitz_s32 (a 32-team season is loaded) is set. A hook runs
before its instruction; one that replaces a compare recomputes the compare's result just before the
branch that uses it. Every site's instruction is checked against the ROM, no hook may sit in a delay
slot, and a site has at most one hook (a site both use has one text that tests both flags).
"""
import argparse
import hashlib
import json
import sys
import tomllib
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BASE_ROM = ROOT / "build" / "baserom.z64"
TP_ROM = ROOT / "build" / "teamplus.z64"
TP_DIR = ROOT / "build" / "tp"
TP_SYMS = TP_DIR / "syms" / "blitz2000_teamplus.syms.toml"
TP_TOML = TP_DIR / "blitz2000_teamplus.toml"
CLASSIC_TOML = ROOT / "blitz2000.toml"
CONFIG = ROOT / "config" / "teamplus.json"


def crc32(data):
    return zlib.crc32(data) & 0xFFFFFFFF


# --- BPS (https://www.romhacking.net/documents/746/) ---------------------------------------

def _number(p, i):
    value, shift = 0, 1
    while True:
        x = p[i]
        i += 1
        value += (x & 0x7F) * shift
        if x & 0x80:
            return value, i
        shift <<= 7
        value += shift


def bps_apply(patch, source):
    if patch[:4] != b"BPS1":
        raise SystemExit("not a BPS patch")
    src_crc, tgt_crc, patch_crc = int.from_bytes(patch[-12:-8], "little"), int.from_bytes(patch[-8:-4], "little"), int.from_bytes(patch[-4:], "little")
    if crc32(patch[:-4]) != patch_crc:
        raise SystemExit("patch file is damaged (patch CRC mismatch)")
    if crc32(source) != src_crc:
        raise SystemExit(f"patch is for a different source ROM (wants CRC32 {src_crc:08X}, ours is {crc32(source):08X})")
    i = 4
    src_size, i = _number(patch, i)
    tgt_size, i = _number(patch, i)
    meta_size, i = _number(patch, i)
    i += meta_size
    out = bytearray(tgt_size)
    o = src_rel = tgt_rel = 0
    end = len(patch) - 12
    while i < end:
        d, i = _number(patch, i)
        cmd, n = d & 3, (d >> 2) + 1
        if cmd == 0:  # SourceRead
            out[o:o + n] = source[o:o + n]
            o += n
        elif cmd == 1:  # TargetRead
            out[o:o + n] = patch[i:i + n]
            i += n
            o += n
        else:
            off, i = _number(patch, i)
            off = -(off >> 1) if off & 1 else off >> 1
            if cmd == 2:  # SourceCopy
                src_rel += off
                out[o:o + n] = source[src_rel:src_rel + n]
                src_rel += n
                o += n
            else:  # TargetCopy (may overlap)
                tgt_rel += off
                for _ in range(n):
                    out[o] = out[tgt_rel]
                    o += 1
                    tgt_rel += 1
    if crc32(out) != tgt_crc:
        raise SystemExit("patched result doesn't match the patch's target CRC")
    return bytes(out)


def xxh3(data):
    import xxhash
    return xxhash.xxh3_64_intdigest(data)


def code_hash(rom, cfg):
    h = b"".join(rom[int(a, 16):int(b, 16)] for a, b in cfg["code_regions"])
    return xxh3(h)


def cmd_rom(args):
    cfg = json.loads(CONFIG.read_text())
    base = BASE_ROM.read_bytes()
    patch = Path(args.bps).read_bytes()
    if f"{crc32(patch[:-4]):08X}" != cfg["patch_crc32"]:
        raise SystemExit(f"{args.bps}: not Zoinkity's Blitz2k-11_team+.bps v2.0 (patch CRC {crc32(patch[:-4]):08X})")
    out = bps_apply(patch, base)
    if f"{crc32(out):08X}" != cfg["target_crc32"] or hashlib.sha1(out).hexdigest() != cfg["target_sha1"]:
        raise SystemExit("result doesn't match the pinned team+ ROM")
    if f"0x{xxh3(out):016X}" != cfg["target_xxh3"] or f"0x{code_hash(out, cfg):016X}" != cfg["code_xxh3"]:
        raise SystemExit("result doesn't match the pinned team+ hashes")
    TP_ROM.write_bytes(out)
    print(f"wrote {TP_ROM.relative_to(ROOT)} (CRC32 {crc32(out):08X}, SHA-1 {hashlib.sha1(out).hexdigest()})")


# --- N64Recomp config -----------------------------------------------------------------------

FIXES = [
    # (func name, vram, new word, comment)
    ("tp_count_teams", 0x803015F8, 0x3C038002, "lui $v1, 0xA002 -> 0x8002: the team count store went through KSEG1"),
    ("ovl_a_func_80306524", 0x8030656C, 0x8CA5796C, "lw $a1, 0xE8C($a1) -> 0x796C: team+ retargeted a vanilla announcer table read into code"),
    ("func_8024C264", 0x8024C6B4, 0x2E100001, "sltiu $s0, $zero, 1 -> $s0, $s0, 1: mirror the right VS helmet only when the team has no right-facing image (team+ always mirrored it)"),
    ("func_8024C174", 0x8024C1D0, 0x9042A437, "lbu $v0, -0x5BCC -> -0x5BC9: the VS-screen pause read the team index's high byte (always team 0)"),
]
# Advanced mode (docs/ADVANCED_SEASON.md): the pack's league in 8 divisions of 4, its 18-week schedule,
# NFL tiebreakers and 7-team playoffs, run by host code (src/game/adv_engine.cpp, adv_season.cpp).
# (section, hook vram, (vram, expected word) the hook stands in for, hook text, comment)
ADV = "if (blitz_adv) "
AS = "if (blitz_adv || blitz_s32) "  # sites Standard's 32-team Season shares (same change)
ENTRY = "if (blitz_adv) { %s; return; }"
ADVANCED = [
    # Season engine: whole functions replaced by host code.
    ("ovl_b", 0x802FBBE4, (0x802FBBE4, 0x27BDFFB0), ENTRY % "blitz_adv_week_driver(rdram, ctx)", "week driver"),
    ("ovl_b", 0x802FDEF4, (0x802FDEF4, 0x27BDFFF0), ENTRY % "ctx->r2 = blitz_adv_build_week(rdram, (int32_t)ctx->r4, (int32_t)ctx->r5)", "week builder"),
    # (division standings 802FEA74: the shared hook in blitz2000.toml, src/game/season_tiebreak.cpp)
    ("ovl_b", 0x802FBAAC, (0x802FBAAC, 0x27BDFFE0),
     ENTRY % "blitz_adv_cpu_skill(rdram, ctx)" + " if (blitz_s32) { blitz_s32_cpu_skill(rdram, ctx); return; }",
     "CPU skill / RNG seed per week (32 teams; Advanced seeds from the season, S32 as the game)"),
    # The game sim (802FD094) is kept: its team-index clamps allow team 31.
    ("ovl_b", 0x802FD0F4, (0x802FD0D0, 0x2BC2001F), AS + "ctx->r2 = (int32_t)ctx->r30 < 32;", "sim: team A index clamp"),
    ("ovl_b", 0x802FD100, (0x802FD0FC, 0x241E001E), "if ((blitz_adv || blitz_s32) && ctx->r2 == 0) ctx->r30 = 31;", "sim: team A clamp value (branch target)"),
    ("ovl_b", 0x802FD108, (0x802FD104, 0x2942001F), AS + "ctx->r2 = (int32_t)ctx->r10 < 32;", "sim: team B index clamp"),
    ("ovl_b", 0x802FD110, (0x802FD10C, 0x2409001E), AS + "ctx->r9 = 31;", "sim: team B clamp value"),
    ("ovl_b", 0x802FD150, (0x802FD150, 0x17C20029), "if (blitz_adv_autosim()) ctx->r2 = 0xFF;", "dev (BLITZ_ADV_AUTOSIM, Advanced and S32): simulate the user's games too"),
    # League leaders: 32 teams; team 31's stats live at record +0x704 (0x802EFDF4).
    ("ovl_b", 0x802FFED8, (0x802FFED8, 0xC4400000), "if ((blitz_adv || blitz_s32) && ctx->r7 == 31) ctx->r2 = (int32_t)(0x802EFDF4u + (uint32_t)ctx->r9);", "leaders: team 31's stats"),
    ("ovl_b", 0x802FFEF0, (0x802FFEEC, 0x28E2001F), AS + "ctx->r2 = (int32_t)ctx->r7 < 32;", "leaders: 32 teams"),
    ("ovl_b", 0x802FFF04, (0x802FFF00, 0x24AB001F), AS + "ctx->r11 = (int32_t)(ctx->r5 + 32);", "leaders: 32 teams"),
    ("ovl_b", 0x802FFF38, (0x802FFF34, 0x28E2001F), AS + "ctx->r2 = (int32_t)ctx->r7 < 32;", "leaders: 32 teams"),
    ("ovl_b", 0x802FFF88, (0x802FFF88, 0xC4400000), "if ((blitz_adv || blitz_s32) && ctx->r3 == 31) ctx->r2 = (int32_t)(0x802EFDF4u + (uint32_t)ctx->r6);", "leaders: team 31's stats"),
    ("ovl_b", 0x802FFFA0, (0x802FFF9C, 0x28E2001F), AS + "ctx->r2 = (int32_t)ctx->r7 < 32;", "leaders: 32 teams"),
    # The list is shown before NEW's season exists (an S31 season may still be loaded): ask whether 32 can play.
    ("ovl_b", 0x803015CC, (0x803015CC, 0x2C610002), "if ((blitz_adv || blitz_s32_capable()) && ctx->r3 == 2) ctx->r3 = 1;", "Season mode lists all 32 teams"),
    # Standings screen: 8 division names and positions, 4 rows each.
    ("ovl_b", 0x802FF350, (0x802FF350, 0x0C09E59C), ADV + "ctx->r20 = blitz_adv_div_names();", "standings: division name table"),
    ("ovl_b", 0x802FF3F8, (0x802FF3F8, 0x0C09DE17), ADV + "ctx->r6 = blitz_adv_nfc_header_y();", "standings: NFC header lower, below 4 AFC rows"),
    ("ovl_b", 0x802FF480, (0x802FF480, 0x0C09E59C), ADV + "blitz_adv_div_menu_pos(ctx);", "standings: division menu positions"),
    ("ovl_b", 0x802FF4B0, (0x802FF4AC, 0x2A220006), ADV + "ctx->r2 = (int32_t)ctx->r17 < 8;", "standings: 8 division menu entries"),
    ("ovl_b", 0x802FF4E8, (0x802FF4E8, 0xAFA80144), ADV + "ctx->r8 = (int32_t)(ctx->r29 + 0x40);", "standings: entries after 8 menu handles"),
    ("ovl_b", 0x802FF528, (0x802FF528, 0x0C0BFA9D), ADV + "ctx->r4 = (int32_t)(ctx->r29 + 0x40);", "standings: entries after 8 menu handles"),
    ("ovl_b", 0x802FF664, (0x802FF660, 0x2A420006), ADV + "ctx->r2 = (int32_t)ctx->r18 < 4;", "standings: 4 team rows"),
    ("ovl_b", 0x802FF6C4, (0x802FF6C0, 0x2A620006), ADV + "{ if ((int32_t)ctx->r19 > 7) ctx->r19 = 7; ctx->r2 = 1; }", "standings: division cursor 0-7"),
    ("ovl_b", 0x802FF6F0, (0x802FF6EC, 0x2A220006), ADV + "ctx->r2 = (int32_t)ctx->r17 < 8;", "standings: unhighlight 8 entries"),
    # Schedule browser, week results, team schedule: 32 teams, 22 weeks, 16 games.
    ("ovl_b", 0x8030113C, (0x8030113C, 0x3C048034), ADV + "{ int w_ = MEM_BU(0, (int32_t)0x802EF6F1); MEM_W(0x2C, ctx->r29) = w_ < 21 ? w_ : 21; }",
     "browser: start at the current week (branch target)"),
    ("ovl_b", 0x80301448, (0x80301448, 0x0062102A), ADV + "ctx->r2 = ctx->r18 == 0 ? 31 : 21; else if (blitz_s32 && ctx->r18 == 0) ctx->r2 = 31;",
     "browser: last team / week (S32: team 31; its table says 30)"),
    ("ovl_b", 0x803016D4, (0x803016D0, 0x8CE7EC50), ADV + "ctx->r7 = MEM_W(0, blitz_adv_week_name((int32_t)ctx->r19));", "week results: week name"),
    ("ovl_b", 0x803017C8, (0x803017C4, 0x9063FA71), ADV + "ctx->r3 = MEM_BU((int32_t)0x802E950B, ctx->r17);", "week results: score from the builder entry"),
    ("ovl_b", 0x803017F4, (0x803017F0, 0x90E7FA71), ADV + "ctx->r7 = MEM_BU((int32_t)0x802E950B, ctx->r17);", "week results: score from the builder entry"),
    ("ovl_b", 0x803018D0, (0x803018CC, 0x9063FA70), ADV + "ctx->r3 = MEM_BU((int32_t)0x802E950A, ctx->r17);", "week results: score from the builder entry"),
    ("ovl_b", 0x803018FC, (0x803018F8, 0x90E7FA70), ADV + "ctx->r7 = MEM_BU((int32_t)0x802E950A, ctx->r17);", "week results: score from the builder entry"),
    ("ovl_b", 0x80301964, (0x80301960, 0x2A82000F), AS + "ctx->r2 = (int32_t)ctx->r20 < 16;", "week results: 16 rows"),
    ("ovl_b", 0x80301958, (0x80301954, 0x2652FFF1), AS + "ctx->r18 = (int32_t)(ctx->r18 + 1);",
     "week results: 14 px rows, so 16 fit above the footer (both paths meet here)"),
    ("ovl_b", 0x80301AF8, (0x80301AF8, 0x01099023), ADV + "blitz_adv_schedule_column(ctx);", "team schedule: 11 + 11 weeks per column"),
    ("ovl_b", 0x80301C34, (0x80301C30, 0x2AA2000F), AS + "ctx->r2 = (int32_t)ctx->r21 < 16;", "team schedule: 16 games a week"),
    ("ovl_b", 0x80301C40, (0x80301C3C, 0x2A820011), ADV + "ctx->r2 = (int32_t)ctx->r20 < 18;", "team schedule: 18 regular weeks (week name)"),
    ("ovl_b", 0x80301C8C, (0x80301C88, 0x8CE7EC50), ADV + "ctx->r7 = MEM_W(0, blitz_adv_week_name((int32_t)ctx->r20));", "team schedule: week name"),
    ("ovl_b", 0x80301CA0, (0x80301C9C, 0x2A830011), ADV + "ctx->r3 = (int32_t)ctx->r20 < 18;", "team schedule: 18 regular weeks (bye)"),
    ("ovl_b", 0x80301CF0, (0x80301CE0, 0x2A820015), ADV + "ctx->r2 = (int32_t)ctx->r20 < 22;", "team schedule: 22 weeks"),
    # VS banner and season header.
    ("main", 0x8024B810, (0x8024B80C, 0x2CE20011), ADV + "ctx->r2 = (uint32_t)ctx->r7 < 18;", "VS banner: 18 regular weeks"),
    ("main", 0x8024B83C, (0x8024B834, 0x2CE20014), ADV + "ctx->r2 = (uint32_t)ctx->r7 < 21;", "VS banner: Super Bowl is week 22"),
    ("main", 0x802599C0, (0x802599BC, 0x2C420015), ADV + "ctx->r2 = MEM_BU(0, (int32_t)0x802EF6F1) < 22;", "season header: season over after week 22"),
    ("main", 0x80263AEC, (0x80263AEC, 0x8FBF0014), ADV + "blitz_adv_new_season(rdram); else blitz_s32_new_season(rdram);",
     "season reset: Advanced state, or S32/S31 for Standard"),
    # Saves (src/game/adv_save.cpp): the save/load screen, main func_802679B8.
    ("main", 0x80267B84, (0x80267B84, 0x00408021), ADV + "blitz_adv_slots_read(rdram, (int32_t)ctx->r2, (int32_t)ctx->r21, MEM_W(0x34, ctx->r29));",
     "slots read: show Advanced seasons, hide other formats"),
    ("main", 0x80267F48, (0x80267F48, 0x0C098D93), ADV + "blitz_adv_before_load(rdram, (int32_t)ctx->r18, (int32_t)ctx->r19);",
     "LOAD: the sidecar's record into the slot buffer"),
    ("main", 0x8026807C, (0x8026807C, 0x0C097D3E), ADV + "blitz_adv_before_write(rdram, (int32_t)ctx->r18, MEM_W(0x34, ctx->r29));",
     "SAVE: sidecar written, stub into the slot"),
    ("main", 0x8026809C, (0x8026809C, 0x0809A048), ADV + "blitz_adv_after_write(rdram, (int32_t)ctx->r2, MEM_W(0x34, ctx->r29));",
     "SAVE: after the pak write"),
]


# Standard Season with 32 teams (src/game/season32.cpp): team+'s own season code in the six 1999-style
# divisions with Houston, widened from 31 teams / 15 games a week to 32 / 16 while blitz_s32 is set (a
# 32-team season is loaded). Tested first as Advanced Stage A (8d95ad1). The sites both modes need are
# in ADVANCED above with AS texts; these are Standard's alone (Advanced replaces their functions).
S32 = "if (blitz_s32) "
SEASON32 = [
    ("ovl_b", 0x802FBA9C, (0x802FBA98, 0x28C2001F), S32 + "ctx->r2 = (int32_t)ctx->r6 < 32;", "division grid build, 32 teams"),
    ("ovl_b", 0x802FBC6C, (0x802FBC68, 0x28C2001F), S32 + "ctx->r2 = (int32_t)ctx->r6 < 32;", "week driver grid build, 32 teams"),
    ("ovl_b", 0x802FBF74, (0x802FBF70, 0x2A02000F), S32 + "ctx->r2 = (int32_t)ctx->r16 < 16;", "week driver: 16 games a week"),
    ("ovl_b", 0x802FD1E0, (0x802FD1DC, 0x28C2001F), S32 + "ctx->r2 = (int32_t)ctx->r6 < 32;", "sim: grid rebuild"),
    ("ovl_b", 0x802FD294, (0x802FD290, 0x28C2001F), S32 + "ctx->r2 = (int32_t)ctx->r6 < 32;", "sim: grid rebuild"),
    ("ovl_b", 0x802FE234, (0x802FE234, 0x8464004C), S32 + "ctx->r3 = (int32_t)(ctx->r3 + 4);", "week builder: bye list at +0x50 (both lh)"),
    ("ovl_b", 0x802FE30C, (0x802FE30C, 0xA025FA6F), S32 + "ctx->r1 = (int32_t)(0x80400000u + 0x591u);",
     "record result: the per-game index byte goes to scratch (game 16's score uses that byte)"),
    ("ovl_b", 0x802FE414, (0x802FE414, 0x3C01802F), "if (blitz_s32 && ctx->r16 == 31) ctx->r3 = 0x700;", "record result: team 31's stats at 0x802EFDF4"),
    ("ovl_b", 0x802FE858, (0x802FE854, 0x2942000F), S32 + "ctx->r2 = (int32_t)ctx->r10 < 16;", "team schedule, 16 games"),
    ("ovl_b", 0x802FEA4C, (0x802FEA48, 0x2942000F), S32 + "ctx->r2 = (int32_t)ctx->r10 < 16;", "team schedule, 16 games"),
    ("ovl_b", 0x802FEB18, (0x802FEB18, 0x546000B8), S32 + "ctx->r3 = blitz_s32_div_size((int32_t)ctx->r18) <= (int32_t)ctx->r17;",
     "standings: skip the slots a division doesn't have"),
    ("ovl_b", 0x802FEE20, (0x802FEE20, 0x02002021), S32 + "ctx->r5 = blitz_s32_div_size((int32_t)ctx->r18);", "standings: sort the division's teams"),
    ("ovl_b", 0x802FF1D0, (0x802FF1CC, 0x2405000F), S32 + "ctx->r5 = 16;", "seeding: NFC wildcard pool of 16"),
    ("ovl_b", 0x802FF53C, (0x802FF53C, 0x3C014000), S32 + "ctx->r21 = blitz_s32_div_size((int32_t)ctx->r19) < 6;", "standings screen: 6th row for 6-team divisions"),
    ("ovl_b", 0x80300E44, (0x80300E40, 0x28C2000F), S32 + "ctx->r2 = (int32_t)ctx->r6 < 16;", "season rating: 16 games a week"),
    ("ovl_b", 0x80300E70, (0x80300E70, 0x3C01802F), "if (blitz_s32 && ctx->r2 == 31) ctx->r3 = 0x700;", "season rating: team 31's stats"),
    ("main", 0x80263690, (0x80263690, 0x24020002), "blitz_s32_after_load(rdram);", "LOAD (after the record copy): a 32-team season or team+'s"),
]


# Drop-in files with names the game lacks (src/game/art_dropin.cpp: new announcer banks such as
# texans.wbk). The TOC can't grow in place (images are found by TOC index), so the game's open-by-name
# (boot func_80001C2C, a binary search) gets a fallback to a separate list. Inert without such files.
DROPIN = [
    ("boot", 0x80001CDC, (0x80001CDC, 0x12600027), "blitz_fs_lookup_extra(rdram, ctx);", "not found: try the extra names (s3, s0)"),
    ("boot", 0x80001CFC, (0x80001CFC, 0x1040001F), "if (blitz_fs_is_extra((int32_t)ctx->r16)) ctx->r2 = 1;", "an extra index passes the range test"),
    ("boot", 0x80001D34, (0x80001D34, 0x00431021), "if (blitz_fs_is_extra((int32_t)ctx->r16)) ctx->r2 = (int32_t)(blitz_fs_extra_entry((int32_t)ctx->r16) - (int32_t)ctx->r3);",
     "the extra's entry instead of TOC + index*32"),
]


# Team select in name order (src/game/team_select.cpp): UP/DOWN and the neighbour rows follow the
# cities alphabetically; the cursor and the stored choice stay real slots. Always on (Modern too).
TEAMSEL = [
    ("ovl_b", 0x802F9B60, (0x802F9B60, 0x02C2082A), "ctx->r22 = blitz_teamsel_moved(rdram, (int32_t)ctx->r22, MEM_W(0x4C, ctx->r29), (int32_t)ctx->r2);",
     "UP/DOWN moved s6 by +-1 (v0 = team count): step in name order"),
    ("ovl_b", 0x802FA3AC, (0x802FA3AC, 0x8FA20054), "ctx->r19 = blitz_teamsel_step(rdram, (int32_t)ctx->r19 + 1, -1, MEM_W(0x54, ctx->r29));",
     "rows above the cursor: the previous team in name order"),
    ("ovl_b", 0x802FA4EC, (0x802FA4EC, 0x24040014), "ctx->r19 = blitz_teamsel_step(rdram, (int32_t)ctx->r19 - 1, 1, (int32_t)ctx->r30);",
     "rows below the cursor: the next team in name order"),
]


# League paks (src/game/content_pack_tp.cpp, build/analysis/league_paks.md): the main menu greys and
# skips the modes a league pak doesn't play (Season always), and league teams get a silent announcer
# (or their base franchise's). No effect without a league pak.
LEAGUE = [
    ("main", 0x80256D30, (0x80256D30, 0x3C028029), "blitz_league_menu(rdram, ctx, 0);", "main menu made: off modes grey, cursor off them"),
    ("main", 0x802570E4, (0x802570E4, 0xAFA20010), "blitz_league_menu(rdram, ctx, 1);", "UP/DOWN moved the mode: step past off modes"),
    ("main", 0x8025712C, (0x8025712C, 0x3C028029), "blitz_league_menu(rdram, ctx, 2);", "redraw greyed all: off modes darker"),
    ("main", 0x80257250, (0x80257250, 0x12020005), "blitz_league_menu(rdram, ctx, 3);", "button dispatch: never confirm an off mode"),
    ("main", 0x8024C528, (0x8024C528, 0x0C09EFD4), "blitz_league_bank(rdram);", "VS screen: announcer bank names before they load"),
]

def is_branch(w):
    """MIPS branch or jump (so the next instruction is its delay slot)."""
    op, rs, funct = w >> 26, (w >> 21) & 31, w & 63
    return (op == 0 and funct in (8, 9)) or op in (1, 2, 3, 4, 5, 6, 7, 0x14, 0x15, 0x16, 0x17) or (op == 0x11 and rs == 8)


COP0 = [  # (vram, destination register)
    (0x80249270, 2),
    (0x802492A0, 3),
]


def function_in(syms, section, vram):
    for sec in syms["section"]:
        if sec["name"] != section:
            continue
        for f in sec["functions"]:
            if f["vram"] <= vram < f["vram"] + f["size"]:
                return f
    raise SystemExit(f"no {section} function contains 0x{vram:08X}")


def cmd_toml(args):
    rom = TP_ROM.read_bytes()
    syms = tomllib.loads(TP_SYMS.read_text())
    text = CLASSIC_TOML.read_text()
    replacements = {
        # N64Recomp resolves these relative to the config file's own directory (build/tp).
        'output_func_path = "build/RecompiledFuncs"': 'output_func_path = "RecompiledFuncs"',
        'symbols_file_path = "build/syms/blitz2000.syms.toml"': 'symbols_file_path = "syms/blitz2000_teamplus.syms.toml"',
        'rom_file_path = "build/baserom.z64"': 'rom_file_path = "../teamplus.z64"',
        # The game loop has its team+ name there (same address and code at the play-log hook).
        'func = "ovl_a_func_802FEF08"': 'func = "tp_game_process"',
    }
    for old, new in replacements.items():
        if old not in text:
            raise SystemExit(f"blitz2000.toml no longer contains: {old}")
        text = text.replace(old, new)

    lines = ["", "# ---- 2026 flavour (team+) fixes, generated by tools/teamplus.py; see its docstring ----"]
    main_func = function_in(syms, "main", COP0[0][0])["name"]
    for vram, reg in COP0:
        lines += ["[[patches.instruction]]", f'func = "{main_func}"', f"vram = 0x{vram:08X}", "value = 0x00000000",
                  "[[patches.hook]]", f'func = "{main_func}"', f"before_vram = 0x{vram:08X}",
                  f'text = "    ctx->r{reg} = blitz_cop0_count();"']
    sec_of = {0x803015F8: "ovl_b", 0x8030656C: "ovl_a", 0x8024C1D0: "main", 0x8024C6B4: "main"}
    expected = {0x8024C6B4: 0x2C100001, 0x803015F8: 0x3C03A002, 0x8030656C: 0x8CA50E8C, 0x8024C1D0: 0x9042A434, 0x80249270: 0x40024800, 0x802492A0: 0x40034800}
    seg_rom = {sec["name"]: (sec["rom"], sec["vram"]) for sec in syms["section"]}
    for vram, want in expected.items():
        name = sec_of.get(vram, "main")
        rom_start, sec_vram = seg_rom[name]
        have = int.from_bytes(rom[rom_start + vram - sec_vram:rom_start + vram - sec_vram + 4], "big")
        if have != want:
            raise SystemExit(f"0x{vram:08X} holds 0x{have:08X}, expected 0x{want:08X}: not the team+ code these fixes are for")
    for _name, vram, value, comment in FIXES:
        f = function_in(syms, sec_of[vram], vram)
        lines += [f"# {comment}", "[[patches.instruction]]", f'func = "{f["name"]}"', f"vram = 0x{vram:08X}", f"value = 0x{value:08X}"]

    lines.append("# ---- Advanced / 32-team Season hooks (no effect unless blitz_adv / blitz_s32 is set), drop-in file lookup, team select, league paks ----")
    hooked = set()
    for name, vram, (chk, want), hook, comment in ADVANCED + SEASON32 + DROPIN + TEAMSEL + LEAGUE:
        if vram in hooked:
            raise SystemExit(f"two hooks at 0x{vram:08X}: give the site one text")
        hooked.add(vram)
        rom_start, sec_vram = seg_rom[name]
        word = lambda a: int.from_bytes(rom[rom_start + a - sec_vram:rom_start + a - sec_vram + 4], "big")
        if word(chk) != want:
            raise SystemExit(f"Advanced hook 0x{vram:08X}: 0x{chk:08X} holds 0x{word(chk):08X}, expected 0x{want:08X}")
        if is_branch(word(vram - 4)):
            raise SystemExit(f"Advanced hook 0x{vram:08X} is in a delay slot")
        f = function_in(syms, name, vram)
        lines += [f"# {comment}", "[[patches.hook]]", f'func = "{f["name"]}"', f"before_vram = 0x{vram:08X}",
                  f'text = "    {hook}"']

    zero_dest = 0
    for sec in syms["section"]:
        for f in sec["functions"]:
            for a in range(f["vram"], f["vram"] + f["size"], 4):
                o = sec["rom"] + (a - sec["vram"])
                w = int.from_bytes(rom[o:o + 4], "big")
                if w in (0x00000020, 0x00000021):  # add/addu $zero, $zero, $zero
                    lines += ["[[patches.instruction]]", f'func = "{f["name"]}"', f"vram = 0x{a:08X}", "value = 0x00000000"]
                    zero_dest += 1
    TP_TOML.parent.mkdir(parents=True, exist_ok=True)
    TP_TOML.write_text(text.rstrip("\n") + "\n" + "\n".join(lines) + "\n")
    print(f"wrote {TP_TOML.relative_to(ROOT)}: {len(COP0)} cop0 hooks, {len(FIXES)} fixes, {len(ADVANCED)} Advanced hooks, {len(SEASON32)} 32-team Season hooks, {zero_dest} zero-destination nops")


def cmd_verify(args):
    cfg = json.loads(CONFIG.read_text())
    if not TP_ROM.exists():
        raise SystemExit("build/teamplus.z64 missing: run with TEAMPLUS_BPS=<path to Blitz2k-11_team+.bps>")
    rom = TP_ROM.read_bytes()
    if hashlib.sha1(rom).hexdigest() != cfg["target_sha1"]:
        raise SystemExit("build/teamplus.z64 isn't the pinned team+ ROM; rebuild it with TEAMPLUS_BPS=...")
    print("build/teamplus.z64 ok")


def cmd_check_code(args):
    cfg = json.loads(CONFIG.read_text())
    rom = Path(args.rom).read_bytes()
    ok = len(rom) == 0x1000000 and f"0x{code_hash(rom, cfg):016X}" == cfg["code_xxh3"]
    print("same code as team+: runs on the 2026 build" if ok else "different code: not for the 2026 build")
    return 0 if ok else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("rom")
    r.add_argument("--bps", required=True)
    sub.add_parser("toml")
    sub.add_parser("verify")
    c = sub.add_parser("check-code")
    c.add_argument("rom")
    args = ap.parse_args(argv)
    return {"rom": cmd_rom, "toml": cmd_toml, "verify": cmd_verify, "check-code": cmd_check_code}[args.cmd](args) or 0


if __name__ == "__main__":
    sys.exit(main())
