"""Read the globalcatbin.bundle.package from the ISO and report what
resource types it contains, plus the first few entries of each type."""

import struct, sys

ISO     = "C:/Users/meyer.n/Downloads/Sims2.iso"
RAR_SIG = b'\x52\x61\x72\x21\x1a\x07\x00'
TARGET  = "TSData/Res/Catalog/Bins/globalcatbin.bundle.package"

DBPF_MAGIC = b'DBPF'

def read32le(data, off):
    return struct.unpack_from('<I', data, off)[0]
def read16le(data, off):
    return struct.unpack_from('<H', data, off)[0]

# ---- ISO read helper ----
def iso_read(f, lba, offset, length):
    f.seek(lba * 2048 + offset)
    return f.read(length)

# ---- Find TSDATA.EXE ----
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

# ---- Find RAR offset ----
def find_rar_offset(f, lba):
    for base in range(0, 64 * 1024 * 1024, 65529):
        data = iso_read(f, lba, base, 65536)
        idx = data.find(RAR_SIG)
        if idx != -1:
            return base + idx
    return None

# ---- RAR walker: find one file ----
BLOCK_TYPE_FILE = 0x74
BLOCK_TYPE_END  = 0x7B

def find_rar_entry(f, lba, rar_offset, target_name):
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
            unpack_size  = read32le(header, 11)
            name_size    = read16le(header, 26)
            large = (block_flags & 0x0100) != 0
            high_off = 32
            if large:
                extra = iso_read(f, lba, pos + 32, 8)
                packed_size  |= read32le(extra, 0) << 32
                unpack_size  |= read32le(extra, 4) << 32
                high_off = 40
            name_bytes = iso_read(f, lba, pos + high_off, name_size)
            name = name_bytes.decode('ascii', errors='replace').replace('\\', '/')
            data_offset = pos + block_size
            if name == target_name:
                return data_offset, packed_size, unpack_size
            pos = data_offset + packed_size
        else:
            has_data = block_flags & 0x8000
            if has_data:
                data_len = read32le(header, 7) if len(header) >= 11 else 0
                pos += block_size + data_len
            else:
                pos += block_size
    return None, None, None

# ---- DBPF scanner ----

def dbpf_scan(f, lba, pkg_offset, pkg_size, max_entries_per_type=3):
    header = iso_read(f, lba, pkg_offset, 96)
    if header[:4] != DBPF_MAGIC:
        print("Not a DBPF package!")
        return
    major = read32le(header, 4)
    entry_count = read32le(header, 36)
    index_off   = read32le(header, 40)
    index_size  = read32le(header, 44)
    # Determine entry size from index_size / entry_count (avoids guessing version)
    entry_size = (index_size // entry_count) if entry_count > 0 else 24
    print(f"  DBPF version {major}.x, {entry_count} entries, index at 0x{index_off:X} ({index_size} bytes), entry size={entry_size}")
    idx = iso_read(f, lba, pkg_offset + index_off, index_size)

    types = {}    # type_id -> list of (group, instance_lo, instance_hi, off, size)
    pos = 0
    for i in range(entry_count):
        if pos + entry_size > len(idx):
            break
        tid  = read32le(idx, pos)
        grp  = read32le(idx, pos + 4)
        inst = read32le(idx, pos + 8)
        if entry_size >= 24:
            inst_hi = read32le(idx, pos + 12)
            off     = read32le(idx, pos + 16)
            sz      = read32le(idx, pos + 20)
        else:
            inst_hi = 0
            off     = read32le(idx, pos + 12)
            sz      = read32le(idx, pos + 16)
        if tid not in types:
            types[tid] = []
        types[tid].append((grp, inst, inst_hi, off, sz))
        pos += entry_size

    print(f"\n  Resource types:")
    for tid, entries in sorted(types.items(), key=lambda x: -len(x[1])):
        print(f"    0x{tid:08X}  {len(entries):5d} entries")
        for grp, inst, inst_hi, off, sz in entries[:max_entries_per_type]:
            # Peek at first 8 bytes of the resource to identify format
            data = iso_read(f, lba, pkg_offset + off, min(sz, 16))
            magic = data[:4].hex() if data else '????'
            print(f"      grp=0x{grp:08X} inst=0x{inst_hi:08X}{inst:08X} "
                  f"off=0x{off:X} sz={sz}  magic={magic}")

with open(ISO, 'rb') as f:
    print("Finding TSDATA.EXE ...")
    tsdata_lba = get_tsdata_exe(f)
    if tsdata_lba is None:
        print("ERROR: TSDATA.EXE not found")
        sys.exit(1)
    print(f"  LBA={tsdata_lba}")

    print("Finding RAR offset ...")
    rar_off = find_rar_offset(f, tsdata_lba)
    if rar_off is None:
        print("ERROR: RAR not found")
        sys.exit(1)
    print(f"  RAR at offset 0x{rar_off:X}")

    print(f"\nLocating {TARGET} ...")
    pkg_off, packed, unpacked = find_rar_entry(f, tsdata_lba, rar_off, TARGET)
    if pkg_off is None:
        print("ERROR: entry not found")
        sys.exit(1)
    print(f"  offset=0x{pkg_off:X}, packed={packed//1024}KB, unpacked={unpacked//1024}KB")

    print("\nScanning DBPF ...")
    dbpf_scan(f, tsdata_lba, pkg_off, packed)
