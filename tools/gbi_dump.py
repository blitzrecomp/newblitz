#!/usr/bin/env python3
"""Decode (and roughly render) an F3DEX2 display list from an RDRAM snapshot.

Walks a display list the way the F3DEX2 RSP microcode would (segment table,
G_DL calls/branches, display-list stack, matrix stack, vertex buffer), tracks
the RSP and RDP state including a TMEM emulation, and can print:

  --cmds       every command, decoded, indented by DL depth
  --draws      one entry per draw (a run of triangles with unchanged state,
               or a texture rectangle) with the full render state behind it:
               geometry mode, othermodes, combiner, colors, G_TEXTURE, every
               tile the draw can sample and the TMEM load that filled it,
               depth/fog ranges and the screen bounding box
  --textures   every TMEM load with its RDRAM source and data statistics
  --dump-textures DIR
               each tile a draw samples, decoded from the emulated TMEM (what
               the RDP actually sees), as PNG
  --render PNG a software rasterization of the frame (z-buffer, combiner,
               blender incl. fog, point sampling, near clipping), plus an
               id image and per-draw visible pixel counts. --lod-model picks
               hardware RDP tile/LOD selection or RT64's computeLOD() model.

RDRAM snapshots from our port are stored as N64ModernRuntime keeps RDRAM:
native little-endian 32-bit words (each N64 big-endian word byte-reversed).
Plain big-endian dumps (e.g. from an emulator) are read with --be.

Usage examples:
  gbi_dump.py build/dumps/rdram_310.bin 8004D7D8 --cmds
  gbi_dump.py build/dumps/rdram_310.bin 8004D7D8 --draws --diff-state
  gbi_dump.py build/dumps/rdram_310.bin 8004D7D8 --textures
  gbi_dump.py build/dumps/rdram_310.bin 8004D7D8 --dump-textures out/tex
  gbi_dump.py build/dumps/rdram_310.bin 8004D7D8 --render out/frame.png
  gbi_dump.py build/dumps/rdram_310.bin 8004D7D8 --render out/f.png --only-draws 0-40
  gbi_dump.py build/dumps/rdram_310.bin 8004D7D8 --cmds --no-follow

Addresses are hex (with or without 0x). Segmented addresses are resolved with
the segment table built from G_MOVEWORD G_MW_SEGMENT, exactly like the RSP:
phys = segment[(addr >> 24) & 0xF] + (addr & 0x00FFFFFF).

G_CULLDL and G_BRANCH_Z depend on transformed vertices. The tool tracks the
matrices, viewport and vertex buffer and evaluates both (see --cull and
--branch-z); the evaluation is a close model of the microcode, not exact.
G_LOAD_UCODE with text 0x80028FA0 switches to F3DLX2.Rej (same command set, a
64-entry vertex buffer); 0x80027C10 switches back to F3DEX2. RT64's extended
GBI (the hook in G_NOOP, then opcode 0x64) is skipped with each command's real
length. Lighting is not computed: lit vertices render mid-grey in --render.

Pure Python 3.10+, no dependencies.
"""
from __future__ import annotations

import os
import sys

# tools/dis.py shadows the stdlib 'dis' module (dataclasses -> inspect -> dis)
# when this script runs from tools/, so take the script directory off the path.
_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:] = [p for p in sys.path if os.path.abspath(p or ".") != _HERE]

import argparse  # noqa: E402
import math  # noqa: E402
import struct  # noqa: E402
import zlib  # noqa: E402
from dataclasses import dataclass, field, replace  # noqa: E402
from pathlib import Path  # noqa: E402

# ---------------------------------------------------------------------------
# Constants (F3DEX2 gbi.h)
# ---------------------------------------------------------------------------

G_VTX, G_MODIFYVTX, G_CULLDL, G_BRANCH_Z = 0x01, 0x02, 0x03, 0x04
G_TRI1, G_TRI2, G_QUAD, G_LINE3D = 0x05, 0x06, 0x07, 0x08
G_SPECIAL_3, G_SPECIAL_2, G_SPECIAL_1 = 0xD3, 0xD4, 0xD5
G_DMA_IO, G_TEXTURE, G_POPMTX, G_GEOMETRYMODE = 0xD6, 0xD7, 0xD8, 0xD9
G_MTX, G_MOVEWORD, G_MOVEMEM, G_LOAD_UCODE = 0xDA, 0xDB, 0xDC, 0xDD
G_DL, G_ENDDL, G_NOOP, G_RDPHALF_1 = 0xDE, 0xDF, 0xE0, 0xE1
G_SETOTHERMODE_L, G_SETOTHERMODE_H = 0xE2, 0xE3
G_TEXRECT, G_TEXRECTFLIP = 0xE4, 0xE5
G_RDPLOADSYNC, G_RDPPIPESYNC, G_RDPTILESYNC, G_RDPFULLSYNC = 0xE6, 0xE7, 0xE8, 0xE9
G_SETKEYGB, G_SETKEYR, G_SETCONVERT, G_SETSCISSOR = 0xEA, 0xEB, 0xEC, 0xED
G_SETPRIMDEPTH, G_RDPSETOTHERMODE, G_LOADTLUT, G_RDPHALF_2 = 0xEE, 0xEF, 0xF0, 0xF1
G_SETTILESIZE, G_LOADBLOCK, G_LOADTILE, G_SETTILE = 0xF2, 0xF3, 0xF4, 0xF5
G_FILLRECT, G_SETFILLCOLOR, G_SETFOGCOLOR, G_SETBLENDCOLOR = 0xF6, 0xF7, 0xF8, 0xF9
G_SETPRIMCOLOR, G_SETENVCOLOR, G_SETCOMBINE = 0xFA, 0xFB, 0xFC
G_SETTIMG, G_SETZIMG, G_SETCIMG = 0xFD, 0xFE, 0xFF

OPNAMES = {
    0x00: "G_SPNOOP", G_VTX: "G_VTX", G_MODIFYVTX: "G_MODIFYVTX", G_CULLDL: "G_CULLDL",
    G_BRANCH_Z: "G_BRANCH_Z", G_TRI1: "G_TRI1", G_TRI2: "G_TRI2", G_QUAD: "G_QUAD",
    G_LINE3D: "G_LINE3D",
    G_SPECIAL_3: "G_SPECIAL_3", G_SPECIAL_2: "G_SPECIAL_2", G_SPECIAL_1: "G_SPECIAL_1",
    G_DMA_IO: "G_DMA_IO", G_TEXTURE: "G_TEXTURE", G_POPMTX: "G_POPMTX",
    G_GEOMETRYMODE: "G_GEOMETRYMODE", G_MTX: "G_MTX", G_MOVEWORD: "G_MOVEWORD",
    G_MOVEMEM: "G_MOVEMEM", G_LOAD_UCODE: "G_LOAD_UCODE", G_DL: "G_DL", G_ENDDL: "G_ENDDL",
    G_NOOP: "G_NOOP", G_RDPHALF_1: "G_RDPHALF_1", G_SETOTHERMODE_L: "G_SETOTHERMODE_L",
    G_SETOTHERMODE_H: "G_SETOTHERMODE_H", G_TEXRECT: "G_TEXRECT", G_TEXRECTFLIP: "G_TEXRECTFLIP",
    G_RDPLOADSYNC: "G_RDPLOADSYNC", G_RDPPIPESYNC: "G_RDPPIPESYNC", G_RDPTILESYNC: "G_RDPTILESYNC",
    G_RDPFULLSYNC: "G_RDPFULLSYNC", G_SETKEYGB: "G_SETKEYGB", G_SETKEYR: "G_SETKEYR",
    G_SETCONVERT: "G_SETCONVERT", G_SETSCISSOR: "G_SETSCISSOR", G_SETPRIMDEPTH: "G_SETPRIMDEPTH",
    G_RDPSETOTHERMODE: "G_RDPSETOTHERMODE", G_LOADTLUT: "G_LOADTLUT", G_RDPHALF_2: "G_RDPHALF_2",
    G_SETTILESIZE: "G_SETTILESIZE", G_LOADBLOCK: "G_LOADBLOCK", G_LOADTILE: "G_LOADTILE",
    G_SETTILE: "G_SETTILE", G_FILLRECT: "G_FILLRECT", G_SETFILLCOLOR: "G_SETFILLCOLOR",
    G_SETFOGCOLOR: "G_SETFOGCOLOR", G_SETBLENDCOLOR: "G_SETBLENDCOLOR",
    G_SETPRIMCOLOR: "G_SETPRIMCOLOR", G_SETENVCOLOR: "G_SETENVCOLOR", G_SETCOMBINE: "G_SETCOMBINE",
    G_SETTIMG: "G_SETTIMG", G_SETZIMG: "G_SETZIMG", G_SETCIMG: "G_SETCIMG",
    0x100: "G_EX(RT64)",
}

FMT = ["RGBA", "YUV", "CI", "IA", "I", "?5", "?6", "?7"]
SIZ = ["4b", "8b", "16b", "32b"]
SIZ_BITS = [4, 8, 16, 32]

MW_NAMES = {0x00: "G_MW_MATRIX", 0x02: "G_MW_NUMLIGHT", 0x04: "G_MW_CLIP", 0x06: "G_MW_SEGMENT",
            0x08: "G_MW_FOG", 0x0A: "G_MW_LIGHTCOL", 0x0C: "G_MW_FORCEMTX", 0x0E: "G_MW_PERSPNORM"}
MV_NAMES = {2: "G_MV_MMTX", 6: "G_MV_PMTX", 8: "G_MV_VIEWPORT", 10: "G_MV_LIGHT",
            12: "G_MV_POINT", 14: "G_MV_MATRIX"}

G_ZBUFFER, G_SHADE, G_CULL_FRONT, G_CULL_BACK = 0x1, 0x4, 0x200, 0x400
G_FOG, G_LIGHTING, G_TEXTURE_GEN = 0x10000, 0x20000, 0x40000
GEOM_BITS = [
    (G_ZBUFFER, "G_ZBUFFER"), (G_SHADE, "G_SHADE"), (G_CULL_FRONT, "G_CULL_FRONT"),
    (G_CULL_BACK, "G_CULL_BACK"), (G_FOG, "G_FOG"), (G_LIGHTING, "G_LIGHTING"),
    (G_TEXTURE_GEN, "G_TEXTURE_GEN"), (0x00080000, "G_TEXTURE_GEN_LINEAR"), (0x00100000, "G_LOD"),
    (0x00200000, "G_SHADING_SMOOTH"), (0x00800000, "G_CLIPPING"),
]

# Other mode H fields: (name, shift, bits, value names)
OMH_FIELDS = [
    ("alphadither", 4, 2, ["PATTERN", "NOTPATTERN", "NOISE", "DISABLE"]),
    ("rgbdither", 6, 2, ["MAGICSQ", "BAYER", "NOISE", "DISABLE"]),
    ("combkey", 8, 1, ["NONE", "KEY"]),
    ("textconv", 9, 3, ["CONV", "?1", "?2", "?3", "?4", "FILTCONV", "FILT", "?7"]),
    ("textfilt", 12, 2, ["POINT", "?1", "BILERP", "AVERAGE"]),
    ("textlut", 14, 2, ["NONE", "?1", "RGBA16", "IA16"]),
    ("textlod", 16, 1, ["TILE", "LOD"]),
    ("textdetail", 17, 2, ["CLAMP", "SHARPEN", "DETAIL", "?3"]),
    ("textpersp", 19, 1, ["NONE", "PERSP"]),
    ("cycletype", 20, 2, ["1CYCLE", "2CYCLE", "COPY", "FILL"]),
    ("colordither", 22, 1, ["0", "1"]),
    ("pipeline", 23, 1, ["NPRIMITIVE", "1PRIMITIVE"]),
]
OML_FIELDS = [
    ("alphacompare", 0, 2, ["NONE", "THRESHOLD", "?2", "DITHER"]),
    ("zsrcsel", 2, 1, ["PIXEL", "PRIM"]),
]
RM_AA_EN, RM_Z_CMP, RM_Z_UPD, RM_IM_RD = 0x8, 0x10, 0x20, 0x40
RM_CVG_X_ALPHA, RM_ALPHA_CVG_SEL, RM_FORCE_BL = 0x1000, 0x2000, 0x4000
RM_FLAGS = [(RM_AA_EN, "AA_EN"), (RM_Z_CMP, "Z_CMP"), (RM_Z_UPD, "Z_UPD"), (RM_IM_RD, "IM_RD"),
            (0x80, "CLR_ON_CVG"), (RM_CVG_X_ALPHA, "CVG_X_ALPHA"), (RM_ALPHA_CVG_SEL, "ALPHA_CVG_SEL"),
            (RM_FORCE_BL, "FORCE_BL"), (0x8000, "TEX_EDGE")]
