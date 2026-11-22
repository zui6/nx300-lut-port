import struct, hashlib, os
def load(p):
    d=open(p,"rb").read()
    h=struct.unpack("<HHIIIIIHHHHHH", d[16:52])
    shoff,shentsz,shnum,shstrndx=h[5],h[10],h[11],h[12]
    S=[]
    for i in range(shnum):
        o=shoff+i*shentsz
        f=struct.unpack("<IIIIIIIIII", d[o:o+40])
        S.append(dict(name=f[0],typ=f[1],flags=f[2],addr=f[3],off=f[4],size=f[5],link=f[6],entsize=f[9]))
    shstr=S[shstrndx]
    for s in S:
        a=shstr['off']+s['name']; b=d.find(b"\0",a); s['nm']=d[a:b].decode('latin1')
    sy={}
    ds=[s for s in S if s['nm']=='.dynsym'][0]
    strt=S[ds['link']]
    for i in range(ds['size']//16):
        o=ds['off']+i*16
        st_name,st_value,st_size,st_info,st_other,st_shndx=struct.unpack("<IIIBBH", d[o:o+16])
        if not st_name: continue
        a=strt['off']+st_name; b=d.find(b"\0",a)
        sy[d[a:b].decode('latin1')]=(st_value,st_size)
    return d,S,sy
def bytes_vaddr(d,S,va,n):
    for s in S:
        if s['typ']==1 and s['addr']<=va and va+n<=s['addr']+s['size']:
            return d[s['off']+(va-s['addr']):s['off']+(va-s['addr'])+n]
    raise SystemExit(f"no mapping for {va:#x}")
files={"iq":r"bin\runtime\libiq-fw-slpcam.so","udd":r"bin\camlibs\libudd.so.3.1",
       "libc":r"bin\syslib\libc-2.13.so","app":r"bin\di-camera-app-nx300"}
INFO={}
for k,p in files.items():
    d,S,sy=load(p)
    INFO[k]=dict(d=d,S=S,sy=sy,size=os.path.getsize(p),sha=hashlib.sha256(open(p,'rb').read()).hexdigest())
    print(f"{k:5s} size={INFO[k]['size']:>9} sha={INFO[k]['sha'][:16]}...")
iq=INFO['iq']; udd=INFO['udd']; lc=INFO['libc']; app=INFO['app']
U=iq['sy']['_ZN17CIpcParameterNode6UpdateEPv'][0]
R=iq['sy']['_ZN17CIpcParameterNode5ResetEv'][0]
LUT=udd['sy']['ipcs_3d_lut'][0]
VT=iq['sy']['_ZTV12CIpcs3DLutD4'][0]
RED=iq['sy']['_ZN12CIpcs3DLutD47m_sRLutE'][0]
plt=[s for s in iq['S'] if s['nm']=='.plt'][0]['addr']
got=None
for s in iq['S']:
    if s['nm'] in ('.rel.plt','.rel.dyn') and s['size']:
        for i in range(s['size']//8):
            o=s['off']+i*8
            r_off,r_info=struct.unpack("<II", iq['d'][o:o+8])
            sym=r_info>>8; typ=r_info&0xff
            nm=None
            ds=[x for x in iq['S'] if x['nm']=='.dynsym'][0]; strt=iq['S'][ds['link']]
            a=strt['off']+struct.unpack("<I",iq['d'][ds['off']+sym*16:ds['off']+sym*16+4])[0]
            b=iq['d'].find(b"\0",a); nm=iq['d'][a:b].decode('latin1')
            if nm=='ipcs_3d_lut' and typ==22: got=r_off
# svc in libc syscall
sv=lc['sy']['syscall'][0]; svn=lc['sy']['syscall'][1]
sdat=bytes_vaddr(lc['d'],lc['S'],sv,svn)
svc=next(sv+i for i in range(0,len(sdat),4) if sdat[i:i+4]==b'\x00\x00\x00\xef')
print(f"\nupdate={U:#x} reset={R:#x} lut={LUT:#x}")
print(f"vtable={VT:#x} vtable_slot={VT+8:#x} lut_got={got:#x} resolver={plt:#x} factory_red={RED:#x} svc={svc:#x}")
out=[]
out.append("/* Generated from NX300 1.45 ELF (port of NX300M 1.15 loader); do not edit. */")
out.append(f'#define STOCK_UPDATE {U}U')
out.append(f'#define STOCK_RESET {R}U')
out.append(f'#define STOCK_LUT {LUT}U')
out.append(f'#define STOCK_VTABLE {VT}U')
out.append(f'#define STOCK_LUT_GOT {got}U')
out.append(f'#define STOCK_LUT_RESOLVER {plt}U')
out.append(f'#define STOCK_SVC {svc}U')
out.append(f'#define STOCK_FACTORY_RED {RED}U')
for key,elf,addr in (("update",iq,U),("reset",iq,R),("lut",udd,LUT)):
    raw=bytes_vaddr(elf['d'],elf['S'],addr,32)
    out.append(f'static const unsigned char stock_{key}[32] = {{' + ','.join(str(x) for x in raw) + '};')
raw=bytes_vaddr(iq['d'],iq['S'],plt,32)
out.append('static const unsigned char stock_lut_resolver[32] = {' + ','.join(str(x) for x in raw) + '};')
raw=bytes_vaddr(lc['d'],lc['S'],svc,32)
out.append('static const unsigned char stock_svc[32] = {' + ','.join(str(x) for x in raw) + '};')
for key in ("iq","udd","libc","app"):
    out.append(f'#define STOCK_{key.upper()}_SIZE {INFO[key]["size"]}U')
    out.append(f'#define STOCK_{key.upper()}_SHA "{INFO[key]["sha"]}"')
os.makedirs("work/nx300build",exist_ok=True)
open("work/nx300build/lut_live_stock.h","w",encoding="ascii",newline="\n").write("\n".join(out)+"\n")
print(f"\nwrote work/nx300build/lut_live_stock.h ({len(out)} lines)")
