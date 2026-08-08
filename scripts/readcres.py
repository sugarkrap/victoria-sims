"""Decode the CRES (scenegraph) in CAS!.package looking for camera transforms.
The CRES/RNode format stores a list of named nodes each with a 4x3 transform."""

import struct, sys

ISO    = "C:/Users/meyer.n/Downloads/Sims2.iso"
RAR_SIG= b'\x52\x61\x72\x21\x1a\x07\x00'
PKG    = "TSData/Res/GlobalLots/CAS!.package"
CRES_TYPE = 0xE519C933
GMND_TYPE = 0x7BA3838C

def r32(d,o): return struct.unpack_from('<I', d, o)[0]
def r16(d,o): return struct.unpack_from('<H', d, o)[0]
def rf32(d,o): return struct.unpack_from('<f', d, o)[0]
def iso_read(f,lba,off,n): f.seek(lba*2048+off); return f.read(n)

def get_tsdata_lba(f):
    f.seek(16*2048); pvd=f.read(2048)
    root_lba=r32(pvd,158)
    f.seek(root_lba*2048); data=f.read(r32(pvd,166))
    pos=0
    while pos<len(data):
        rec=data[pos]
        if rec==0: break
        nl=data[pos+32]; name=data[pos+33:pos+33+nl].decode('ascii',errors='replace').split(';')[0]
        if name=='TSDATA.EXE': return r32(data,pos+2)
        pos+=rec
    return None

def find_rar(f,lba):
    for base in range(0,64*1024*1024,65529):
        chunk=iso_read(f,lba,base,65536)
        i=chunk.find(RAR_SIG)
        if i!=-1: return base+i
    return None

def find_pkg(f,lba,rar,name_target):
    pos=rar
    while True:
        hdr=iso_read(f,lba,pos,40)
        if len(hdr)<7: break
        bt=hdr[2]; bf=r16(hdr,3); bs=r16(hdr,5)
        if bs==0 or bt==0x7B: break
        if bt==0x74:
            ps=r32(hdr,7); ns=r16(hdr,26)
            ho=32
            if bf&0x0100: ex=iso_read(f,lba,pos+32,8); ps|=r32(ex,0)<<32; ho=40
            nm=iso_read(f,lba,pos+ho,ns).decode('ascii','replace').replace(chr(92),'/')
            do=pos+bs
            if nm==name_target: return do,ps
            pos=do+ps
        else:
            pos+=bs+(r32(hdr,7) if bf&0x8000 else 0)
    return None,None