CVG_DST = ["CLAMP", "WRAP", "FULL", "SAVE"]
ZMODE = ["OPA", "INTER", "XLU", "DEC"]
BL_P = ["IN", "MEM", "BL", "FOG"]
BL_A = ["IN_A", "FOG_A", "SHADE_A", "0"]
BL_M = ["IN", "MEM", "BL", "FOG"]
BL_B = ["1MA", "MEM_A", "1", "0"]

CC_A = ["COMBINED", "TEXEL0", "TEXEL1", "PRIMITIVE", "SHADE", "ENVIRONMENT", "1", "NOISE"] + ["0"] * 8
CC_B = ["COMBINED", "TEXEL0", "TEXEL1", "PRIMITIVE", "SHADE", "ENVIRONMENT", "CENTER", "K4"] + ["0"] * 8
CC_C = ["COMBINED", "TEXEL0", "TEXEL1", "PRIMITIVE", "SHADE", "ENVIRONMENT", "SCALE",
        "COMBINED_ALPHA", "TEXEL0_ALPHA", "TEXEL1_ALPHA", "PRIMITIVE_ALPHA", "SHADE_ALPHA",
        "ENV_ALPHA", "LOD_FRACTION", "PRIM_LOD_FRAC", "K5"] + ["0"] * 16
CC_D = ["COMBINED", "TEXEL0", "TEXEL1", "PRIMITIVE", "SHADE", "ENVIRONMENT", "1", "0"]
AC_ABD = ["COMBINED", "TEXEL0", "TEXEL1", "PRIMITIVE", "SHADE", "ENVIRONMENT", "1", "0"]
AC_C = ["LOD_FRACTION", "TEXEL0", "TEXEL1", "PRIMITIVE", "SHADE", "ENVIRONMENT", "PRIM_LOD_FRAC", "0"]

VTX_BUFFER_SIZE = 32   # F3DEX2
VTX_BUFFER_REJ = 64    # F3DLX2.Rej: no clipping, so the vertex buffer has 64 entries (the players' loads use up to 46)
# The game's two microcodes, by G_LOAD_UCODE text address (physical): F3DEX2 at boot (OSTask ucode 0x80027C10), F3DLX2.Rej
# for the sections between the two switches (docs/PSP_PORT.md 4.5).
UCODE_F3DEX2_TEXT, UCODE_F3DLX2_REJ_TEXT = 0x027C10, 0x028FA0

# RT64's extended GBI (lib/rt64/include/rt64_extended_gbi.h): "G_NOOP 0xE0525464, w1 = (1 << 28) | opcode" turns on the
# extended opcode (0x64 by default); each G_EX command is 1-3 64-bit words, by its 24-bit sub-opcode. The desktop's
# gfx_ext.cpp emits them, so the dumps hold them. Real F3DEX2 never sees them.
RT64_HOOK_W0 = 0xE0525464
GEX_NAMES = {0x00: "NOOP", 0x01: "PRINT", 0x02: "TEXRECT", 0x03: "FILLRECT", 0x04: "SETVIEWPORT", 0x05: "SETSCISSOR",
             0x06: "SETRECTALIGN", 0x07: "SETVIEWPORTALIGN", 0x08: "SETSCISSORALIGN", 0x09: "SETREFRESHRATE",
             0x0A: "VERTEXZTEST", 0x0B: "ENDVERTEXZTEST", 0x0C: "MATRIXGROUP", 0x0D: "POPMATRIXGROUP",
             0x13: "EDITGROUPBYADDRESS", 0x14: "VERTEX", 0x30: "MATRIX_FLOAT", 0x31: "SETVERTEXSEGMENT"}
GEX_WORDS = {0x02: 3, 0x03: 2, 0x04: 2, 0x05: 2, 0x06: 2, 0x07: 2, 0x08: 3, 0x0C: 2, 0x13: 2, 0x14: 2, 0x30: 2, 0x31: 2}  # others: 1
DL_STACK_DEPTH = 18    # F3DEX2 display list stack
MTX_STACK_DEPTH = 10   # F3DEX2 modelview stack (in RDRAM; gbi.h documents 10)


def bits(v: int, shift: int, n: int) -> int:
    return (v >> shift) & ((1 << n) - 1)


def s16(v: int) -> int:
    return v - 0x10000 if v & 0x8000 else v


def s32(v: int) -> int:
    return v - (1 << 32) if v & 0x80000000 else v


def parse_ranges(text: str) -> set[int]:
    out: set[int] = set()
    for part in text.split(","):
        if "-" in part:
            a, b = part.split("-")
            out.update(range(int(a), int(b) + 1))
        elif part:
            out.add(int(part))
    return out


# ---------------------------------------------------------------------------
# RDRAM access
# ---------------------------------------------------------------------------

class Rdram:
    """An RDRAM snapshot, read in N64 (big-endian) byte order.

    word_swapped=True (default): the file holds native little-endian 32-bit
    words, as N64ModernRuntime stores RDRAM. The image is converted to
    big-endian once on load, so every other read is a plain slice.
    """

    def __init__(self, path: Path, word_swapped: bool = True):
        raw = path.read_bytes()
        if word_swapped:
            n = len(raw) // 4
            raw = struct.pack(f">{n}I", *struct.unpack(f"<{n}I", raw[:n * 4]))
        self.data = raw
        self.size = len(raw)

    @staticmethod
    def phys(addr: int) -> int:
        return addr & 0x1FFFFFFF

    def valid(self, addr: int, n: int = 1) -> bool:
        p = self.phys(addr)
        return p + n <= self.size

    def u16(self, addr: int) -> int:
        p = self.phys(addr)
        return (self.data[p] << 8) | self.data[p + 1]

    def u32(self, addr: int) -> int:
        p = self.phys(addr)
        return int.from_bytes(self.data[p:p + 4], "big")

    def read(self, addr: int, n: int) -> bytes:
        p = self.phys(addr)
        out = self.data[p:p + n]
        return out + bytes(n - len(out)) if len(out) < n else out


# ---------------------------------------------------------------------------
# Texture formats and TMEM
# ---------------------------------------------------------------------------

def rgba5551(v: int) -> tuple:
    r, g, b = (v >> 11) & 31, (v >> 6) & 31, (v >> 1) & 31
    return ((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2), 255 if v & 1 else 0)


def ia16(v: int) -> tuple:
    return (v >> 8, v >> 8, v >> 8, v & 255)


@dataclass
class Tile:
    fmt: int = 0
    siz: int = 0
    line: int = 0
    tmem: int = 0
    palette: int = 0
    cmt: int = 0
    maskt: int = 0
    shiftt: int = 0
    cms: int = 0
    masks: int = 0
    shifts: int = 0
    uls: int = 0
    ult: int = 0
    lrs: int = 0
    lrt: int = 0

    def describe(self) -> str:
        cm = lambda c: ("M" if c & 1 else "") + ("C" if c & 2 else "") or "W"  # noqa: E731
        w = (self.lrs - self.uls) / 4 + 1
        h = (self.lrt - self.ult) / 4 + 1
        return (f"{FMT[self.fmt]}{SIZ[self.siz]} line={self.line} tmem=0x{self.tmem:03X} pal={self.palette} "
                f"s:{cm(self.cms)} m{self.masks} sh{self.shifts} t:{cm(self.cmt)} m{self.maskt} sh{self.shiftt} "
                f"size=({self.uls / 4:g},{self.ult / 4:g})-({self.lrs / 4:g},{self.lrt / 4:g}) [{w:g}x{h:g}]")

    def sample_size(self) -> tuple[int, int]:
        """Width/height the tile can address: its tile size, or the mask if smaller."""
        w = (self.lrs >> 2) - (self.uls >> 2) + 1
        h = (self.lrt >> 2) - (self.ult >> 2) + 1
        if self.masks:
            w = min(w, 1 << self.masks) if w > 0 else 1 << self.masks
        if self.maskt:
            h = min(h, 1 << self.maskt) if h > 0 else 1 << self.maskt
        return max(w, 1), max(h, 1)


