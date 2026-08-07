"""Scan the Sims 2 ISO for .package files, then scan those packages for
thumbnail resource types so we know what type IDs to look for."""

import struct, sys

ISO = "C:/Users/meyer.n/Downloads/Sims2.iso"

# ---------- ISO 9660 directory walk ----------

def parse_dir(f, lba, size, depth=0, found=None):
    if found is None:
        found = []
    f.seek(lba * 2048)
    data = f.read(size)
    pos = 0
    while pos < len(data):
        rec_len = data[pos]
        if rec_len == 0:
            # Pad to next sector boundary
            pos = (pos // 2048 + 1) * 2048
            if pos >= len(data):
                break
            continue
        flags = data[pos + 25]
        file_lba  = struct.unpack_from('<I', data, pos + 2)[0]
        file_size = struct.unpack_from('<I', data, pos + 10)[0]
        name_len  = data[pos + 32]
        raw = data[pos + 33 : pos + 33 + name_len]
        name = raw.decode('ascii', errors='replace').split(';')[0]
        is_dir = (flags & 2) != 0
        if name not in ('.', '..'):
            if is_dir and depth < 8:
                parse_dir(f, file_lba, file_size, depth + 1, found)
            elif not is_dir and name.lower().endswith('.package'):
                found.append((name, file_lba, file_size))
        pos += rec_len
    return found

# ---------- DBPF reader ----------

DBPF_MAGIC = b'DBPF'

def read_dbpf_types(f, lba, pkg_size):
    """Return a set of (type_id, instance_id) for every entry in the DBPF."""
    offset = lba * 2048
    f.seek(offset)
    header = f.read(96)
    if len(header) < 96 or header[:4] != DBPF_MAGIC:
        return {}
    index_version_major = struct.unpack_from('<I', header, 4)[0]
    index_entry_count   = struct.unpack_from('<I', header, 36)[0]
    index_offset        = struct.unpack_from('<I', header, 40)[0]
    index_size          = struct.unpack_from('<I', header, 44)[0]

    if index_entry_count == 0 or index_size == 0:
        return {}

    # Figure out entry size from index_version
    # Major 1 = 20 bytes/entry, Major 2 = 24 bytes/entry
    entry_size = 24 if index_version_major >= 2 else 20

    f.seek(offset + index_offset)
    idx = f.read(index_size)

    types = {}  # type_id -> count
    pos = 0
    for _ in range(index_entry_count):
        if pos + 4 > len(idx):
            break
        type_id = struct.unpack_from('<I', idx, pos)[0]
        types[type_id] = types.get(type_id, 0) + 1
        pos += entry_size
    return types


# ---------- main ----------

print(f"Opening {ISO} ...")
with open(ISO, 'rb') as f:
    f.seek(16 * 2048)
    pvd = f.read(2048)
    root_lba  = struct.unpack_from('<I', pvd, 158)[0]
    root_size = struct.unpack_from('<I', pvd, 166)[0]

    print("Walking directory tree ...")
    packages = parse_dir(f, root_lba, root_size)
    print(f"  {len(packages)} .package file(s) found\n")

    # Aggregate type IDs across all packages
    all_types = {}
    for name, lba, size in packages:
        types = read_dbpf_types(f, lba, size)
        for tid, count in types.items():
            all_types[tid] = all_types.get(tid, 0) + count

print("Resource types found across all packages:")
print(f"  {'Type ID':>12}  {'Count':>8}")
for tid, count in sorted(all_types.items(), key=lambda x: -x[1]):
    print(f"  0x{tid:08X}  {count:8d}")
