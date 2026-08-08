"""Scan Skins.package and any CAS-related packages to find thumbnail types."""

import struct, sys

ISO     = "C:/Users/meyer.n/Downloads/Sims2.iso"
RAR_SIG = b'\x52\x61\x72\x21\x1a\x07\x00'
TARGETS = [
    "TSData/Res/Catalog/Skins/Skins.package",
    "TSData/Res/GlobalLots/CAS!.package",
]
DBPF_MAGIC = b'DBPF'

def read32le(data, off):
    return struct.unpack_from('<I', data, off)[0]
def read16le(data, off):
    return struct.unpack_from('<H', data, off)[0]

def iso_read(f, lba, offset, length):
    f.seek(lba * 2048 + offset)
    return f.read(length)

def get_tsdata_exe(f):
    f.seek(16 * 2048)
    pvd = f.read(2048)
    root_lba  = read32le(pvd, 158)
    root_size = read32le(pvd, 166)
    f.seek(root_lba * 2048)
    data = f.read(root_size)
    pos = 0
    while pos < len(data):
        rec = data[pos]
        if rec == 0: break
        file_lba = read32le(data, pos + 2)
        name_len = data[pos + 32]
        name = data[pos + 33:pos + 33 + name_len].decode('ascii', errors='replace').split(';')[0]
        if name == 'TSDATA.EXE':
            return file_lba
        pos += rec
    return None

def find_rar_offset(f, lba):
    for base in range(0, 64 * 1024 * 1024, 65529):
        data = iso_read(f, lba, base, 65536)
        idx = data.find(RAR_SIG)
        if idx != -1:
            return base + idx
    return None

BLOCK_TYPE_FILE = 0x74
BLOCK_TYPE_END  = 0x7B

def walk_rar(f, lba, rar_offset, target_names):
    """Walk the RAR archive and return {name: (data_offset, packed_size)} for targets."""
    results = {}
    pos = rar_offset
    while True:
        header = iso_read(f, lba, pos, 40)
        if len(header) < 7: break
        block_type  = header[2]
        block_flags = read16le(header, 3)
        block_size  = read16le(header, 5)
        if block_size == 0: break
        if block_type == BLOCK_TYPE_END: break
        if block_type == BLOCK_TYPE_FILE:
            packed_size  = read32le(header, 7)
            name_size    = read16le(header, 26)
            large = (block_flags & 0x0100) != 0
            high_off = 32
            if large:
                extra = iso_read(f, lba, pos + 32, 8)
                packed_size  |= read32le(extra, 0) << 32
                high_off = 40
            name_bytes = iso_read(f, lba, pos + high_off, name_size)
            name = name_bytes.decode('ascii', errors='replace').replace('\\', '/')
            data_offset = pos + block_size
            if name in target_names:
                results[name] = (data_offset, packed_size)
                if len(results) == len(target_names):
                    break
            pos = data_offset + packed_size
        else:
            has_data = block_flags & 0x8000
            if has_data:
                data_len = read32le(header, 7) if len(header) >= 11 else 0
                pos += block_size + data_len
            else:
                pos += block_size
    return results

def scan_dbpf(f, lba, pkg_offset, pkg_size, label):
    print(f"\n=== {label} ===")
    header = iso_read(f, lba, pkg_offset, 96)
    if header[:4] != DBPF_MAGIC:
        print("  NOT a DBPF package")
        return
    entry_count = read32le(header, 36)
    index_off   = read32le(header, 40)
    index_size  = read32le(header, 44)
    entry_size  = (index_size // entry_count) if entry_count > 0 else 24
    print(f"  {entry_count} entries, entry_size={entry_size}")

    idx = iso_read(f, lba, pkg_offset + index_off, index_size)
    types = {}
    pos = 0
    for i in range(entry_count):
        if pos + entry_size > len(idx): break
        tid  = read32le(idx, pos)
        off  = read32le(idx, pos + 16) if entry_size >= 24 else read32le(idx, pos + 12)
        sz   = read32le(idx, pos + 20) if entry_size >= 24 else read32le(idx, pos + 16)
        if tid not in types:
            types[tid] = []
        types[tid].append((off, sz))
        pos += entry_size

    for tid, entries in sorted(types.items(), key=lambda x: -len(x[1])):
        # Show first entry's bytes
        off, sz = entries[0]
        data = iso_read(f, lba, pkg_offset + off, min(sz, 12)) if sz > 0 else b''
        magic = data[:8].hex() if data else '????'
        print(f"  0x{tid:08X}  {len(entries):5d} entries  e.g. sz={sz}  magic={magic}")

with open(ISO, 'rb') as f:
    tsdata_lba = get_tsdata_exe(f)
    rar_off = find_rar_offset(f, tsdata_lba)
    print(f"TSDATA.EXE LBA={tsdata_lba}, RAR at 0x{rar_off:X}")

    found = walk_rar(f, tsdata_lba, rar_off, TARGETS)
    for target in TARGETS:
        if target in found:
            pkg_off, pkg_size = found[target]
            scan_dbpf(f, tsdata_lba, pkg_off, pkg_size, target)
        else:
            print(f"\n=== {target} NOT FOUND ===")
