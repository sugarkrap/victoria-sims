"""Extract text/config files from the RAR archive in TSDATA.EXE."""

import struct, sys

ISO     = "C:/Users/meyer.n/Downloads/Sims2.iso"
RAR_SIG = b'\x52\x61\x72\x21\x1a\x07\x00'
TARGETS = [
    "TSData/Res/Lights/LightRigs/CASThumbnails.txt",
    "TSData/Res/Lights/LightRigs/SimThumbnails.txt",
    "TSData/Res/Lights/cas/casStudio.txt",
    "TSData/Res/Lights/CAS_lighting.txt",
    "TSData/Res/Lights/CAS_lights.txt",
    "TSData/Res/Lights/cas/casNeighborhoodPose.txt",
]

def read32le(d, o): return struct.unpack_from('<I', d, o)[0]
def read16le(d, o): return struct.unpack_from('<H', d, o)[0]

def iso_read(f, lba, offset, length):
    f.seek(lba * 2048 + offset); return f.read(length)

def get_tsdata_exe(f):
    f.seek(16 * 2048); pvd = f.read(2048)
    root_lba = read32le(pvd, 158); root_size = read32le(pvd, 166)
    f.seek(root_lba * 2048); data = f.read(root_size)
    pos = 0
    while pos < len(data):
        rec = data[pos]
        if rec == 0: break
        name_len = data[pos + 32]
        name = data[pos + 33:pos + 33 + name_len].decode('ascii', errors='replace').split(';')[0]
        if name == 'TSDATA.EXE': return read32le(data, pos + 2)
        pos += rec
    return None

def find_rar_offset(f, lba):
    for base in range(0, 64 * 1024 * 1024, 65529):
        data = iso_read(f, lba, base, 65536)
        idx = data.find(RAR_SIG)
        if idx != -1: return base + idx
    return None

def walk_and_extract(f, lba, rar_offset, targets):
    remaining = set(targets)
    pos = rar_offset
    while remaining:
        header = iso_read(f, lba, pos, 40)
        if len(header) < 7: break
        btype = header[2]; bflags = read16le(header, 3); bsize = read16le(header, 5)
        if bsize == 0 or btype == 0x7B: break
        if btype == 0x74:
            packed = read32le(header, 7)
            nsize  = read16le(header, 26)
            hoff = 32
            if bflags & 0x0100:
                ex = iso_read(f, lba, pos + 32, 8)
                packed |= read32le(ex, 0) << 32
                hoff = 40
            name = iso_read(f, lba, pos + hoff, nsize).decode('ascii', errors='replace').replace('\\', '/')
            doff = pos + bsize
            if name in remaining:
                content = iso_read(f, lba, doff, packed)
                yield name, content
                remaining.discard(name)
            pos = doff + packed
        else:
            pos += bsize + (read32le(header, 7) if bflags & 0x8000 else 0)

with open(ISO, 'rb') as f:
    lba = get_tsdata_exe(f)
    rar = find_rar_offset(f, lba)
    for name, content in walk_and_extract(f, lba, rar, TARGETS):
        print(f"\n{'='*70}")
        print(f"  {name}")
        print('='*70)
        try:
            print(content.decode('latin-1'))
        except Exception as e:
            print(f"  <decode error: {e}>")
            print(f"  {content[:200].hex()}")
