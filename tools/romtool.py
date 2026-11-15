#!/usr/bin/env python3
"""ROM intake for the Blitz recomp: identify, normalize to .z64, pin, verify.

Commands:
  scan               Identify every ROM file in rom/. Fails if more than one
                     distinct Blitz game is present and no target is pinned.
  import [--target ID] [--force]
                     Convert the chosen dump to big-endian and write
                     build/baserom.z64, then pin its hashes in
                     config/target_rom.json.
  verify             Check build/baserom.z64 against the pin. The build calls
                     this before recompiling; exits non-zero on any mismatch.
  info PATH          Print header details for any N64 ROM.

Nothing derived from the ROM other than hashes and header metadata is ever
written outside rom/ and build/ (both gitignored).
"""
import argparse
import hashlib
import json
import shutil
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ROM_DIR = ROOT / "rom"
BUILD_DIR = ROOT / "build"
BASEROM = BUILD_DIR / "baserom.z64"
KNOWN = ROOT / "config" / "known_roms.json"
PIN = ROOT / "config" / "target_rom.json"
ROM_EXTS = {".z64", ".n64", ".v64", ".rom", ".bin"}

# First word of the header in each on-disk byte order.
MAGIC = {
    bytes.fromhex("80371240"): "z64",  # big-endian (native)
    bytes.fromhex("37804012"): "v64",  # 16-bit byteswapped
    bytes.fromhex("40123780"): "n64",  # 32-bit little-endian
}

# CRC32 of the IPL3 boot code (0x40..0x1000) -> CIC chip, per n64crc.
CIC_BY_BOOTCODE_CRC = {
    0x6170A4A1: "6101",
    0x90BB6CB5: "6102/7101",
    0x0B050EE0: "6103/7103",
    0x98BC2C86: "6105/7105",
    0xACC8580A: "6106/7106",
    0x009E9EA3: "7102",
}
# The CIC 6103 and 6106 boot code loads the game below the header's entry point.
ENTRY_ADJUST = {"6103/7103": 0x100000, "6106/7106": 0x200000}


def to_z64(data: bytes) -> tuple[bytes, str]:
    fmt = MAGIC.get(data[:4])
    if fmt is None:
        raise ValueError(f"not an N64 ROM (first word {data[:4].hex()})")
    if fmt == "z64":
        return data, fmt
    if len(data) % 4:
        raise ValueError("ROM size is not a multiple of 4")
    b = bytearray(data)
    if fmt == "v64":
        b[0::2], b[1::2] = data[1::2], data[0::2]
    else:
        b[0::4], b[1::4], b[2::4], b[3::4] = data[3::4], data[2::4], data[1::4], data[0::4]
    return bytes(b), fmt


def header(z64: bytes) -> dict:
    be32 = lambda off: int.from_bytes(z64[off:off + 4], "big")
    cic = CIC_BY_BOOTCODE_CRC.get(zlib.crc32(z64[0x40:0x1000]), "unknown")
    entry = be32(0x08)
    country = chr(z64[0x3E]) if 32 <= z64[0x3E] < 127 else "?"
    return {
        "name": z64[0x20:0x34].decode("ascii", "replace").rstrip(" \0"),
        "game_code": z64[0x3B:0x3F].decode("ascii", "replace"),
        "country": {"E": "USA", "J": "Japan", "P": "Europe"}.get(country, country),
        "version": z64[0x3F],
        "crc": [f"{be32(0x10):08X}", f"{be32(0x14):08X}"],
        "entry_point": f"0x{entry:08X}",
        "load_address": f"0x{entry - ENTRY_ADJUST.get(cic, 0):08X}",
        "libultra": f"0x{be32(0x0C):08X}",
        "cic": cic,
        "size_mib": len(z64) / (1 << 20),
    }


def microcode_strings(z64: bytes) -> list[str]:
    """Version strings embedded in RSP microcode data, e.g. 'RSP Gfx ucode F3DEX 1.23 ...'.

    Compressed games may hide these; an empty result means 'check in M1', not 'custom'.
    """
    found = []
    for marker in (b"RSP Gfx ucode", b"RSP SW Version"):
        start = 0
        while (i := z64.find(marker, start)) != -1:
            end = i
            while end < len(z64) and 32 <= z64[end] < 127:
                end += 1
            text = z64[i:end].decode("ascii").strip()
            if text not in found:
                found.append(text)
            start = end
    return found


def hashes(data: bytes) -> dict:
    return {"md5": hashlib.md5(data).hexdigest(), "sha1": hashlib.sha1(data).hexdigest()}


def known() -> dict:
    return json.loads(KNOWN.read_text())


def identify(path: Path) -> dict:
    raw = path.read_bytes()
    z64, fmt = to_z64(raw)
    h = hashes(z64)
    db = known()
    match = next((r for r in db["roms"] if r["md5"] == h["md5"]), None)
    rejected = next((r for r in db["rejected"] if r["md5"] == h["md5"]), None)
    return {"path": path, "format": fmt, "z64": z64, "header": header(z64),
            "match": match, "rejected": rejected, **h}


