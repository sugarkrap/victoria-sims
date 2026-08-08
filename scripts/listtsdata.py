"""Recursively list all files in specific ISO directories."""
import struct

ISO = "C:/Users/meyer.n/Downloads/Sims2.iso"

def list_dir(f, lba, size, prefix="", depth=0, max_depth=8, results=None):
    if results is None:
        results = []
    if depth > max_depth:
        return results
    f.seek(lba * 2048)
    data = f.read(size)
    pos = 0
    while pos < len(data):
        rec_len = data[pos]
        if rec_len == 0:
            pos = (pos // 2048 + 1) * 2048
            if pos >= len(data):
                break
            continue
        flags = data[pos + 25]
        file_lba  = struct.unpack_from('<I', data, pos + 2)[0]
        file_size = struct.unpack_from('<I', data, pos + 10)[0]
        name_len  = data[pos + 32]
        raw  = data[pos + 33 : pos + 33 + name_len]
        name = raw.decode('ascii', errors='replace').split(';')[0]
        is_dir = bool(flags & 2)
        if name not in ('.', '..'):
            path = f"{prefix}/{name}"
            if is_dir:
                list_dir(f, file_lba, file_size, path, depth+1, max_depth, results)
            else:
                results.append((path, file_lba, file_size))
        pos += rec_len
    return results

with open(ISO, 'rb') as f:
    # Read Primary VD
    f.seek(16 * 2048)
    pvd = f.read(2048)
    root_lba  = struct.unpack_from('<I', pvd, 158)[0]
    root_size = struct.unpack_from('<I', pvd, 166)[0]

    # Find TSDATA directory
    f.seek(root_lba * 2048)
    data = f.read(root_size)
    tsdata_lba = tsdata_size = 0
    pos = 0
    while pos < len(data):
        rec_len = data[pos]
        if rec_len == 0:
            break
        flags = data[pos + 25]
        file_lba  = struct.unpack_from('<I', data, pos + 2)[0]
        file_size = struct.unpack_from('<I', data, pos + 10)[0]
        name_len  = data[pos + 32]
        name = data[pos + 33:pos + 33 + name_len].decode('ascii', errors='replace').split(';')[0]
        if name == 'TSDATA' and (flags & 2):
            tsdata_lba, tsdata_size = file_lba, file_size
        pos += rec_len

    print(f"TSDATA dir: lba={tsdata_lba}, size={tsdata_size}")
    files = list_dir(f, tsdata_lba, tsdata_size, "TSDATA")
    print(f"\nAll files under TSDATA ({len(files)} total):")
    for path, lba, size in sorted(files):
        print(f"  {path:70s}  {size//1024:7d} KB  lba={lba}")
