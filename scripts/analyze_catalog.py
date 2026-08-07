"""
Analyze all 5491 catalog entries from BodyShopThumbnails:
- Distribution of src_grp values
- Cross-reference catalog src_insts vs binary/xml SKIN_ENTRY instances in Skins.package
Uses the working refpack_decomp from parse_catidx.py.
"""
import struct, sys, collections

ISO = 'C:/Users/meyer.n/Downloads/Sims2.iso'
RAR_SIG = b'\x52\x61\x72\x21\x1a\x07\x00'

def r16(b, o): return struct.unpack_from('<H', b, o)[0]
def r32(b, o): return struct.unpack_from('<I', b, o)[0]

def iso_read(f, lba, offset, length):
    f.seek(lba * 2048 + offset)
    return f.read(length)

def refpack_decomp(raw):
    decsz=(raw[6]<<16)|(raw[7]<<8)|raw[8]
    src=raw[9:]; dst=bytearray(decsz); rp=0; wp=0
    while rp<len(src):
        c=src[rp]; lc=0; cc=0; dist=0; last=False
        if c<0x80:
            lc=c&3; cc=((c&0x1C)>>2)+3; dist=((c&0x60)<<3)|src[rp+1]+1; rp+=2
        elif c<0xC0:
            lc=(src[rp+1]&0xC0)>>6; cc=(c&0x3F)+4; dist=((src[rp+1]&0x3F)<<8)|src[rp+2]+1; rp+=3
        elif c<0xE0:
            lc=c&3; cc=((c&0x0C)<<6)|src[rp+3]+5; dist=((c&0x10)<<12)|(src[rp+1]<<8)|src[rp+2]+1; rp+=4
        elif c<0xFC:
            lc=((c&0x1F)<<2)+4; rp+=1
        else:
            lc=c&3; last=True; rp+=1
        for i in range(lc): dst[wp]=src[rp+i]; wp+=1
        rp+=lc
        for i in range(cc):
            if wp>=len(dst): break
            dst[wp]=dst[wp-dist] if wp-dist>=0 else 0; wp+=1
        if last: break
    return bytes(dst[:wp])