def describe(info: dict) -> str:
    hd = info["header"]
    lines = [f"{info['path'].relative_to(ROOT) if info['path'].is_relative_to(ROOT) else info['path']}"]
    if info["match"]:
        m = info["match"]
        lines.append(f"  game      {m['title']} [{m['id']}] {m['region']} rev {m['revision']}  (verified good dump)")
    elif info["rejected"]:
        lines.append(f"  game      REJECTED: {info['rejected']['name']} ({info['rejected']['reason']})")
    else:
        lines.append("  game      UNKNOWN: hash matches no known NFL Blitz dump")
    ucode = [f"  ucode     {u}" for u in microcode_strings(info["z64"])]
    ucode = ucode or ["  ucode     no version string found (compressed or custom; check in M1)"]
    lines += [
        f"  format    {info['format']}{' (will convert to z64)' if info['format'] != 'z64' else ''}",
        f"  header    '{hd['name']}' code {hd['game_code']} {hd['country']} v{hd['version']}  {hd['size_mib']:.0f} MiB",
        f"  boot      CIC {hd['cic']}  entry {hd['entry_point']}  load {hd['load_address']}  libultra field {hd['libultra']}",
        f"  crc       {' '.join(hd['crc'])}",
        *ucode,
        f"  md5       {info['md5']}",
        f"  sha1      {info['sha1']}",
    ]
    return "\n".join(lines)


def rom_files() -> list[Path]:
    return sorted(p for p in ROM_DIR.iterdir() if p.is_file() and p.suffix.lower() in ROM_EXTS)


def scan_rom_dir() -> list[dict]:
    found = []
    for p in rom_files():
        try:
            found.append(identify(p))
        except ValueError as e:
            print(f"{p.name}: skipped ({e})")
    return found


def cmd_scan(args) -> int:
    found = scan_rom_dir()
    if not found:
        print("No ROMs in rom/. Place your cartridge dump there (.z64/.n64/.v64).")
        return 1
    for info in found:
        print(describe(info))
    games = {i["match"]["id"] for i in found if i["match"]}
    if len(games) > 1 and not PIN.exists():
        print(f"\nMultiple Blitz games present ({', '.join(sorted(games))}). "
              "Choose one with: tools/romtool.py import --target <id>")
        return 2
    return 0


def cmd_import(args) -> int:
    found = [i for i in scan_rom_dir() if i["match"]]
    if args.target:
        found = [i for i in found if i["match"]["id"] == args.target]
    ids = {i["match"]["id"] for i in found}
    if not found:
        print("No verified Blitz dump" + (f" with id '{args.target}'" if args.target else "") + " in rom/.")
        print("Run tools/romtool.py scan to see what was found.")
        return 1
    if len(ids) > 1:
        print(f"Multiple Blitz games present ({', '.join(sorted(ids))}); pass --target <id>.")
        return 2
    info = found[0]
    if PIN.exists() and not args.force:
        pin = json.loads(PIN.read_text())
        if pin["sha1"] != info["sha1"]:
            print(f"config/target_rom.json pins {pin['id']} ({pin['sha1']}).")
            print("Re-run with --force to switch targets.")
            return 2
    BUILD_DIR.mkdir(exist_ok=True)
    BASEROM.write_bytes(info["z64"])
    m, hd = info["match"], info["header"]
    PIN.write_text(json.dumps({
        "id": m["id"], "title": m["title"], "region": m["region"], "revision": m["revision"],
        "md5": info["md5"], "sha1": info["sha1"], "crc": hd["crc"], "cic": hd["cic"],
        "entry_point": hd["entry_point"], "load_address": hd["load_address"],
        "size_bytes": len(info["z64"]),
    }, indent=2) + "\n")
    print(describe(info))
    print(f"\nWrote {BASEROM.relative_to(ROOT)} and pinned {PIN.relative_to(ROOT)}.")
    return 0


def cmd_verify(args) -> int:
    if not PIN.exists():
        print("No target pinned. Run tools/romtool.py import first.", file=sys.stderr)
        return 1
    pin = json.loads(PIN.read_text())
    if not BASEROM.exists():
        print(f"{BASEROM.relative_to(ROOT)} missing. Run tools/romtool.py import.", file=sys.stderr)
        return 1
    sha1 = hashlib.sha1(BASEROM.read_bytes()).hexdigest()
    if sha1 != pin["sha1"]:
        print(f"baserom SHA-1 {sha1} does not match pinned {pin['sha1']} ({pin['id']}).", file=sys.stderr)
        return 1
    print(f"baserom OK: {pin['title']} [{pin['id']}] sha1 {sha1}")
    return 0


def cmd_normalize(args) -> int:
    """Copy a ROM in any byte order to a big-endian .z64 (dev runs: tools/run_dev.sh, tools/live.sh)."""
    data, order = to_z64(Path(args.src).read_bytes())
    Path(args.dst).write_bytes(data)
    return 0


def cmd_info(args) -> int:
    print(describe(identify(Path(args.path).resolve())))
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("scan").set_defaults(fn=cmd_scan)
    p = sub.add_parser("import")
    p.add_argument("--target")
    p.add_argument("--force", action="store_true")
    p.set_defaults(fn=cmd_import)
    sub.add_parser("verify").set_defaults(fn=cmd_verify)
    p = sub.add_parser("normalize")
    p.add_argument("src")
    p.add_argument("dst")
    p.set_defaults(fn=cmd_normalize)
    p = sub.add_parser("info")
    p.add_argument("path")
    p.set_defaults(fn=cmd_info)
    args = ap.parse_args()
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