def dbpf_resources(f,lba,pkg_off,want_types):
    hdr=iso_read(f,lba,pkg_off,96)
    ec=r32(hdr,36); io=r32(hdr,40); isz=r32(hdr,44)
    es=(isz//ec) if ec else 20
    idx=iso_read(f,lba,pkg_off+io,isz)
    res=[]
    pos=0
    for _ in range(ec):
        if pos+es>len(idx): break
        tid=r32(idx,pos); grp=r32(idx,pos+4); inst=r32(idx,pos+8)
        if es>=24: off=r32(idx,pos+16); sz=r32(idx,pos+20)
        else:      off=r32(idx,pos+12); sz=r32(idx,pos+16)
        if tid in want_types: res.append((tid,grp,inst,off,sz))
        pos+=es
    return res

def read_resource(f,lba,pkg_off,off,sz):
    """Read resource, decompressing if RefPack."""
    raw=iso_read(f,lba,pkg_off+off,sz)
    if len(raw)>=6 and raw[4:6]==b'\x10\xfb':
        decsz=(raw[6]<<16)|(raw[7]<<8)|raw[8]
        return refpack_decomp(raw[9:], decsz)
    return raw

def refpack_decomp(src, decsz):
    dst=bytearray(decsz)
    rp=0; wp=0
    while rp<len(src):
        c=src[rp]; rp+=1
        if c<0x80:
            if rp>=len(src): break
            c2=src[rp]; rp+=1
            lc=(c&0x03); cc=((c&0x1C)>>2)+3; dist=((c&0x60)<<3)|c2+1
            for _ in range(lc): dst[wp]=src[rp]; wp+=1; rp+=1
            for _ in range(cc): dst[wp]=dst[wp-dist]; wp+=1
        elif c<0xC0:
            if rp+1>=len(src): break
            c2=src[rp]; c3=src[rp+1]; rp+=2
            lc=(c2>>6)&0x03; cc=(c&0x3F)+4; dist=(((c2&0x3F))<<8)|c3+1
            for _ in range(lc): dst[wp]=src[rp]; wp+=1; rp+=1
            for _ in range(cc): dst[wp]=dst[wp-dist]; wp+=1
        elif c<0xE0:
            if rp+2>=len(src): break
            c2=src[rp]; c3=src[rp+1]; c4=src[rp+2]; rp+=3
            lc=(c&0x03); cc=((c&0x0C)<<6)|c4+5; dist=(((c&0x10)<<12)|((c2)<<8)|c3)+1
            for _ in range(lc): dst[wp]=src[rp]; wp+=1; rp+=1
            for _ in range(cc): dst[wp]=dst[wp-dist]; wp+=1
        else:
            lc=(c&0x03); cc=0; dist=0
            for _ in range(lc): dst[wp]=src[rp]; wp+=1; rp+=1
            if lc==0: break
    return bytes(dst[:wp])

def parse_cres(data):
    """Parse a CRES resource and print node names and transforms."""
    pos=0
    # CRES header: version (uint32), then node list
    if len(data)<4: return
    version=r32(data,0); pos=4
    print(f"CRES version: {version}")
    # Read number of nodes
    if pos+4>len(data): return
    node_count=r32(data,pos); pos+=4
    print(f"Node count: {node_count}")
    for i in range(min(node_count,100)):
        if pos+4>len(data): break
        name_len=r32(data,pos); pos+=4
        name=data[pos:pos+name_len].decode('latin-1','replace'); pos+=name_len
        # try to read a 4x3 transform (12 floats = 48 bytes)
        if pos+48<=len(data):
            m=struct.unpack_from('<12f', data, pos)
            print(f"  [{i}] {name!r}")
            print(f"       row0: ({m[0]:.3f}, {m[1]:.3f}, {m[2]:.3f})")
            print(f"       row1: ({m[3]:.3f}, {m[4]:.3f}, {m[5]:.3f})")
            print(f"       row2: ({m[6]:.3f}, {m[7]:.3f}, {m[8]:.3f})")
            print(f"       pos:  ({m[9]:.3f}, {m[10]:.3f}, {m[11]:.3f})")
        else:
            print(f"  [{i}] {name!r}")
        # skip rest of node (variable size, scan for next string)
        pos+=48  # crude: advance past transform
        # we might get lost — stop if data looks bad
        if pos>=len(data): break

with open(ISO,'rb') as f:
    lba=get_tsdata_lba(f)
    rar=find_rar(f,lba)
    pkg_off,_=find_pkg(f,lba,rar,PKG)
    resources=dbpf_resources(f,lba,pkg_off,{CRES_TYPE})
    print(f"Found {len(resources)} CRES entries")
    for tid,grp,inst,off,sz in resources[:3]:
        print(f"\n=== CRES grp=0x{grp:08X} inst=0x{inst:08X} sz={sz} ===")
        data=read_resource(f,lba,pkg_off,off,sz)
        print(f"Decompressed: {len(data)} bytes")
        # Raw hex dump of first 256 bytes
        print("First 256 bytes:")
        for i in range(0,min(256,len(data)),16):
            chunk=data[i:i+16]
            h=' '.join(f'{b:02x}' for b in chunk)
            p=''.join(chr(b) if 32<=b<127 else '.' for b in chunk)
            print(f"  {i:04x}: {h:48s}  {p}")