with open(ISO, 'rb') as f:
    # Find TSDATA.EXE
    f.seek(16*2048); pvd=f.read(2048)
    root_lba=r32(pvd,158)
    f.seek(root_lba*2048); data=f.read(r32(pvd,166))
    pos=0; tsdata_lba=None
    while pos<len(data):
        rec=data[pos]
        if rec==0: break
        nl=data[pos+32]
        nm=data[pos+33:pos+33+nl].decode('ascii','replace').split(';')[0]
        if nm=='TSDATA.EXE': tsdata_lba=r32(data,pos+2)
        pos+=rec
    print(f"TSDATA.EXE LBA={tsdata_lba}")

    # Find RAR
    rar_off=None
    for base in range(0,64*1024*1024,65529):
        chunk=iso_read(f,tsdata_lba,base,65536)
        i=chunk.find(RAR_SIG)
        if i!=-1: rar_off=base+i; break
    print(f"RAR offset=0x{rar_off:X}")

    # Walk RAR, find BodyShopThumbnails and Skins.package
    pos2=rar_off; bst_doff=None; bst_sz=None; skins_doff=None; skins_sz=None
    while True:
        hdr=iso_read(f,tsdata_lba,pos2,40)
        if len(hdr)<7: break
        bt=hdr[2]; bf=r16(hdr,3); bs=r16(hdr,5)
        if bs==0 or bt==0x7B: break
        if bt==0x74:
            psize=r32(hdr,7); nsize=r16(hdr,26); hoff=32
            if bf&0x0100:
                ex=iso_read(f,tsdata_lba,pos2+32,8)
                psize|=r32(ex,0)<<32; hoff=40
            rnm=iso_read(f,tsdata_lba,pos2+hoff,nsize).decode('ascii','replace').replace('\\','/')
            doff=pos2+bs
            if 'BodyShopThumbnails' in rnm:
                bst_doff=doff; bst_sz=psize
                print(f"Found BodyShopThumbnails at offset=0x{doff:X} size={psize//1024}KB")
            elif 'Skins.package' in rnm and 'Locale' not in rnm:
                skins_doff=doff; skins_sz=psize
                print(f"Found Skins.package at offset=0x{doff:X} size={psize//1024}KB")
            if bst_doff and skins_doff: break
            pos2=doff+psize
        else:
            pos2+=bs+(r32(hdr,7) if bf&0x8000 else 0)

    # Read Skins.package entirely into memory
    print(f"\nReading Skins.package ({skins_sz//1024}KB)...")
    skins_data=iso_read(f,tsdata_lba,skins_doff,skins_sz)
    assert skins_data[:4]==b'DBPF', "Not DBPF!"
    skins_cnt=r32(skins_data,36); skins_ioff=r32(skins_data,40)
    print(f"  {skins_cnt} entries")
    BINARY_SKIN=0xEBCF3E27; XML_SKIN=0x0C1FE246; RKL=0xAC506764
    bin_insts=set(); xml_insts=set(); rkl_insts=set()
    for i in range(skins_cnt):
        p=skins_ioff+i*20
        t=r32(skins_data,p); ins=r32(skins_data,p+8)
        if t==BINARY_SKIN: bin_insts.add(ins)
        elif t==XML_SKIN: xml_insts.add(ins)
        elif t==RKL: rkl_insts.add(ins)
    print(f"  binary SKIN_ENTRY: {len(bin_insts)}, XML: {len(xml_insts)}, RKL: {len(rkl_insts)}")

    # Read BodyShopThumbnails DBPF header to get index location
    bst_hdr=iso_read(f,tsdata_lba,bst_doff,96)
    assert bst_hdr[:4]==b'DBPF'
    bst_cnt=r32(bst_hdr,36); bst_ioff=r32(bst_hdr,40)
    print(f"\nBodyShopThumbnails: {bst_cnt} entries, index at offset {bst_ioff}")

    # Read BST DBPF index
    bst_idx=iso_read(f,tsdata_lba,bst_doff+bst_ioff,bst_cnt*20)
    JPEG=0x856DDBAC; CATALOG_IDX=0x43494745
    cat_off=None; cat_sz=None; jpeg_insts=set()
    for i in range(bst_cnt):
        p=i*20; t=r32(bst_idx,p); ins=r32(bst_idx,p+8); off=r32(bst_idx,p+12); sz=r32(bst_idx,p+16)
        if t==JPEG: jpeg_insts.add(ins)
        elif t==CATALOG_IDX: cat_off=off; cat_sz=sz
    print(f"  {len(jpeg_insts)} JPEG thumbnails, catalog at offset {cat_off} size={cat_sz}")

    # Read and decompress the catalog index
    raw=iso_read(f,tsdata_lba,bst_doff+cat_off,cat_sz)
    dec=refpack_decomp(raw)
    print(f"  Catalog: compressed={cat_sz} -> decompressed={len(dec)}")

    ver=r32(dec,0); cnt=r32(dec,4)
    esz=(len(dec)-8)//cnt if cnt else 0
    print(f"  ver={ver} count={cnt} entry_size={esz}")

    # Analyze all entries
    src_grps=collections.Counter()
    src_insts_set=set()
    matched_bin=0; matched_xml=0; matched_rkl=0; in_bst=0
    seen_grps={}

    for i in range(cnt):
        base=8+i*esz
        if base+esz>len(dec): break
        src_type=r32(dec,base+4)
        src_grp =r32(dec,base+8)
        src_inst=r32(dec,base+12)
        tinst   =r32(dec,base+28)

        src_grps[src_grp]+=1
        src_insts_set.add(src_inst)
        if src_inst in bin_insts: matched_bin+=1
        if src_inst in xml_insts: matched_xml+=1
        if src_inst in rkl_insts: matched_rkl+=1
        if tinst in jpeg_insts:   in_bst+=1

        if src_grp not in seen_grps:
            seen_grps[src_grp]=(i,src_type,src_inst,tinst)

    print(f"\nsrc_grp distribution ({len(src_grps)} unique groups):")
    for grp,c in src_grps.most_common():
        ei,st,si,ti=seen_grps[grp]
        print(f"  0x{grp:08X}: {c:5d} entries  (first at idx {ei}: src_type=0x{st:08X} src_inst=0x{si:08X})")

    print(f"\nCross-reference with Skins.package:")
    print(f"  src_inst in binary SKIN_ENTRY: {matched_bin} / {cnt}")
    print(f"  src_inst in XML SKIN_ENTRY:    {matched_xml} / {cnt}")
    print(f"  src_inst in RKL:               {matched_rkl} / {cnt}")
    print(f"  thumb_inst in BST:             {in_bst} / {cnt}")

    print(f"\nFirst 3 src_insts from catalog:")
    for i in range(3):
        base=8+i*esz
        sg=r32(dec,base+8); si=r32(dec,base+12); ti=r32(dec,base+28)
        print(f"  Entry {i}: src_grp=0x{sg:08X} src_inst=0x{si:08X} thumb_inst=0x{ti:08X}")
        print(f"    -> src_inst in bin_insts: {si in bin_insts}, xml_insts: {si in xml_insts}")
