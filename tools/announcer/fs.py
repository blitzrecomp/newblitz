"""Asset file system readers for vanilla ('fs') and team+ ('zs') Blitz 2000 ROMs. Analysis only."""
import struct, sys
FS = 0x150C40

def be32(b, o): return struct.unpack_from('>I', b, o)[0]

def toc(rom):
    magic = rom[FS:FS+4]
    tocoff = be32(rom, FS+8); n = be32(rom, FS+12)
    t = FS + tocoff
    out = []
    if magic[:2] == b'fs':
        for i in range(n):
            e = t + i*40
            name = rom[e:e+0x18].split(b'\0')[0].decode()
            usize, off, typ, ssize = struct.unpack_from('>IIII', rom, e+0x18)
            out.append(dict(i=i, name=name, usize=usize, off=off, type=typ, psize=ssize, rom=FS+off))
    elif magic[:2] == b'zs':
        for i in range(n):
            e = t + i*32
            name = rom[e:e+0x14].split(b'\0')[0].decode()
            usize, off, psize = struct.unpack_from('>III', rom, e+0x14)
            out.append(dict(i=i, name=name, usize=usize, off=off, type=5 if psize == 0 else 4, psize=psize, rom=FS+off))
    else:
        raise ValueError(magic)
    return magic, out

def load(path):
    return open(path, 'rb').read()

def get(rom, ent):
    if ent['type'] != 5:
        raise NotImplementedError('compressed')
    return rom[ent['rom']:ent['rom']+ent['usize']]

if __name__ == '__main__':
    rom = load(sys.argv[1])
    magic, ents = toc(rom)
    print(magic, len(ents))
    pat = sys.argv[2] if len(sys.argv) > 2 else None
    for e in ents:
        if pat is None or any(e['name'].lower().endswith(x) for x in pat.split(',')):
            print(f"{e['i']:5d} {e['name']:24s} usize={e['usize']:8d} type={e['type']} psize={e['psize']:8d} rom={e['rom']:#x}")

def read(rom, ent):
    if ent['type'] == 5:
        return rom[ent['rom']:ent['rom']+ent['usize']]
    import lzo
    return lzo.decompress(rom[ent['rom']:ent['rom']+ent['psize']], ent['usize'])

def by_name(ents):
    return {e['name'].lower(): e for e in ents}
