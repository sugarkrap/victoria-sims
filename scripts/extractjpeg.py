"""Extract JPEG entries (type 0x856DDBAC) from CAS!.package to see what they are."""

import struct, sys

ISO     = "C:/Users/meyer.n/Downloads/Sims2.iso"
RAR_SIG = b'\x52\x61\x72\x21\x1a\x07\x00'
TARGET  = "TSData/Res/GlobalLots/CAS!.package"
JPEG_TYPE = 0x856DDBAC

def read32le(data, off): return struct.unpack_from('<I', data, off)[0]
def read16le(data, off): return struct.unpack_from('<H', data, off)[0]

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
        if name == 'TSDATA.EXE': return file_lba
        pos += rec
    return None

def find_rar_offset(f, lba):
    for base in range(0, 64 * 1024 * 1024, 65529):
        data = iso_read(f, lba, base, 65536)
        idx = data.find(RAR_SIG)
        if idx != -1: return base + idx
    return None

def find_rar_entry(f, lba, rar_offset, target_name):
    pos = rar_offset
    while True:
        header = iso_read(f, lba, pos, 40)
        if len(header) < 7: break
        block_type  = header[2]
        block_flags = read16le(header, 3)
        block_size  = read16le(header, 5)
        if block_size == 0: break
        if block_type == 0x7B: break  # END
        if block_type == 0x74:  # FILE
            packed_size = read32le(header, 7)
            name_size   = read16le(header, 26)
            high_off = 32
            if block_flags & 0x0100:
                extra = iso_read(f, lba, pos + 32, 8)
                packed_size |= read32le(extra, 0) << 32
                high_off = 40
            name_bytes = iso_read(f, lba, pos + high_off, name_size)
            name = name_bytes.decode('ascii', errors='replace').replace('\\', '/')
            data_off = pos + block_size
            if name == target_name: return data_off
            pos = data_off + packed_size
        else:
            if block_flags & 0x8000:
                pos += block_size + read32le(header, 7)
            else:
                pos += block_size
    return None

with open(ISO, 'rb') as f:
    tsdata_lba = get_tsdata_exe(f)
    rar_off = find_rar_offset(f, tsdata_lba)
    pkg_off = find_rar_entry(f, tsdata_lba, rar_off, TARGET)

    header = iso_read(f, tsdata_lba, pkg_off, 96)
    entry_count = read32le(header, 36)
    index_off   = read32le(header, 40)
    index_size  = read32le(header, 44)
    entry_size  = (index_size // entry_count) if entry_count else 20
    idx = iso_read(f, tsdata_lba, pkg_off + index_off, index_size)

    i = 0
    pos = 0
    for _ in range(entry_count):
        if pos + entry_size > len(idx): break
        tid  = read32le(idx, pos)
        grp  = read32le(idx, pos + 4)
        inst = read32le(idx, pos + 8)
        off  = read32le(idx, pos + 12)
        sz   = read32le(idx, pos + 16)
        if tid == JPEG_TYPE:
            print(f"Entry {i}: grp=0x{grp:08X} inst=0x{inst:08X} sz={sz}")
            data = iso_read(f, tsdata_lba, pkg_off + off, sz)
            # The resource starts with a 4-byte size, then check for JPEG
            if data[:2] == b'\xff\xd8':
                outname = f"cas_jpeg_{i}.jpg"
                with open(outname, 'wb') as out: out.write(data)
                print(f"  -> Saved pure JPEG {sz}B to {outname}")
            elif len(data) >= 4 and data[4:6] == b'\xff\xd8':
                # Maybe 4-byte header prefix
                outname = f"cas_jpeg_{i}.jpg"
                with open(outname, 'wb') as out: out.write(data[4:])
                print(f"  -> Saved JPEG (offset 4) to {outname}")
            else:
                print(f"  -> magic={data[:8].hex()}")
            i += 1
        pos += entry_size