class Tmem:
    """4 KB of TMEM, filled the way the RDP fills it.

    Odd lines are stored with the two 32-bit halves of every 64-bit word
    swapped (LOADTILE by line, LOADBLOCK by the dxt line counter), and
    sampling undoes it, as on hardware. RGBA32 texels are split: red/green in
    the low 2 KB, blue/alpha in the high 2 KB. TLUT entries live in the high
    half, each 16-bit entry replicated four times per 64-bit word.
    """

    def __init__(self) -> None:
        self.mem = bytearray(4096)

    def _store(self, byte_addr: int, data: bytes, odd: bool, split32: bool = False) -> None:
        """Store bytes at a TMEM byte address (64-bit aligned chunks)."""
        for k in range(0, len(data), 8):
            chunk = data[k:k + 8].ljust(8, b"\0")
            if odd:
                chunk = chunk[4:8] + chunk[0:4]
            a = (byte_addr + k) & 0xFFF
            if split32:
                a &= 0x7FF
            self.mem[a:a + 8] = chunk

    def load_block(self, mem: Rdram, timg: tuple, tile: Tile, sl: int, tl: int, sh: int, dxt: int) -> tuple[int, int]:
        fmt, siz, width, addr = timg
        bpp = SIZ_BITS[siz]
        start = addr + (tl * width + sl) * bpp // 8
        texels = sh - sl + 1
        nbytes = (texels * bpp + 7) // 8
        raw = mem.read(start, nbytes)
        base = tile.tmem * 8
        if siz == 3:  # RGBA32: 4 bytes/texel in RDRAM -> RG low half, BA high half
            words = (texels * 2 + 7) // 8
            for wi in range(words):
                odd = (((tl << 11) + wi * dxt) >> 11) & 1
                lo = bytearray(8)
                hi = bytearray(8)
                for k in range(4):
                    i = wi * 4 + k
                    px = raw[i * 4:i * 4 + 4].ljust(4, b"\0")
                    lo[k * 2:k * 2 + 2] = px[0:2]
                    hi[k * 2:k * 2 + 2] = px[2:4]
                self._store(base + wi * 8, bytes(lo), bool(odd), split32=True)
                self._store(((base + wi * 8) & 0x7FF) | 0x800, bytes(hi), bool(odd))
        else:
            for wi in range((nbytes + 7) // 8):
                odd = (((tl << 11) + wi * dxt) >> 11) & 1
                self._store(base + wi * 8, raw[wi * 8:wi * 8 + 8], bool(odd))
        return start, nbytes

    def load_tile(self, mem: Rdram, timg: tuple, tile: Tile, sl: int, tl: int, sh: int, th: int) -> tuple[int, int]:
        fmt, siz, width, addr = timg
        bpp = SIZ_BITS[siz]
        x0, y0, x1, y1 = sl >> 2, tl >> 2, sh >> 2, th >> 2
        row_bytes = ((x1 - x0 + 1) * bpp + 7) // 8
        start = addr + (y0 * width + x0) * bpp // 8
        for r in range(y1 - y0 + 1):
            src = addr + ((y0 + r) * width + x0) * bpp // 8
            raw = mem.read(src, row_bytes)
            dst = (tile.tmem + r * tile.line) * 8
            if siz == 3:
                lo = bytearray()
                hi = bytearray()
                for i in range(0, len(raw), 4):
                    lo += raw[i:i + 2]
                    hi += raw[i + 2:i + 4]
                self._store(dst, bytes(lo), bool(r & 1), split32=True)
                self._store((dst & 0x7FF) | 0x800, bytes(hi), bool(r & 1))
            else:
                self._store(dst, raw, bool(r & 1))
        nbytes = ((y1 - y0) * width + (x1 - x0 + 1)) * bpp // 8
        return start, nbytes

    def load_tlut(self, mem: Rdram, timg: tuple, tile: Tile, sl: int, tl: int, sh: int, th: int) -> tuple[int, int]:
        fmt, siz, width, addr = timg
        first, last = sl >> 2, sh >> 2
        count = last - first + 1
        start = addr + (tl >> 2) * width * 2 + first * 2
        for i in range(count):
            e = mem.read(start + i * 2, 2)
            a = (tile.tmem * 8 + i * 8) & 0xFFF
            self.mem[a:a + 8] = e * 4
        return start, count * 2


def tmem_texel(tm: bytes, tile: Tile, s: int, t: int, tlut_mode: int) -> tuple:
    """Fetch and decode one texel at integer tile-relative (s, t)."""
    siz, fmt = tile.siz, tile.fmt
    xor = 4 if t & 1 else 0
    base = (tile.tmem + tile.line * t) * 8
    if siz == 0:
        a = ((base + (s >> 1)) ^ xor) & 0xFFF
        v = (tm[a] >> 4) if not (s & 1) else (tm[a] & 15)
    elif siz == 1:
        v = tm[((base + s) ^ xor) & 0xFFF]
    elif siz == 2:
        a = ((base + s * 2) ^ xor) & 0xFFE
        v = (tm[a] << 8) | tm[a + 1]
    else:
        a = ((base + s * 2) ^ xor) & 0x7FE
        return (tm[a], tm[a + 1], tm[a | 0x800], tm[(a | 0x800) + 1])
    if tlut_mode and siz <= 1:
        idx = ((tile.palette << 4) | v) if siz == 0 else v
        a = (0x800 + idx * 8) & 0xFFF
        e = (tm[a] << 8) | tm[a + 1]
        return ia16(e) if tlut_mode == 3 else rgba5551(e)
    name = FMT[fmt]
    if name == "RGBA" and siz == 2:
        return rgba5551(v)
    if name == "IA":
        if siz == 0:
            i = (v >> 1) * 255 // 7
            return (i, i, i, 255 if v & 1 else 0)
        if siz == 1:
            i = (v >> 4) * 17
            return (i, i, i, (v & 15) * 17)
        return ia16(v)
    if name == "I":
        i = v * 17 if siz == 0 else v & 255
        return (i, i, i, i)
    if name == "CI":  # CI without TLUT enabled: the index reads as intensity
        i = v * 17 if siz == 0 else v & 255
        return (i, i, i, i)
    i = v & 255
    return (i, i, i, 255)


def tile_coord(v: float, shift: int, ul: int, mask: int, cm: int, ul_to_lr: int) -> int:
    """Apply the RDP tile pipeline to one texture coordinate (in texels)."""
    if 0 < shift <= 10:
        v /= (1 << shift)
    elif shift > 10:
        v *= (1 << (16 - shift))
    i = math.floor(v - ul / 4)
    if (cm & 2) or mask == 0:
        if i < 0:
            i = 0
        elif i > ul_to_lr:
            i = ul_to_lr
    if mask:
        if (cm & 1) and (i >> mask) & 1:
            i = ~i
        i &= (1 << mask) - 1
    return i


def sample_tile(tm: bytes, tile: Tile, s: float, t: float, tlut_mode: int) -> tuple:
    si = tile_coord(s, tile.shifts, tile.uls, tile.masks, tile.cms, (tile.lrs >> 2) - (tile.uls >> 2))
    ti = tile_coord(t, tile.shiftt, tile.ult, tile.maskt, tile.cmt, (tile.lrt >> 2) - (tile.ult >> 2))
    return tmem_texel(tm, tile, si, ti, tlut_mode)


def write_png(path: Path, width: int, height: int, px) -> None:
    """px: flat sequence of (r, g, b[, a]) tuples, row-major."""
    def chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            p = px[y * width + x]
            rows.extend((p[0], p[1], p[2], p[3] if len(p) > 3 else 255))
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(rows), 6)) + chunk(b"IEND", b"")
    path.write_bytes(png)


def byte_stats(raw: bytes) -> str:
    if not raw:
        return "empty"
    nz = sum(1 for b in raw if b)
    return f"{nz * 100 // len(raw)}% nonzero, {len(set(raw))} distinct byte values"


# ---------------------------------------------------------------------------
# Decoded state
# ---------------------------------------------------------------------------

@dataclass
class TmemLoad:
    """One LOADBLOCK / LOADTILE / LOADTLUT, with the image it copied from."""
    kind: str
    dl_addr: int
    timg: tuple       # (fmt, siz, width, phys address) at load time
    tile: int
    tmem: int
    params: tuple
    src_start: int    # physical start address of the bytes read
    src_bytes: int    # bytes read from RDRAM

    def describe(self) -> str:
        fmt, siz, width, _ = self.timg
        return (f"{self.kind} @{self.dl_addr:08X} from 0x{self.src_start:08X}+0x{self.src_bytes:X} "
                f"(TIMG {FMT[fmt]}{SIZ[siz]} w={width}) -> tile{self.tile} tmem=0x{self.tmem:03X} {self.params}")


@dataclass
class Vertex:
    x: int = 0
    y: int = 0
    z: int = 0
    s: float = 0.0  # after the G_TEXTURE scale, s10.5 units
    t: float = 0.0
    rgba: tuple = (0, 0, 0, 0)  # shade color; alpha replaced by fog when G_FOG
    clip: tuple | None = None    # (x, y, z, w) clip coordinates
    screen: tuple | None = None  # (x, y, z) in pixels / screen z


@dataclass
class State:
    segments: list = field(default_factory=lambda: [0] * 16)
    geometry: int = 0
    othermode_h: int = 0
    othermode_l: int = 0
    combine: tuple = (0, 0)
    tex_on: int = 0
    tex_tile: int = 0
    tex_levels: int = 0  # G_TEXTURE "level" field (mip levels - 1); the RDP's max_level
    tex_s: int = 0
    tex_t: int = 0
    timg: tuple = (0, 0, 1, 0)  # fmt, siz, width, phys address
    cimg: tuple = (0, 0, 0, 0)
    zimg: int = 0
    tiles: list = field(default_factory=lambda: [Tile() for _ in range(8)])
    prim: int = 0
    prim_minlevel: int = 0
    prim_lodfrac: int = 0
    env: int = 0
    fog: int = 0
    blend: int = 0
    fill: int = 0
    prim_depth: tuple = (0, 0)
    scissor: tuple = (0, 0, 320, 240)
    rdphalf1: int = 0
    rdphalf2: int = 0
    fog_mul: int = 0
    fog_ofs: int = 0
    num_lights: int = 0
    perspnorm: int = 0
    proj: list | None = None
    mv_stack: list = field(default_factory=list)
    mv: list | None = None
    mtx_addr_proj: int = 0
    mtx_addr_mv: int = 0
    viewport: tuple | None = None  # (scale xyzw, trans xyzw), raw RSP units
    vtx: list = field(default_factory=lambda: [Vertex() for _ in range(VTX_BUFFER_REJ)])
    vtx_size: int = VTX_BUFFER_SIZE  # 64 while F3DLX2.Rej is loaded
    gex_op: int = -1  # RT64 extended opcode once the hook turned it on
    vtx_src: int = 0
    loads: dict = field(default_factory=dict)  # tmem word address -> last TmemLoad that wrote it
    tlut_load: TmemLoad | None = None


@dataclass
class Snap:
    """Everything the rasterizer needs, captured when a draw begins."""
    tmem: bytes
    tiles: list
    othermode_h: int
    othermode_l: int
    combine: tuple
    geometry: int
    prim: int
    prim_minlevel: int
    prim_lodfrac: int
    env: int
    fog: int
    blend: int
    tex_on: int
    tex_tile: int
    tex_levels: int
    scissor: tuple
    prim_depth: tuple


@dataclass
class Draw:
    index: int
    dl_addr: int        # address of the first command of the batch
    kind: str = "tris"  # tris / texrect / fillrect
    tris: int = 0
    cmds: int = 0
    vtx_src: int = 0
    state_text: str = ""
    screen_bbox: tuple | None = None
    textures: list = field(default_factory=list)  # (tile index, Tile snapshot, TmemLoad)
    ndc_z: tuple | None = None
    w_range: tuple | None = None
    behind: int = 0
    fog_range: tuple | None = None
    snap: Snap | None = None
    tri_data: list = field(default_factory=list)  # [(v0, v1, v2)], v = (cx, cy, cz, cw, s, t, r, g, b, a)
    rect: tuple | None = None  # texrect/fillrect: (xl, yl, xh, yh, tile, s, t, dsdx, dtdy, flip, fillcolor)
    vp: tuple | None = None    # viewport the draw's vertices were transformed with


IDENTITY = [[1.0 if i == j else 0.0 for j in range(4)] for i in range(4)]


def mat_mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def read_mtx(mem: Rdram, addr: int):
    """Read a libultra Mtx (s15.16: 16 integer halves, then 16 fraction halves)."""
    raw = mem.read(addr, 64)
    ints = struct.unpack(">16h", raw[:32])
    fracs = struct.unpack(">16H", raw[32:])
    return [[ints[i * 4 + j] + fracs[i * 4 + j] / 65536.0 for j in range(4)] for i in range(4)]


# ---------------------------------------------------------------------------
# Formatting helpers
# ---------------------------------------------------------------------------

def fmt_geometry(g: int) -> str:
    names = [n for b, n in GEOM_BITS if g & b]
    rest = g & ~sum(b for b, _ in GEOM_BITS)
    if rest:
        names.append(f"0x{rest:X}")
    return "|".join(names) or "0"


def fmt_othermode_h(h: int, full: bool = True) -> str:
    keep = ("cycletype", "textlod", "textdetail", "textlut", "textfilt", "textpersp", "textconv")
    out = []
    for name, sh, n, vals in OMH_FIELDS:
        v = bits(h, sh, n)
        if full or name in keep:
            out.append(f"{name}={vals[v] if v < len(vals) else v}")
    return " ".join(out)


def fmt_rendermode(rm: int) -> str:
    flags = [n for b, n in RM_FLAGS if rm & b]
    flags.append(f"CVG_DST_{CVG_DST[bits(rm, 8, 2)]}")
    flags.append(f"ZMODE_{ZMODE[bits(rm, 10, 2)]}")
    c1 = f"{BL_P[bits(rm, 30, 2)]}*{BL_A[bits(rm, 26, 2)]}+{BL_M[bits(rm, 22, 2)]}*{BL_B[bits(rm, 18, 2)]}"
    c2 = f"{BL_P[bits(rm, 28, 2)]}*{BL_A[bits(rm, 24, 2)]}+{BL_M[bits(rm, 20, 2)]}*{BL_B[bits(rm, 16, 2)]}"
    return "|".join(flags) + f" bl1=({c1}) bl2=({c2})"


def fmt_othermode_l(lo: int) -> str:
    out = [f"{name}={vals[bits(lo, sh, n)]}" for name, sh, n, vals in OML_FIELDS]
    return " ".join(out) + " rm=" + fmt_rendermode(lo)


def decode_combine(w0: int, w1: int):
    """Return (cycle1 rgb, cycle1 alpha, cycle2 rgb, cycle2 alpha), each (a, b, c, d) names."""
    a0, c0, Aa0, Ac0 = bits(w0, 20, 4), bits(w0, 15, 5), bits(w0, 12, 3), bits(w0, 9, 3)
    a1, c1 = bits(w0, 5, 4), bits(w0, 0, 5)
    b0, b1, Aa1, Ac1 = bits(w1, 28, 4), bits(w1, 24, 4), bits(w1, 21, 3), bits(w1, 18, 3)
    d0, Ab0, Ad0 = bits(w1, 15, 3), bits(w1, 12, 3), bits(w1, 9, 3)
    d1, Ab1, Ad1 = bits(w1, 6, 3), bits(w1, 3, 3), bits(w1, 0, 3)
    return ((CC_A[a0], CC_B[b0], CC_C[c0], CC_D[d0]), (AC_ABD[Aa0], AC_ABD[Ab0], AC_C[Ac0], AC_ABD[Ad0]),
            (CC_A[a1], CC_B[b1], CC_C[c1], CC_D[d1]), (AC_ABD[Aa1], AC_ABD[Ab1], AC_C[Ac1], AC_ABD[Ad1]))


