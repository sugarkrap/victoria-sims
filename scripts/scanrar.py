"""Scan the TSDATA.EXE embedded RAR archive from the ISO, list the package
filenames it contains, then read a few catalogue-related packages and report
all DBPF resource type IDs found, so we can identify the thumbnail type."""

import struct, sys

ISO     = "C:/Users/meyer.n/Downloads/Sims2.iso"
RAR_SIG = b'\x52\x61\x72\x21\x1a\x07\x00'  # RAR 2.x / classic marker

# ---- ISO helpers ----

def read_iso_uint32_le(data, off):
    return struct.unpack_from('<I', data, off)[0]

def get_tsdata_exe(f):
    """Return (lba, size) of TSDATA.EXE from the primary volume descriptor."""
    f.seek(16 * 2048)
    pvd = f.read(2048)
    root_lba  = read_iso_uint32_le(pvd, 158)
    root_size = read_iso_uint32_le(pvd, 166)

    f.seek(root_lba * 2048)
    data = f.read(root_size)
    pos = 0
    while pos < len(data):
        rec = data[pos]
        if rec == 0:
            break
        file_lba = read_iso_uint32_le(data, pos + 2)
        file_size_lo = read_iso_uint32_le(data, pos + 10)
        name_len = data[pos + 32]
        name = data[pos + 33 : pos + 33 + name_len].decode('ascii', errors='replace').split(';')[0]
        if name == 'TSDATA.EXE':
            return file_lba, file_size_lo
        pos += rec
    return None, None

def iso_read(f, lba, offset, length):
    """Read `length` bytes at `offset` within the file at `lba`."""
    f.seek(lba * 2048 + offset)
    return f.read(length)

# ---- Find RAR start offset inside the EXE ----

def find_rar_offset(f, lba, file_size):
    """Scan the EXE in 64 KB chunks looking for the RAR signature."""
    chunk = 65536
    # Only scan the first ~64 MB (the program part; archive appended after)
    scan_limit = min(file_size, 64 * 1024 * 1024)
    for base in range(0, scan_limit, chunk - 7):
        data = iso_read(f, lba, base, min(chunk, scan_limit - base))
        idx = data.find(RAR_SIG)
        if idx != -1:
            return base + idx
    return None

# ---- RAR archive walker ----
# We only need the classic (RAR 2.x) format, which is what the engine reads.

BLOCK_TYPE_ARCHIVE_HEADER = 0x73
BLOCK_TYPE_FILE           = 0x74
BLOCK_TYPE_END            = 0x7B

def rar_list_files(f, lba, file_size, rar_offset, max_files=4000):
    """Yield (name, packed_size, unpack_size, data_offset_in_file) for each entry."""
    pos = rar_offset

    while pos < file_size:
        header = iso_read(f, lba, pos, 32)
        if len(header) < 7:
            break
        block_type  = header[2]
        block_flags = struct.unpack_from('<H', header, 3)[0]
        block_size  = struct.unpack_from('<H', header, 5)[0]
        if block_size == 0:
            break

        if block_type == BLOCK_TYPE_END:
            break

        if block_type == BLOCK_TYPE_FILE:
            if len(header) < 32:
                break
            packed_size   = struct.unpack_from('<I', header, 7)[0]
            unpack_size   = struct.unpack_from('<I', header, 11)[0]
            name_size     = struct.unpack_from('<H', header, 26)[0]

            large = (block_flags & 0x0100) != 0
            high_off = 32
            if large:
                extra = iso_read(f, lba, pos + 32, 8)
                packed_size  |= struct.unpack_from('<I', extra, 0)[0] << 32
                unpack_size  |= struct.unpack_from('<I', extra, 4)[0] << 32
                high_off = 40

            name_bytes = iso_read(f, lba, pos + high_off, name_size)
            name = name_bytes.decode('ascii', errors='replace').replace('\\', '/')

            data_offset = pos + block_size
            yield name, packed_size, unpack_size, data_offset
            pos = data_offset + packed_size
        else:
            # Skip other block types
            if block_flags & 0x8000:
                # Has data — its length is in bytes 7-10
                if len(header) >= 11:
                    data_len = struct.unpack_from('<I', header, 7)[0]
                else:
                    data_len = 0
                pos += block_size + data_len
            else:
                pos += block_size

# ---- DBPF type scanner ----

DBPF_MAGIC = b'DBPF'

def dbpf_types_from_bytes(data):
    """Given raw (uncompressed) DBPF bytes, return dict of type_id -> count."""
    if len(data) < 96 or data[:4] != DBPF_MAGIC:
        return {}
    major = struct.unpack_from('<I', data, 4)[0]
    entry_count = struct.unpack_from('<I', data, 36)[0]
    index_off   = struct.unpack_from('<I', data, 40)[0]
    index_size  = struct.unpack_from('<I', data, 44)[0]
    if entry_count == 0 or index_size == 0:
        return {}
    entry_size = 24 if major >= 2 else 20
    idx = data[index_off : index_off + index_size]
    types = {}
    pos = 0
    for _ in range(entry_count):
        if pos + 4 > len(idx):
            break
        tid = struct.unpack_from('<I', idx, pos)[0]
        types[tid] = types.get(tid, 0) + 1
        pos += entry_size
    return types

# ---- main ----

print(f"Locating TSDATA.EXE in {ISO} ...")
with open(ISO, 'rb') as f:
    lba, exe_size = get_tsdata_exe(f)
    if lba is None:
        print("ERROR: TSDATA.EXE not found")
        sys.exit(1)
    print(f"  LBA={lba}, reported size={exe_size} bytes")

    print("Scanning for embedded RAR signature ...")
    rar_off = find_rar_offset(f, lba, exe_size if exe_size < 200*1024*1024 else 64*1024*1024)
    if rar_off is None:
        print("ERROR: RAR signature not found in first 64 MB")
        sys.exit(1)
    print(f"  RAR archive starts at offset 0x{rar_off:X} within TSDATA.EXE\n")

    print("Listing archive entries (first 200) ...")
    all_types = {}
    scanned = 0
    entries = []
    for name, packed, unpacked, data_off in rar_list_files(f, lba, exe_size if exe_size < 2**32 else 2**31, rar_off):
        entries.append((name, packed, unpacked, data_off))
        if len(entries) <= 200:
            print(f"  {name:60s}  {packed//1024:6d}KB packed  {unpacked//1024:6d}KB unpacked")

print(f"\nTotal entries found: {len(entries)}")
