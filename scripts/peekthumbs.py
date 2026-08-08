"""Read a resource of type 0xCCCEF852 from globalcatbin and dump its header
to identify the thumbnail format, then extract one to disk to look at it."""

import struct, sys, os

ISO     = "C:/Users/meyer.n/Downloads/Sims2.iso"
RAR_SIG = b'\x52\x61\x72\x21\x1a\x07\x00'
TARGET  = "TSData/Res/Catalog/Bins/globalcatbin.bundle.package"
DBPF_MAGIC = b'DBPF'
TARGET_TYPE = 0xCCCEF852

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
            if name == target_name:
                return data_offset
            pos = data_offset + packed_size
        else:
            has_data = block_flags & 0x8000
            if has_data:
                data_len = read32le(header, 7) if len(header) >= 11 else 0
                pos += block_size + data_len
            else:
                pos += block_size
    return None

def scan_and_extract(f, lba, pkg_offset):
    header = iso_read(f, lba, pkg_offset, 96)
    entry_count = read32le(header, 36)
    index_off   = read32le(header, 40)
    index_size  = read32le(header, 44)
    entry_size  = index_size // entry_count if entry_count else 24
    print(f"DBPF: {entry_count} entries, entry_size={entry_size}")

    idx = iso_read(f, lba, pkg_offset + index_off, index_size)

    hits = []
    pos = 0
    for i in range(entry_count):
        if pos + entry_size > len(idx): break
        tid  = read32le(idx, pos)
        grp  = read32le(idx, pos + 4)
        inst = read32le(idx, pos + 8)
        inh  = read32le(idx, pos + 12)
        off  = read32le(idx, pos + 16)
        sz   = read32le(idx, pos + 20)
        if tid == TARGET_TYPE:
            hits.append((grp, inst, inh, off, sz))
        pos += entry_size

    print(f"\nFound {len(hits)} entries of type 0x{TARGET_TYPE:08X}")
    for i, (grp, inst, inh, off, sz) in enumerate(hits[:5]):
        print(f"\n  Entry {i}: grp=0x{grp:08X} inst=0x{inh:08X}{inst:08X} off=0x{off:X} sz={sz}")
        data = iso_read(f, lba, pkg_offset + off, min(sz, 256))
        print(f"  First 64 bytes: {data[:64].hex()}")
        # Check if it looks like JPEG (FFD8FF) or PNG (89504E47) or TGA or something else
        if data[:3] == b'\xff\xd8\xff':
            print(f"  --> JPEG!")
            outname = f"thumb_{i}.jpg"
        elif data[:4] == b'\x89PNG':
            print(f"  --> PNG!")
            outname = f"thumb_{i}.png"
        else:
            print(f"  --> Unknown format, magic={data[:8].hex()}")
            outname = f"thumb_{i}.bin"
        # Extract to disk
        full = iso_read(f, lba, pkg_offset + off, sz)
        with open(outname, 'wb') as out:
            out.write(full)
        print(f"  Saved {sz} bytes to {outname}")

with open(ISO, 'rb') as f:
    tsdata_lba = get_tsdata_exe(f)
    print(f"TSDATA.EXE LBA={tsdata_lba}")
    rar_off = find_rar_offset(f, tsdata_lba)
    print(f"RAR at 0x{rar_off:X}")
    pkg_off = find_rar_entry(f, tsdata_lba, rar_off, TARGET)
    print(f"Package at 0x{pkg_off:X}")
    scan_and_extract(f, tsdata_lba, pkg_off)
