"""Debug: dump the top-level ISO directory to understand its layout."""
import struct

ISO = "C:/Users/meyer.n/Downloads/Sims2.iso"

with open(ISO, 'rb') as f:
    # Check all volume descriptors
    for sector in range(16, 32):
        f.seek(sector * 2048)
        vd = f.read(2048)
        vd_type = vd[0]
        magic = vd[1:6]
        if magic != b'CD001':
            break
        types = {0: 'boot', 1: 'primary', 2: 'supplementary', 255: 'terminator'}
        print(f"Sector {sector}: type={vd_type} ({types.get(vd_type,'?')})")
        if vd_type == 1:  # Primary Volume Descriptor
            root_lba  = struct.unpack_from('<I', vd, 158)[0]
            root_size = struct.unpack_from('<I', vd, 166)[0]
            print(f"  Root dir: lba={root_lba}, size={root_size}")
            # Dump top-level entries
            f.seek(root_lba * 2048)
            data = f.read(root_size)
            pos = 0
            count = 0
            while pos < len(data) and count < 50:
                rec_len = data[pos]
                if rec_len == 0:
                    pos = (pos // 2048 + 1) * 2048
                    if pos >= len(data): break
                    continue
                flags = data[pos + 25]
                file_lba  = struct.unpack_from('<I', data, pos + 2)[0]
                file_size = struct.unpack_from('<I', data, pos + 10)[0]
                name_len  = data[pos + 32]
                name = data[pos + 33:pos + 33 + name_len]
                is_dir = bool(flags & 2)
                print(f"  {'DIR' if is_dir else 'FILE':4s} {name!r:40s}  lba={file_lba}  size={file_size}")
                pos += rec_len
                count += 1