def fmt_combine(w0: int, w1: int, two_cycle: bool = True) -> str:
    c1rgb, c1a, c2rgb, c2a = decode_combine(w0, w1)
    f = lambda t: f"({t[0]}-{t[1]})*{t[2]}+{t[3]}"  # noqa: E731
    s = f"c1: rgb={f(c1rgb)} a={f(c1a)}"
    if two_cycle:
        s += f" | c2: rgb={f(c2rgb)} a={f(c2a)}"
    return s


def combine_inputs(w0: int, w1: int, two_cycle: bool) -> set:
    c1rgb, c1a, c2rgb, c2a = decode_combine(w0, w1)
    used = set(c1rgb) | set(c1a)
    if two_cycle:
        used |= set(c2rgb) | set(c2a)
    return used


def rgba_f(v: int) -> tuple:
    return ((v >> 24) / 255.0, ((v >> 16) & 255) / 255.0, ((v >> 8) & 255) / 255.0, (v & 255) / 255.0)


# ---------------------------------------------------------------------------
# The walker
# ---------------------------------------------------------------------------

class Walker:
    def __init__(self, mem: Rdram, args):
        self.mem = mem
        self.args = args
        self.st = State()
        self.tmem = Tmem()
        self.draws: list[Draw] = []
        self.cur_draw: Draw | None = None
        self.state_dirty = True
        self.ncmds = 0
        self.notes: list[str] = []
        self.op_counts: dict[int, int] = {}
        self.textures_seen: dict = {}
        self.pending_rect: Draw | None = None
        self.pending_rect_words = 0

    # -- helpers ---------------------------------------------------------------
    def seg(self, addr: int) -> int:
        """Resolve a segmented address to a physical one (RSP semantics)."""
        return (self.st.segments[(addr >> 24) & 0xF] + (addr & 0x00FFFFFF)) & 0x00FFFFFF

    def say(self, depth: int, addr: int, w0: int, w1: int, text: str) -> None:
        if self.args.cmds:
            print(f"{'  ' * depth}{addr:08X}: {w0:08X} {w1:08X}  {text}")

    def mvp(self):
        if self.st.proj is None:
            return None
        mv = self.st.mv if self.st.mv is not None else IDENTITY
        return mat_mul(mv, self.st.proj)

    def transform(self, v: Vertex, m) -> None:
        if m is None:
            v.clip = v.screen = None
            return
        x, y, z = v.x, v.y, v.z
        cx = x * m[0][0] + y * m[1][0] + z * m[2][0] + m[3][0]
        cy = x * m[0][1] + y * m[1][1] + z * m[2][1] + m[3][1]
        cz = x * m[0][2] + y * m[1][2] + z * m[2][2] + m[3][2]
        cw = x * m[0][3] + y * m[1][3] + z * m[2][3] + m[3][3]
        v.clip = (cx, cy, cz, cw)
        if self.st.viewport and abs(cw) > 1e-9:
            (sx, sy, sz, _), (tx, ty, tz, _) = self.st.viewport
            v.screen = ((cx / cw) * sx / 4 + tx / 4, (cy / cw) * -sy / 4 + ty / 4, (cz / cw) * sz + tz)
        else:
            v.screen = None

    @staticmethod
    def outcode(v: Vertex) -> int:
        if v.clip is None:
            return 0
        x, y, z, w = v.clip
        c = 0
        if x < -w:
            c |= 1
        if x > w:
            c |= 2
        if y < -w:
            c |= 4
        if y > w:
            c |= 8
        if z > w:
            c |= 16
        if w < 0.0 or z < -w:
            c |= 32
        return c

    # -- draw bookkeeping ------------------------------------------------------
    def mark_dirty(self) -> None:
        self.state_dirty = True

    def snapshot(self) -> Snap:
        st = self.st
        return Snap(tmem=bytes(self.tmem.mem), tiles=[replace(t) for t in st.tiles],
                    othermode_h=st.othermode_h, othermode_l=st.othermode_l, combine=st.combine,
                    geometry=st.geometry, prim=st.prim, prim_minlevel=st.prim_minlevel,
                    prim_lodfrac=st.prim_lodfrac, env=st.env, fog=st.fog, blend=st.blend,
                    tex_on=st.tex_on, tex_tile=st.tex_tile, tex_levels=st.tex_levels,
                    scissor=st.scissor, prim_depth=st.prim_depth)

    def sampled_tiles(self, tile_override: int | None = None) -> list[int]:
        """Tiles the RDP can sample for the current state."""
        st = self.st
        h = st.othermode_h
        two = bits(h, 20, 2) == 1
        base = st.tex_tile if tile_override is None else tile_override
        used = combine_inputs(*st.combine, two_cycle=two)
        want = {base}
        if bits(h, 16, 1):  # LOD: up to prim_tile + max_level + 2 with detail/sharpen
            extra = 2 if bits(h, 17, 2) else 1
            for i in range(st.tex_levels + extra + 1):
                want.add((base + i) & 7)
        elif "TEXEL1" in used or "TEXEL1_ALPHA" in used:
            want.add((base + 1) & 7)
        return sorted(want)

    def state_summary(self, tile_override: int | None = None) -> tuple[str, list]:
        st = self.st
        h, lo = st.othermode_h, st.othermode_l
        two = bits(h, 20, 2) == 1
        lines = [f"geom={fmt_geometry(st.geometry)}",
                 f"omH: {fmt_othermode_h(h, full=False)}",
                 f"omL: {fmt_othermode_l(lo)}",
                 f"cc: {fmt_combine(*st.combine, two_cycle=two)}",
                 f"prim={st.prim:08X} (minlvl={st.prim_minlevel} lodfrac={st.prim_lodfrac}) "
                 f"env={st.env:08X} fog={st.fog:08X} blend={st.blend:08X} "
                 f"fogmul/ofs={s16(st.fog_mul)}/{s16(st.fog_ofs)}",
                 f"G_TEXTURE: on={st.tex_on} tile={st.tex_tile} level={st.tex_levels} "
                 f"scale=0x{st.tex_s:04X},0x{st.tex_t:04X}"]
        tex_info = []
        if st.tex_on or tile_override is not None:
            for t in self.sampled_tiles(tile_override):
                tl = st.tiles[t]
                lines.append(f"  tile{t}: {tl.describe()}")
                ld = st.loads.get(tl.tmem)
                if ld:
                    lines.append(f"     <- {ld.describe()}")
                else:
                    covering = [x for x in st.loads.values() if x.kind != "LOADTLUT"
                                and x.tmem <= tl.tmem < x.tmem + (x.src_bytes + 7) // 8]
                    if covering:
                        lines.append(f"     <- inside {covering[-1].describe()}")
                    else:
                        lines.append("     <- (no load seen this frame covers this TMEM address)")
                tex_info.append((t, replace(tl), ld))
            if FMT[st.tiles[st.tex_tile].fmt] == "CI" or bits(h, 14, 2):
                if st.tlut_load:
                    lines.append(f"  TLUT: {st.tlut_load.describe()}")
        lines.append(f"proj@{st.mtx_addr_proj:08X} mv@{st.mtx_addr_mv:08X}")
        return "\n".join(lines), tex_info

    def new_draw(self, addr: int, kind: str, tile_override: int | None = None) -> Draw:
        text, tex = self.state_summary(tile_override)
        d = Draw(index=len(self.draws), dl_addr=addr, kind=kind, vtx_src=self.st.vtx_src,
                 state_text=text, textures=tex, snap=self.snapshot(), vp=self.st.viewport)
        self.draws.append(d)
        return d

    def begin_tris(self, addr: int, idxs: list[int]) -> None:
        st = self.st
        if self.state_dirty or self.cur_draw is None:
            self.cur_draw = self.new_draw(addr, "tris")
            self.state_dirty = False
        d = self.cur_draw
        d.cmds += 1
        d.tris += len(idxs) // 3
        for k in range(0, len(idxs) - 2, 3):
            vs = [st.vtx[i] for i in idxs[k:k + 3]]
            if all(v.clip is not None for v in vs):
                d.tri_data.append(tuple((*v.clip, v.s, v.t, *v.rgba) for v in vs))
        fog_on = bool(st.geometry & G_FOG)
        for i in idxs:
            v = st.vtx[i]
            if v.clip is not None:
                _, _, cz, cw = v.clip
                d.w_range = (cw, cw) if d.w_range is None else (min(d.w_range[0], cw), max(d.w_range[1], cw))
                if cw <= 0:
                    d.behind += 1
                else:
                    zw = cz / cw
                    d.ndc_z = (zw, zw) if d.ndc_z is None else (min(d.ndc_z[0], zw), max(d.ndc_z[1], zw))
                    if fog_on:
                        fa = v.rgba[3]
                        d.fog_range = (fa, fa) if d.fog_range is None else (min(d.fog_range[0], fa), max(d.fog_range[1], fa))
            sc = v.screen
            if sc is None:
                continue
            if d.screen_bbox is None:
                d.screen_bbox = (sc[0], sc[1], sc[0], sc[1])
            else:
                x0, y0, x1, y1 = d.screen_bbox
                d.screen_bbox = (min(x0, sc[0]), min(y0, sc[1]), max(x1, sc[0]), max(y1, sc[1]))

    # -- texture loading -----------------------------------------------------
    def record_load(self, kind: str, addr: int, tile_idx: int, params: tuple, start: int, nbytes: int) -> None:
        st = self.st
        ld = TmemLoad(kind=kind, dl_addr=addr, timg=st.timg, tile=tile_idx, tmem=st.tiles[tile_idx].tmem,
                      params=params, src_start=start, src_bytes=nbytes)
        if kind == "LOADTLUT":
            st.tlut_load = ld
        st.loads[ld.tmem] = ld
        key = (start, nbytes, kind)
        if key not in self.textures_seen:
            raw = self.mem.read(start, nbytes) if self.mem.valid(start, nbytes) else b""
            self.textures_seen[key] = (ld, byte_stats(raw))

    # -- the main loop -------------------------------------------------------
    def run(self, start: int) -> None:
        stack: list[int] = []
        pc = self.seg(start)
        depth = 0
        while True:
            if not self.mem.valid(pc, 8):
                self.notes.append(f"PC 0x{pc:08X} is outside RDRAM; stopping")
                break
            w0, w1 = self.mem.u32(pc), self.mem.u32(pc + 4)
            addr = pc | 0x80000000
            pc += 8
            self.ncmds += 1
            if self.ncmds > self.args.max_cmds:
                self.notes.append(f"stopped after {self.args.max_cmds} commands (--max-cmds)")
                break
            op = w0 >> 24
            if op == self.st.gex_op:
                sub = w0 & 0xFFFFFF
                words = GEX_WORDS.get(sub, 1)
                self.say(depth, addr, w0, w1, f"G_EX_{GEX_NAMES.get(sub, f'{sub:02X}')} ({words} words) 0x{w1:08X}")
                pc += 8 * (words - 1)
                self.op_counts[0x100] = self.op_counts.get(0x100, 0) + 1
                continue
            self.op_counts[op] = self.op_counts.get(op, 0) + 1
            res = self.exec(op, w0, w1, addr, depth)
            if res is None:
                continue
            kind, target = res
            if kind == "call":
                if len(stack) >= DL_STACK_DEPTH:
                    self.notes.append(f"{addr:08X}: DL stack overflow (depth {len(stack) + 1})")
                stack.append(pc)
                pc = target
                depth += 1
            elif kind == "branch":
                pc = target
            elif kind == "end":
                if not stack:
                    break
                pc = stack.pop()
                depth -= 1

    def exec(self, op: int, w0: int, w1: int, addr: int, depth: int):
        st = self.st
        name = OPNAMES.get(op, f"OP_{op:02X}")
        say = lambda text: self.say(depth, addr, w0, w1, f"{name} {text}")  # noqa: E731
        follow = self.args.follow

        if op == G_VTX:
            n = bits(w0, 12, 8)
            v0 = bits(w0, 1, 7) - n
            src = self.seg(w1)
            st.vtx_src = src
            m = self.mvp()
            fog_on = bool(st.geometry & G_FOG)
            lit = bool(st.geometry & G_LIGHTING)
            lines = []
            for i in range(n):
                if not (0 <= v0 + i < st.vtx_size):
                    self.notes.append(f"{addr:08X}: G_VTX writes outside the vertex buffer")
                    continue
                x, y, z, _flag, s, t, r, g, b, a = struct.unpack(">hhhHhhBBBB", self.mem.read(src + i * 16, 16))
                v = Vertex(x=x, y=y, z=z)
                # F3DEX2 scales ST by the G_TEXTURE scale at load time (vmudm: s * scale >> 16).
                v.s, v.t = (s * st.tex_s) / 65536.0, (t * st.tex_t) / 65536.0
                self.transform(v, m)
                if lit:
                    r = g = b = 160
                if fog_on and v.clip:
                    cz, cw = v.clip[2], v.clip[3]
                    a = 0 if cw == 0 else min(max(max(cz, 0.0) / cw * s16(st.fog_mul) + s16(st.fog_ofs), 0.0), 255.0)
                v.rgba = (r, g, b, a)
                st.vtx[v0 + i] = v
                if self.args.vertices:
                    scr = f" scr=({v.screen[0]:.1f},{v.screen[1]:.1f},{v.screen[2]:.0f})" if v.screen else ""
                    lines.append(f"      v{v0 + i}: pos=({x},{y},{z}) st=({s / 32:g},{t / 32:g}) "
                                 f"rgba/n=({r},{g},{b},{a:.0f}){scr}")
            say(f"n={n} v0={v0} src=0x{src:08X} (seg {w1:08X})")
            if self.args.cmds:
                for line in lines:
                    print(line)
        elif op == G_MODIFYVTX:
            where, vi = bits(w0, 16, 8), bits(w0, 0, 16) // 2
            say(f"vtx={vi} where=0x{where:02X} val=0x{w1:08X}")
            if 0 <= vi < st.vtx_size:
                v = st.vtx[vi]
                if where == 0x10:  # G_MWO_POINT_RGBA
                    v.rgba = tuple(w1.to_bytes(4, "big"))
                elif where == 0x14:  # G_MWO_POINT_ST (already-scaled ST)
                    v.s, v.t = float(s16(w1 >> 16)), float(s16(w1 & 0xFFFF))
                elif where == 0x18 and v.screen:  # G_MWO_POINT_XYSCREEN
                    v.screen = (s16(w1 >> 16) / 4, s16(w1 & 0xFFFF) / 4, v.screen[2])
                elif where == 0x1C and v.screen:  # G_MWO_POINT_ZSCREEN
                    v.screen = (v.screen[0], v.screen[1], w1)
        elif op == G_CULLDL:
            v0, vn = bits(w0, 0, 16) // 2, bits(w1, 0, 16) // 2
            rng = [i for i in range(v0, vn + 1) if 0 <= i < st.vtx_size]
            have = all(st.vtx[i].clip is not None for i in rng)
            common = 0x3F
            for i in rng:
                common &= self.outcode(st.vtx[i])
            culled = have and common != 0
            verdict = ("would CULL (rest of this DL skipped)" if culled else "visible") if have else "unknown"
            say(f"v{v0}..v{vn} -> {verdict}")
            if culled and self.args.cull == "eval":
                return ("end", 0)
        elif op == G_BRANCH_Z:
            vi = bits(w0, 0, 12) // 2
            target = self.seg(st.rdphalf1)
            v = st.vtx[vi] if 0 <= vi < st.vtx_size else None
            vz = None
            if v is not None and v.clip is not None and abs(v.clip[3]) > 1e-9 and st.viewport:
                (_, _, sz, _), (_, _, tz, _) = st.viewport
                vz = int(((v.clip[2] / v.clip[3]) * sz + tz) * 65536)  # screen z, s15.16
            take = {"always": True, "never": False}.get(self.args.branch_z)
            if take is None:
                take = vz is not None and vz <= s32(w1)
            say(f"vtx={vi} zval=0x{w1:08X} target=0x{target:08X} vz={'?' if vz is None else hex(vz)} "
                f"-> {'BRANCH' if take else 'fall through'}")
            if take and follow:
                return ("branch", target)
        elif op in (G_TRI1, G_TRI2, G_QUAD):
            idx = [bits(w0, 16, 8) // 2, bits(w0, 8, 8) // 2, bits(w0, 0, 8) // 2]
            if op != G_TRI1:
                idx += [bits(w1, 16, 8) // 2, bits(w1, 8, 8) // 2, bits(w1, 0, 8) // 2]
            if any(i >= st.vtx_size for i in idx):
                self.notes.append(f"{addr:08X}: triangle index outside the vertex buffer")
                idx = [i for i in idx if i < st.vtx_size]
            say(" ".join(f"({idx[i]},{idx[i + 1]},{idx[i + 2]})" for i in range(0, len(idx) - 2, 3)))
            self.begin_tris(addr, idx)
        elif op == G_LINE3D:
            say(f"w1=0x{w1:08X}")
        elif op == G_DMA_IO:
            say(f"flag={bits(w0, 23, 1)} dmem=0x{bits(w0, 10, 13) * 8:X} size={bits(w0, 0, 12) + 1} "
                f"dram=0x{self.seg(w1):08X}")
        elif op == G_TEXTURE:
            st.tex_levels, st.tex_tile, st.tex_on = bits(w0, 11, 3), bits(w0, 8, 3), bits(w0, 1, 7)
            st.tex_s, st.tex_t = w1 >> 16, w1 & 0xFFFF
            say(f"on={st.tex_on} tile={st.tex_tile} level={st.tex_levels} s=0x{st.tex_s:04X} t=0x{st.tex_t:04X}")
            self.mark_dirty()
        elif op == G_POPMTX:
            n = w1 // 64
            for _ in range(n):
                if st.mv_stack:
                    st.mv, st.mtx_addr_mv = st.mv_stack.pop()
            say(f"pop {n} modelview")
            self.mark_dirty()
        elif op == G_GEOMETRYMODE:
            clear = (~w0) & 0x00FFFFFF
            st.geometry = (st.geometry & ~clear) | w1
            say(f"clear={fmt_geometry(clear)} set={fmt_geometry(w1)} -> {fmt_geometry(st.geometry)}")
            self.mark_dirty()
        elif op == G_MTX:
            p = bits(w0, 0, 8) ^ 0x01  # F3DEX2 stores param ^ G_MTX_PUSH
            push, load, proj = p & 1, p & 2, p & 4
            src = self.seg(w1)
            m = read_mtx(self.mem, src)
            say(f"{'PROJ' if proj else 'MODELVIEW'} {'LOAD' if load else 'MUL'}"
                f"{' PUSH' if push and not proj else ''} src=0x{src:08X} t=({m[3][0]:.1f},{m[3][1]:.1f},{m[3][2]:.1f})")
            if self.args.cmds and self.args.matrices:
                for row in m:
                    print(f"{'  ' * depth}      [{' '.join(f'{x:11.4f}' for x in row)}]")
            if proj:
                st.proj = m if load or st.proj is None else mat_mul(m, st.proj)
                st.mtx_addr_proj = src
            else:
                if push:
                    if len(st.mv_stack) >= MTX_STACK_DEPTH:
                        self.notes.append(f"{addr:08X}: modelview stack overflow")
                    st.mv_stack.append((st.mv, st.mtx_addr_mv))
                st.mv = m if load or st.mv is None else mat_mul(m, st.mv)
                st.mtx_addr_mv = src
            self.mark_dirty()
        elif op == G_MOVEWORD:
            index, offset = bits(w0, 16, 8), bits(w0, 0, 16)
            extra = ""
            if index == 0x06:
                st.segments[(offset // 4) & 0xF] = w1 & 0x00FFFFFF
                extra = f" segment[{offset // 4}] = 0x{w1:08X}"
            elif index == 0x08:
                st.fog_mul, st.fog_ofs = w1 >> 16, w1 & 0xFFFF
                fm, fo = s16(st.fog_mul), s16(st.fog_ofs)
                if fm:
                    near = 500 - fo * 500 / fm
                    far = near + 128000 / fm
                    extra = f" fog mul={fm} ofs={fo} (gSPFogPosition {near:.0f}..{far:.0f})"
                self.mark_dirty()
            elif index == 0x02:
                st.num_lights = w1 // 24
                extra = f" numlights={st.num_lights}"
            elif index == 0x0E:
                st.perspnorm = w1 & 0xFFFF
                extra = f" perspnorm=0x{st.perspnorm:04X}"
            elif index == 0x04:
                edge = {4: "RNX", 0xC: "RNY", 0x14: "RPX", 0x1C: "RPY"}.get(offset, f"0x{offset:X}")
                extra = f" clip ratio {edge} = {s16(w1 & 0xFFFF)}"
            say(f"{MW_NAMES.get(index, f'idx{index:02X}')} ofs=0x{offset:04X} data=0x{w1:08X}{extra}")
        elif op == G_MOVEMEM:
            idx, ofs, size = bits(w0, 0, 8), bits(w0, 8, 8) * 8, (bits(w0, 19, 5) + 1) * 8
            src = self.seg(w1)
            extra = ""
            if idx == 8:
                vals = struct.unpack(">8h", self.mem.read(src, 16))
                st.viewport = (vals[0:4], vals[4:8])
                extra = f" viewport scale={vals[0:4]} trans={vals[4:8]} (x,y in 1/4 px)"
            elif idx == 10:
                extra = f" ({'lookat' if ofs < 48 else 'light ' + str((ofs - 48) // 24)})"
            say(f"{MV_NAMES.get(idx, idx)} ofs={ofs} size={size} src=0x{src:08X}{extra}")
        elif op == G_LOAD_UCODE:
            text = self.seg(w1)
            kind = {UCODE_F3DLX2_REJ_TEXT: "F3DLX2.Rej", UCODE_F3DEX2_TEXT: "F3DEX2"}.get(text)
            st.vtx_size = VTX_BUFFER_REJ if kind == "F3DLX2.Rej" else VTX_BUFFER_SIZE
            say(f"text=0x{text:08X} dsize=0x{bits(w0, 0, 16) + 1:X} data=0x{st.rdphalf1:08X} ({kind or 'unknown microcode'})")
            self.notes.append(f"{addr:08X}: G_LOAD_UCODE text=0x{text:08X} -> {kind or 'unknown (decoded as F3DEX2)'}, "
                              f"vertex buffer {st.vtx_size}")
        elif op == G_DL:
            target = self.seg(w1)
            nopush = bits(w0, 16, 8)
            say(f"{'branch' if nopush else 'call'} 0x{target:08X} (seg {w1:08X})")
            if follow:
                return ("branch" if nopush else "call", target)
        elif op == G_ENDDL:
            say("")
            return ("end", 0)
        elif op in (G_NOOP, 0x00):
            if op == G_NOOP and w0 == RT64_HOOK_W0:
                hook = w1 >> 28
                if hook == 1:
                    st.gex_op = w1 & 0xFF
                elif hook == 2:
                    st.gex_op = -1
                say(f"RT64 hook {['GETVERSION', 'ENABLE', 'DISABLE', 'DL', 'BRANCH'][hook] if hook < 5 else hook} "
                    f"0x{w1:08X}{f' (extended opcode 0x{st.gex_op:02X})' if hook == 1 else ''}")
            else:
                say(f"0x{w1:08X}" if w1 else "")
        elif op in (G_RDPHALF_1, G_RDPHALF_2):
            if op == G_RDPHALF_1:
                st.rdphalf1 = w1
            else:
                st.rdphalf2 = w1
            extra = ""
            if self.pending_rect is not None:
                d = self.pending_rect
                xl, yl, xh, yh, tile, s, t, dsdx, dtdy, flip, fc = d.rect
                if op == G_RDPHALF_1:
                    s, t = s16(w1 >> 16) / 32.0, s16(w1 & 0xFFFF) / 32.0
                    extra = f"  (texrect s={s:g} t={t:g})"
                else:
                    dsdx, dtdy = s16(w1 >> 16) / 1024.0, s16(w1 & 0xFFFF) / 1024.0
                    extra = f"  (texrect dsdx={dsdx:g} dtdy={dtdy:g})"
                    self.pending_rect = None
                d.rect = (xl, yl, xh, yh, tile, s, t, dsdx, dtdy, flip, fc)
            say(f"0x{w1:08X}{extra}")
        elif op in (G_SETOTHERMODE_L, G_SETOTHERMODE_H):
            length = bits(w0, 0, 8) + 1
            shift = max(0, 32 - bits(w0, 8, 8) - length)
            mask = ((1 << length) - 1) << shift
            if op == G_SETOTHERMODE_H:
                st.othermode_h = (st.othermode_h & ~mask) | (w1 & mask)
                val = (w1 & mask) >> shift
                fld = next(((n, vals) for n, sh, nb, vals in OMH_FIELDS if sh == shift and nb == length), None)
                if fld:
                    say(f"{fld[0]} = {fld[1][val] if val < len(fld[1]) else val}  -> H=0x{st.othermode_h:08X}")
                else:
                    say(f"bits {shift}..{shift + length - 1} = 0x{val:X}  -> H=0x{st.othermode_h:08X}")
            else:
                st.othermode_l = (st.othermode_l & ~mask) | (w1 & mask)
                if shift == 3 and length == 29:
                    say(f"rendermode = {fmt_rendermode(st.othermode_l)}  -> L=0x{st.othermode_l:08X}")
                else:
                    say(f"bits {shift}..{shift + length - 1} = 0x{(w1 & mask) >> shift:X}  -> L=0x{st.othermode_l:08X}")
            self.mark_dirty()
        elif op in (G_TEXRECT, G_TEXRECTFLIP):
            xh, yh = bits(w0, 12, 12) / 4, bits(w0, 0, 12) / 4
            tile, xl, yl = bits(w1, 24, 3), bits(w1, 12, 12) / 4, bits(w1, 0, 12) / 4
            say(f"tile={tile} ({xl:g},{yl:g})-({xh:g},{yh:g})")
            self.cur_draw = None
            self.mark_dirty()
            d = self.new_draw(addr, "texrect", tile_override=tile)
            d.cmds = 1
            d.screen_bbox = (xl, yl, xh, yh)
            d.rect = (xl, yl, xh, yh, tile, 0.0, 0.0, 1.0, 1.0, op == G_TEXRECTFLIP, 0)
            self.pending_rect = d
        elif op in (G_RDPLOADSYNC, G_RDPPIPESYNC, G_RDPTILESYNC, G_RDPFULLSYNC):
            say("")
        elif op in (G_SETKEYGB, G_SETKEYR, G_SETCONVERT):
            say(f"w0=0x{w0 & 0xFFFFFF:06X} w1=0x{w1:08X}")
        elif op == G_SETSCISSOR:
            st.scissor = (bits(w0, 12, 12) / 4, bits(w0, 0, 12) / 4, bits(w1, 12, 12) / 4, bits(w1, 0, 12) / 4)
            say(f"mode={bits(w1, 24, 2)} ({st.scissor[0]:g},{st.scissor[1]:g})-({st.scissor[2]:g},{st.scissor[3]:g})")
            self.mark_dirty()
        elif op == G_SETPRIMDEPTH:
            st.prim_depth = (w1 >> 16, w1 & 0xFFFF)
            say(f"z=0x{w1 >> 16:04X} dz=0x{w1 & 0xFFFF:04X}")
            self.mark_dirty()
        elif op == G_RDPSETOTHERMODE:
            st.othermode_h = (w0 & 0x00FFFFFF) | (st.othermode_h & 0xFF000000)
            st.othermode_l = w1
            say(f"H=0x{st.othermode_h:08X} L=0x{w1:08X}")
            self.mark_dirty()
        elif op in (G_LOADTLUT, G_SETTILESIZE, G_LOADTILE, G_LOADBLOCK):
            sl, tl = bits(w0, 12, 12), bits(w0, 0, 12)
            tile, sh, th = bits(w1, 24, 3), bits(w1, 12, 12), bits(w1, 0, 12)
            t = st.tiles[tile]
            if op == G_SETTILESIZE:
                t.uls, t.ult, t.lrs, t.lrt = sl, tl, sh, th
                say(f"tile={tile} ({sl / 4:g},{tl / 4:g})-({sh / 4:g},{th / 4:g})")
            elif op == G_LOADBLOCK:
                start, nbytes = self.tmem.load_block(self.mem, st.timg, t, sl, tl, sh, th)
                say(f"tile={tile} sl={sl} tl={tl} texels={sh - sl + 1} dxt=0x{th:03X} "
                    f"({nbytes} bytes from 0x{start:08X} -> tmem=0x{t.tmem:03X})")
                self.record_load("LOADBLOCK", addr, tile, (sl, tl, sh, th), start, nbytes)
            elif op == G_LOADTILE:
                start, nbytes = self.tmem.load_tile(self.mem, st.timg, t, sl, tl, sh, th)
                say(f"tile={tile} ({sl / 4:g},{tl / 4:g})-({sh / 4:g},{th / 4:g}) "
                    f"from 0x{start:08X} -> tmem=0x{t.tmem:03X} line={t.line}")
                self.record_load("LOADTILE", addr, tile, (sl / 4, tl / 4, sh / 4, th / 4), start, nbytes)
            else:
                start, nbytes = self.tmem.load_tlut(self.mem, st.timg, t, sl, tl, sh, th)
                say(f"tile={tile} count={nbytes // 2} from 0x{start:08X} -> tmem=0x{t.tmem:03X}")
                self.record_load("LOADTLUT", addr, tile, (nbytes // 2,), start, nbytes)
            self.mark_dirty()
        elif op == G_SETTILE:
            tile = bits(w1, 24, 3)
            t = st.tiles[tile]
            t.fmt, t.siz, t.line, t.tmem = bits(w0, 21, 3), bits(w0, 19, 2), bits(w0, 9, 9), bits(w0, 0, 9)
            t.palette, t.cmt, t.maskt, t.shiftt = bits(w1, 20, 4), bits(w1, 18, 2), bits(w1, 14, 4), bits(w1, 10, 4)
            t.cms, t.masks, t.shifts = bits(w1, 8, 2), bits(w1, 4, 4), bits(w1, 0, 4)
            say(f"tile={tile} {FMT[t.fmt]}{SIZ[t.siz]} line={t.line} tmem=0x{t.tmem:03X} pal={t.palette} "
                f"cmt={t.cmt} maskt={t.maskt} shiftt={t.shiftt} cms={t.cms} masks={t.masks} shifts={t.shifts}")
            self.mark_dirty()
        elif op == G_FILLRECT:
            xh, yh = bits(w0, 12, 12) / 4, bits(w0, 0, 12) / 4
            xl, yl = bits(w1, 12, 12) / 4, bits(w1, 0, 12) / 4
            say(f"({xl:g},{yl:g})-({xh:g},{yh:g}) cimg=0x{st.cimg[3]:08X} fill=0x{st.fill:08X}")
            self.cur_draw = None
            self.mark_dirty()
            d = self.new_draw(addr, "fillrect")
            d.cmds = 1
            d.screen_bbox = (xl, yl, xh, yh)
            d.rect = (xl, yl, xh, yh, 0, 0.0, 0.0, 0.0, 0.0, False, st.fill if st.cimg[3] != st.zimg else None)
        elif op == G_SETFILLCOLOR:
            st.fill = w1
            say(f"0x{w1:08X}")
        elif op in (G_SETFOGCOLOR, G_SETBLENDCOLOR, G_SETENVCOLOR):
            if op == G_SETFOGCOLOR:
                st.fog = w1
            elif op == G_SETBLENDCOLOR:
                st.blend = w1
            else:
                st.env = w1
            say(f"{w1:08X}")
            self.mark_dirty()
        elif op == G_SETPRIMCOLOR:
            st.prim, st.prim_minlevel, st.prim_lodfrac = w1, bits(w0, 8, 8), bits(w0, 0, 8)
            say(f"{w1:08X} minlevel={st.prim_minlevel} lodfrac={st.prim_lodfrac}")
            self.mark_dirty()
        elif op == G_SETCOMBINE:
            st.combine = (w0 & 0x00FFFFFF, w1)
            say(fmt_combine(*st.combine))
            self.mark_dirty()
        elif op in (G_SETTIMG, G_SETZIMG, G_SETCIMG):
            fmt, siz, width = bits(w0, 21, 3), bits(w0, 19, 2), bits(w0, 0, 12) + 1
            a = self.seg(w1)
            if op == G_SETTIMG:
                st.timg = (fmt, siz, width, a)
                note = "  ** framebuffer as texture (== current CIMG) **" if st.cimg[3] and a == st.cimg[3] else ""
                say(f"{FMT[fmt]}{SIZ[siz]} w={width} 0x{a:08X}{note}")
            elif op == G_SETCIMG:
                st.cimg = (fmt, siz, width, a)
                say(f"{FMT[fmt]}{SIZ[siz]} w={width} 0x{a:08X}")
            else:
                st.zimg = a
                say(f"0x{a:08X}")
        elif op in (G_SPECIAL_1, G_SPECIAL_2, G_SPECIAL_3):
            say(f"0x{w1:08X}")
        else:
            say(f"(not an F3DEX2 command) 0x{w1:08X}")
        return None


# ---------------------------------------------------------------------------
# Software rasterizer (approximate; for identifying draws, not for accuracy)
# ---------------------------------------------------------------------------

class Renderer:
    """Rasterize the captured draws into a color buffer.

    Point-sampled textures from the emulated TMEM, real combiner equations
    (both cycles), blender (incl. fog and the FORCE_BL rule), z-buffer with
    Z_CMP/Z_UPD, alpha compare, near/far clipping, back/front face culling.
    No coverage/AA, dithering, bilinear filtering or lighting.

    lod_model: 'rdp' selects tiles and LOD fraction as the hardware does
    (angrylion tclod), 'rt64' as RT64's computeLOD() in TextureSampler.hlsli
    (tiles relative to G_TEXTURE's tile, clamped to its level count).
    """

    def __init__(self, width: int, height: int, lod_model: str = "rdp"):
        self.w, self.h = width, height
        self.color = [(0.0, 0.0, 0.0)] * (width * height)
        self.z = [float("inf")] * (width * height)
        self.ids = [-1] * (width * height)
        self.lod_model = lod_model
        self.stats: dict[int, int] = {}

    # -- texture/LOD ---------------------------------------------------------
    def texels(self, snap: Snap, s: float, t: float, lod: float, tlut: int, tile_base: int) -> tuple:
        """Return (texel0, texel1, lod_fraction) for one pixel, s/t in texels."""
        h = snap.othermode_h
        tiles = snap.tiles
        prim_tile = tile_base
        lod_frac = 0.0
        t0i, t1i = prim_tile, (prim_tile + 1) & 7
        if bits(h, 16, 1):  # textlod = LOD
            sharpen = bits(h, 17, 2) == 1
            detail = bits(h, 17, 2) == 2
            max_level = snap.tex_levels
            if self.lod_model == "rdp":
                min_level = (snap.prim_minlevel & 0x1F) / 32.0
                lod = max(lod, min_level)
                magnify = lod < 1.0
                l_tile = 0 if lod < 1.0 else min(int(math.floor(math.log2(lod))), 7)
                distant = l_tile >= max_level
                lod_frac = (lod / (1 << l_tile)) % 1.0 if not magnify else lod
                if not sharpen and not detail:
                    if distant:
                        lod_frac = 1.0
                    elif magnify:
                        lod_frac = 0.0
                if distant:
                    l_tile = max_level
                if not detail:
                    t0i = (prim_tile + l_tile) & 7
                    t1i = t0i if (distant or (not sharpen and magnify)) else (t0i + 1) & 7
                else:
                    t0i = prim_tile if magnify else (prim_tile + l_tile + 1) & 7
                    t1i = ((prim_tile + l_tile + 1) if (distant or magnify) else (prim_tile + l_tile + 2)) & 7
            else:  # RT64 computeLOD()
                tile_max = (max_level + 1) - 1
                max_dst = lod
                if detail or sharpen:
                    max_dst = max(max_dst, (snap.prim_minlevel & 0x1F) / 32.0)
                tb = math.floor(math.log2(max_dst)) if max_dst > 0 else -64
                lod_frac = max_dst / (2 ** max(tb, 0)) - 1.0
                if sharpen and max_dst < 1.0:
                    lod_frac = max_dst - 1.0
                if detail:
                    if lod_frac < 0.0:
                        lod_frac = max_dst
                    tb += 1
                elif tb >= tile_max:
                    lod_frac = 1.0
                if detail or sharpen:
                    tb = max(tb, 0)
                else:
                    lod_frac = max(lod_frac, 0.0)
                r0 = min(max(tb, 0), tile_max)
                r1 = min(max(tb + 1, 0), tile_max)
                t0i, t1i = (prim_tile + r0) & 7, (prim_tile + r1) & 7
        c0 = sample_tile(snap.tmem, tiles[t0i], s, t, tlut)
        c1 = sample_tile(snap.tmem, tiles[t1i], s, t, tlut)
        return c0, c1, lod_frac

    # -- one pixel -------------------------------------------------------------
    def shade_pixel(self, snap: Snap, cc, shade: tuple, tex0: tuple, tex1: tuple, lod_frac: float,
                    mem: tuple, two: bool):
        prim, env = rgba_f(snap.prim), rgba_f(snap.env)
        t0 = (tex0[0] / 255.0, tex0[1] / 255.0, tex0[2] / 255.0, tex0[3] / 255.0)
        t1 = (tex1[0] / 255.0, tex1[1] / 255.0, tex1[2] / 255.0, tex1[3] / 255.0)
        plf = snap.prim_lodfrac / 255.0
        comb = (0.0, 0.0, 0.0, 0.0)

        def val(name, alpha, tex_a, tex_b):
            if name == "COMBINED":
                return comb[3] if alpha else comb[:3]
            if name == "TEXEL0":
                return tex_a[3] if alpha else tex_a[:3]
            if name == "TEXEL1":
                return tex_b[3] if alpha else tex_b[:3]
            if name == "PRIMITIVE":
                return prim[3] if alpha else prim[:3]
            if name == "SHADE":
                return shade[3] if alpha else shade[:3]
            if name == "ENVIRONMENT":
                return env[3] if alpha else env[:3]
            if name == "1":
                return 1.0 if alpha else (1.0, 1.0, 1.0)
            if name == "NOISE":
                return 0.5 if alpha else (0.5, 0.5, 0.5)
            scalar = {"COMBINED_ALPHA": comb[3], "TEXEL0_ALPHA": tex_a[3], "TEXEL1_ALPHA": tex_b[3],
                      "PRIMITIVE_ALPHA": prim[3], "SHADE_ALPHA": shade[3], "ENV_ALPHA": env[3],
                      "LOD_FRACTION": lod_frac, "PRIM_LOD_FRAC": plf}.get(name, 0.0)
            return scalar if alpha else (scalar, scalar, scalar)

        cycles = (cc[0:2], cc[2:4]) if two else (cc[0:2],)
        for ci, (rgb, alp) in enumerate(cycles):
            ta, tb = (t0, t1) if ci == 0 else (t1, t0)  # cycle 2 sees TEXEL0/1 swapped on hardware
            A, B, C, D = (val(n, False, ta, tb) for n in rgb)
            r = [min(max((A[k] - B[k]) * C[k] + D[k], 0.0), 1.0) for k in range(3)]
            Aa, Ba, Ca, Da = (val(n, True, ta, tb) for n in alp)
            a = min(max((Aa - Ba) * Ca + Da, 0.0), 1.0)
            comb = (r[0], r[1], r[2], a)
        return comb

    def blend(self, snap: Snap, comb: tuple, shade_a: float, mem: tuple, two: bool) -> tuple:
        lo = snap.othermode_l
        fog, bl = rgba_f(snap.fog), rgba_f(snap.blend)
        force = bool(lo & RM_FORCE_BL)

        def cycle(n, inp, inp_a):
            P, A, M, B = bits(lo, 30 - 2 * n, 2), bits(lo, 26 - 2 * n, 2), bits(lo, 22 - 2 * n, 2), bits(lo, 18 - 2 * n, 2)
            pick = lambda k: (inp, mem, bl[:3], fog[:3])[k]  # noqa: E731
            a = (inp_a, fog[3], shade_a, 0.0)[A]
            b = (1.0 - a, 1.0, 1.0, 0.0)[B]
            p, m = pick(P), pick(M)
            return tuple(min(max(p[k] * a + m[k] * b, 0.0), 1.0) for k in range(3)), pick(P)

        rgb = comb[:3]
        if two:
            blended, _ = cycle(0, rgb, comb[3])  # first blender cycle always blends in 2-cycle mode
            res, p_only = cycle(1, blended, comb[3])
            return res if force else p_only
        res, p_only = cycle(0, rgb, comb[3])
        return res if force else p_only

    # -- triangles -------------------------------------------------------------
    @staticmethod
    def clip_poly(poly: list, plane) -> list:
        out = []
        n = len(poly)
        for i in range(n):
            a, b = poly[i], poly[(i + 1) % n]
            da, db = plane(a), plane(b)
            if da >= 0:
                out.append(a)
            if (da >= 0) != (db >= 0):
                f = da / (da - db)
                out.append(tuple(a[k] + (b[k] - a[k]) * f for k in range(len(a))))
        return out

    def draw(self, d: Draw, vp: tuple) -> None:
        snap = d.snap
        h, lo, geom = snap.othermode_h, snap.othermode_l, snap.geometry
        cyc = bits(h, 20, 2)
        if d.kind == "fillrect":
            self.fill_rect(d)
            return
        if d.kind == "texrect":
            self.tex_rect(d)
            return
        two = cyc == 1
        cc = decode_combine(*snap.combine)
        textured = snap.tex_on != 0
        tlut = bits(h, 14, 2)
        persp = bits(h, 19, 1)
        uses_lod = bits(h, 16, 1)
        zcmp = (geom & G_ZBUFFER) and (lo & RM_Z_CMP)
        zupd = (geom & G_ZBUFFER) and (lo & RM_Z_UPD)
        zdec = bits(lo, 10, 2) == 3
        acmp = bits(lo, 0, 2)
        cvg_x_alpha = lo & RM_CVG_X_ALPHA
        blend_a = rgba_f(snap.blend)[3]
        (sx, sy, sz, _), (tx, ty, tz, _) = vp
        x0s, y0s, x1s, y1s = snap.scissor
        x0s, y0s = max(int(x0s), 0), max(int(y0s), 0)
        x1s, y1s = min(int(math.ceil(x1s)), self.w), min(int(math.ceil(y1s)), self.h)
        for tri in d.tri_data:
            poly = list(tri)
            poly = self.clip_poly(poly, lambda v: v[3] + v[2])         # near: z >= -w
            poly = self.clip_poly(poly, lambda v: v[3] - v[2]) if poly else poly  # far: z <= w
            poly = self.clip_poly(poly, lambda v: v[3] - 1e-4) if poly else poly
            if len(poly) < 3:
                continue
            pts = []
            for v in poly:
                cx, cy, cz, cw, s, t, r, g, b, a = v
                iw = 1.0 / cw
                pts.append((cx * iw * sx / 4 + tx / 4, -cy * iw * sy / 4 + ty / 4, cz * iw,
                            iw, s / 32.0, t / 32.0, r / 255.0, g / 255.0, b / 255.0, a / 255.0))
            # Cull on the whole (convex, planar) polygon's screen-space winding.
            area = 0.0
            for i in range(len(pts)):
                xa, ya = pts[i][0], pts[i][1]
                xb, yb = pts[(i + 1) % len(pts)][0], pts[(i + 1) % len(pts)][1]
                area += xa * yb - xb * ya
            if (geom & G_CULL_BACK) and area > 0:
                continue
            if (geom & G_CULL_FRONT) and area < 0:
                continue
            for i in range(1, len(pts) - 1):
                self.raster(d, pts[0], pts[i], pts[i + 1], cc, two, textured, tlut, persp, uses_lod,
                            zcmp, zupd, zdec, acmp, cvg_x_alpha, blend_a, (x0s, y0s, x1s, y1s))

    def raster(self, d, p0, p1, p2, cc, two, textured, tlut, persp, uses_lod, zcmp, zupd, zdec,
               acmp, cvg_x_alpha, blend_a, sc) -> None:
        snap = d.snap
        x0s, y0s, x1s, y1s = sc
        ax, ay = p0[0], p0[1]
        bx, by = p1[0], p1[1]
        cx, cy = p2[0], p2[1]
        den = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy)
        if abs(den) < 1e-9:
            return
        xmin, xmax = max(int(math.floor(min(ax, bx, cx))), x0s), min(int(math.ceil(max(ax, bx, cx))), x1s - 1)
        ymin, ymax = max(int(math.floor(min(ay, by, cy))), y0s), min(int(math.ceil(max(ay, by, cy))), y1s - 1)
        if xmin > xmax or ymin > ymax:
            return
        # attribute vectors: z, iw, s*iw, t*iw (or s, t), r, g, b, a
        def attrs(p):
            iw = p[3]
            if persp:
                return (p[2], iw, p[4] * iw, p[5] * iw, p[6], p[7], p[8], p[9])
            return (p[2], iw, p[4], p[5], p[6], p[7], p[8], p[9])
        A0, A1, A2 = attrs(p0), attrs(p1), attrs(p2)
        inv = 1.0 / den
        width = self.w
        did = d.index
        tile_base = snap.tex_tile

        def interp(px, py):
            l0 = ((by - cy) * (px - cx) + (cx - bx) * (py - cy)) * inv
            l1 = ((cy - ay) * (px - cx) + (ax - cx) * (py - cy)) * inv
            l2 = 1.0 - l0 - l1
            return l0, l1, l2

        def st_at(px, py):
            l0, l1, l2 = interp(px, py)
            s = A0[2] * l0 + A1[2] * l1 + A2[2] * l2
            t = A0[3] * l0 + A1[3] * l1 + A2[3] * l2
            if persp:
                iw = A0[1] * l0 + A1[1] * l1 + A2[1] * l2
                if iw <= 0:
                    return s, t
                return s / iw, t / iw
            return s, t

        for y in range(ymin, ymax + 1):
            py = y + 0.5
            row = y * width
            for x in range(xmin, xmax + 1):
                px = x + 0.5
                l0, l1, l2 = interp(px, py)
                if l0 < 0 or l1 < 0 or l2 < 0:
                    continue
                z = A0[0] * l0 + A1[0] * l1 + A2[0] * l2
                i = row + x
                if zcmp:
                    if zdec:
                        if abs(z - self.z[i]) > 2e-3:
                            continue
                    elif z > self.z[i]:
                        continue
                shade = (A0[4] * l0 + A1[4] * l1 + A2[4] * l2, A0[5] * l0 + A1[5] * l1 + A2[5] * l2,
                         A0[6] * l0 + A1[6] * l1 + A2[6] * l2, A0[7] * l0 + A1[7] * l1 + A2[7] * l2)
                tex0 = tex1 = (0, 0, 0, 255)
                lod_frac = 0.0
                if textured:
                    s, t = st_at(px, py)
                    lod = 0.0
                    if uses_lod:
                        s1, t1 = st_at(px + 1, py)
                        s2, t2 = st_at(px, py + 1)
                        lod = max(abs(s1 - s), abs(t1 - t), abs(s2 - s), abs(t2 - t))
                    tex0, tex1, lod_frac = self.texels(snap, s, t, lod, tlut, tile_base)
                comb = self.shade_pixel(snap, cc, shade, tex0, tex1, lod_frac, self.color[i], two)
                if acmp == 1 and comb[3] < blend_a:
                    continue
                if acmp == 3 and comb[3] < 0.5:
                    continue
                if cvg_x_alpha and comb[3] < 1.0 / 8:
                    continue
                self.color[i] = self.blend(snap, comb, shade[3], self.color[i], two)
                self.ids[i] = did
                if zupd:
                    self.z[i] = z

    def tex_rect(self, d: Draw) -> None:
        snap = d.snap
        xl, yl, xh, yh, tile, s0, t0, dsdx, dtdy, flip, _ = d.rect
        cyc = bits(snap.othermode_h, 20, 2)
        copy = cyc == 2
        if copy:
            dsdx /= 4.0  # copy mode steps 4 texels per pixel in the rect's dsdx encoding
        cc = decode_combine(*snap.combine)
        tlut = bits(snap.othermode_h, 14, 2)
        tl = snap.tiles[tile]
        x0, y0 = max(int(xl), 0), max(int(yl), 0)
        x1, y1 = min(int(xh) + (1 if copy or cyc == 3 else 0), self.w), min(int(yh) + (1 if copy else 0), self.h)
        for y in range(y0, y1):
            for x in range(x0, x1):
                ds, dt = (x - xl), (y - yl)
                if flip:
                    ds, dt = dt, ds
                s, t = s0 + ds * dsdx, t0 + dt * dtdy
                tex = sample_tile(snap.tmem, tl, s, t, tlut)
                i = y * self.w + x
                if copy:
                    if (snap.othermode_l & 3) and tex[3] == 0:
                        continue
                    self.color[i] = (tex[0] / 255.0, tex[1] / 255.0, tex[2] / 255.0)
                else:
                    comb = self.shade_pixel(snap, cc, (1.0, 1.0, 1.0, 1.0), tex, tex, 0.0, self.color[i], cyc == 1)
                    if (snap.othermode_l & 3) == 1 and comb[3] < rgba_f(snap.blend)[3]:
                        continue
                    if (snap.othermode_l & RM_CVG_X_ALPHA) and comb[3] < 1.0 / 8:
                        continue
                    self.color[i] = self.blend(snap, comb, 1.0, self.color[i], cyc == 1)
                self.ids[i] = d.index

    def fill_rect(self, d: Draw) -> None:
        xl, yl, xh, yh, *_rest, fill = d.rect
        if fill is None:
            return  # z-buffer clear
        c = rgba5551(fill >> 16)
        col = (c[0] / 255.0, c[1] / 255.0, c[2] / 255.0)
        for y in range(max(int(yl), 0), min(int(yh) + 1, self.h)):
            for x in range(max(int(xl), 0), min(int(xh) + 1, self.w)):
                self.color[y * self.w + x] = col
                self.ids[y * self.w + x] = d.index

    def finish(self) -> None:
        for i in self.ids:
            if i >= 0:
                self.stats[i] = self.stats.get(i, 0) + 1


def id_color(i: int) -> tuple:
    if i < 0:
        return (0, 0, 0)
    hue = (i * 0.618033988749895) % 1.0
    sat, val = 0.65 + 0.35 * ((i * 7) % 3) / 2, 0.75 + 0.25 * ((i * 5) % 2)
    k = int(hue * 6)
    f = hue * 6 - k
    p, q, tt = val * (1 - sat), val * (1 - f * sat), val * (1 - (1 - f) * sat)
    r, g, b = [(val, tt, p), (q, val, p), (p, val, tt), (p, q, val), (tt, p, val), (val, p, q)][k % 6]
    return (int(r * 255), int(g * 255), int(b * 255))


# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------

def print_draws(w: Walker, only: set[int] | None) -> None:
    prev_text = None
    for d in w.draws:
        if only is not None and d.index not in only:
            continue
        bbox = ""
        if d.screen_bbox:
            x0, y0, x1, y1 = d.screen_bbox
            bbox = f" screen=({x0:.0f},{y0:.0f})-({x1:.0f},{y1:.0f})"
        what = {"texrect": "TEXRECT", "fillrect": "FILLRECT"}.get(d.kind, f"{d.tris} tris in {d.cmds} cmds")
        if d.kind == "texrect" and d.rect:
            what += f" s,t=({d.rect[5]:g},{d.rect[6]:g}) ds,dt=({d.rect[7]:g},{d.rect[8]:g}){' FLIP' if d.rect[9] else ''}"
        print(f"== draw {d.index} @{d.dl_addr:08X}: {what}, vtx from 0x{d.vtx_src:08X}{bbox}")
        depth = []
        if d.ndc_z:
            depth.append(f"z/w=[{d.ndc_z[0]:.4f},{d.ndc_z[1]:.4f}]")
        if d.w_range:
            depth.append(f"w=[{d.w_range[0]:.1f},{d.w_range[1]:.1f}]")
        if d.behind:
            depth.append(f"{d.behind} vertex refs with w<=0")
        if d.fog_range:
            depth.append(f"fog alpha=[{d.fog_range[0]:.0f},{d.fog_range[1]:.0f}]")
        if depth:
            print("   " + " ".join(depth))
        if w.args.diff_state and prev_text is not None:
            old = set(prev_text.splitlines())
            for line in d.state_text.splitlines():
                if line not in old:
                    print(f"   {line}")
        else:
            for line in d.state_text.splitlines():
                print(f"   {line}")
        prev_text = d.state_text


def print_textures(w: Walker) -> None:
    print(f"== {len(w.textures_seen)} distinct TMEM loads")
    for (_start, _n, _k), (ld, stats) in sorted(w.textures_seen.items(), key=lambda kv: kv[1][0].dl_addr):
        print(f"   {ld.describe()}: {stats}")


def dump_textures(w: Walker, outdir: Path, only: set[int] | None) -> None:
    """Write every tile each draw can sample, decoded from the emulated TMEM, as PNG."""
    outdir.mkdir(parents=True, exist_ok=True)
    done = set()
    n = 0
    for d in w.draws:
        if only is not None and d.index not in only:
            continue
        if d.snap is None:
            continue
        tlut = bits(d.snap.othermode_h, 14, 2)
        for tidx, tile, _ld in d.textures:
            tw, th = tile.sample_size()
            if tw * th > 1 << 18:
                continue
            key = (d.snap.tmem, tile.fmt, tile.siz, tile.line, tile.tmem, tile.palette, tw, th, tlut)
            if key in done:
                continue
            done.add(key)
            px = [tmem_texel(d.snap.tmem, tile, s, t, tlut) for t in range(th) for s in range(tw)]
            name = f"d{d.index:04d}_tile{tidx}_{FMT[tile.fmt]}{SIZ_BITS[tile.siz]}_tmem{tile.tmem:03X}_{tw}x{th}.png"
            write_png(outdir / name, tw, th, px)
            n += 1
    print(f"{n} textures written to {outdir}")


def render(w: Walker, path: Path, only: set[int] | None, lod_model: str, scale: int) -> None:
    # Output size from the first scissor seen (the game's full-screen scissor).
    sc = next((d.snap.scissor for d in w.draws if d.snap), (0, 0, 320, 240))
    width, height = int(sc[2]), int(sc[3])
    r = Renderer(width, height, lod_model)
    for d in w.draws:
        if only is not None and d.index not in only:
            continue
        if d.kind == "tris" and d.vp is None:
            continue
        r.draw(d, d.vp)
    r.finish()
    px = [(int(c[0] * 255), int(c[1] * 255), int(c[2] * 255)) for c in r.color]
    if scale > 1:
        px = [px[(y // scale) * width + (x // scale)] for y in range(height * scale) for x in range(width * scale)]
    write_png(path, width * scale, height * scale, px)
    ids_path = path.with_name(path.stem + "_ids" + path.suffix)
    ipx = [id_color(i) for i in r.ids]
    if scale > 1:
        ipx = [ipx[(y // scale) * width + (x // scale)] for y in range(height * scale) for x in range(width * scale)]
    write_png(ids_path, width * scale, height * scale, ipx)
    print(f"rendered {width}x{height} -> {path} and {ids_path} (lod model: {lod_model})")
    print("visible pixels per draw (draw: pixels, id color):")
    for i in sorted(r.stats):
        c = id_color(i)
        print(f"   draw {i}: {r.stats[i]} px  #{c[0]:02X}{c[1]:02X}{c[2]:02X}")


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Decode (and roughly render) an F3DEX2 display list from an N64 RDRAM snapshot.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Usage examples:" + __doc__.split("Usage examples:")[1])
    ap.add_argument("dump", type=Path, help="RDRAM snapshot (4 or 8 MB)")
    ap.add_argument("start", type=lambda x: int(x, 16), help="display list address (hex; virtual, physical or segmented)")
    ap.add_argument("--be", action="store_true", help="dump is plain big-endian (default: N64ModernRuntime word-swapped)")
    ap.add_argument("--cmds", action="store_true", help="print every command decoded (default if nothing else is asked)")
    ap.add_argument("--vertices", action="store_true", help="with --cmds, print each loaded vertex")
    ap.add_argument("--matrices", action="store_true", help="with --cmds, print each matrix")
    ap.add_argument("--draws", action="store_true", help="print a per-draw summary with the full render state")
    ap.add_argument("--diff-state", action="store_true", help="with --draws, print only the state lines that changed")
    ap.add_argument("--only-draws", metavar="LIST", help="limit --draws/--render/--dump-textures to draws, e.g. 0-21,40")
    ap.add_argument("--textures", action="store_true", help="list every TMEM load with its source and data stats")
    ap.add_argument("--dump-textures", type=Path, metavar="DIR", help="decode every tile the draws sample into PNGs")
    ap.add_argument("--render", type=Path, metavar="PNG", help="software-render the frame (also writes PNG_ids)")
    ap.add_argument("--render-scale", type=int, default=2, help="integer upscale for --render output (default 2)")
    ap.add_argument("--lod-model", choices=["rdp", "rt64"], default="rdp",
                    help="tile/LOD selection for --render: hardware RDP (default) or RT64's computeLOD()")
    ap.add_argument("--no-follow", dest="follow", action="store_false", help="do not follow G_DL / G_BRANCH_Z")
    ap.add_argument("--cull", choices=["never", "eval"], default="eval",
                    help="G_CULLDL: 'eval' ends the DL when all vertices are outside one plane (default)")
    ap.add_argument("--branch-z", choices=["eval", "always", "never"], default="eval",
                    help="G_BRANCH_Z: evaluate the depth test (default), always branch, or never branch")
    ap.add_argument("--max-cmds", type=int, default=200000, help="safety limit on commands decoded")
    ap.add_argument("--segment", action="append", default=[], metavar="N=ADDR",
                    help="preset a segment base (hex), e.g. --segment 6=80123000")
    args = ap.parse_args()
    if not (args.cmds or args.draws or args.textures or args.dump_textures or args.render):
        args.cmds = True
    only = parse_ranges(args.only_draws) if args.only_draws else None

    mem = Rdram(args.dump, word_swapped=not args.be)
    w = Walker(mem, args)
    for s in args.segment:
        n, a = s.split("=")
        w.st.segments[int(n, 0) & 0xF] = int(a, 16) & 0x00FFFFFF
    w.run(args.start)

    if args.draws:
        print_draws(w, only)
    if args.textures:
        print_textures(w)
    if args.dump_textures:
        dump_textures(w, args.dump_textures, only)
    if args.render:
        render(w, args.render, only, args.lod_model, args.render_scale)
    summary = ", ".join(f"{OPNAMES.get(op, hex(op))}={n}" for op, n in sorted(w.op_counts.items()))
    print(f"== {w.ncmds} commands, {len(w.draws)} draws. {summary}", file=sys.stderr)
    for n in w.notes:
        print(f"note: {n}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
