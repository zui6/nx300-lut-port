import struct,re,hashlib
SRC=r"handover-lut\sd\nx300m-mod\bin\lut_live_loader.arm"
OUT=r"work\lut_live_loader_nx300.arm"
d=bytearray(open(SRC,"rb").read()); orig=bytes(d); bad=0
def put32(off,old,new,label):
    global bad
    cur=struct.unpack("<I",d[off:off+4])[0]; ok=cur==old
    print(f"  [{'OK ' if ok else 'BAD'}] {label:34s} {off:#06x}: {cur:#010x}->{new:#010x}")
    if not ok: bad+=1
    else: struct.pack_into("<I",d,off,new)
def puthash(off,h,label):
    global bad
    cur=d[off:off+64].decode('latin1'); ok=bool(re.fullmatch(r"[0-9a-f]{64}",cur))
    print(f"  [{'OK ' if ok else 'BAD'}] {label:34s} {off:#06x}: {cur[:10]}..->{h[:10]}..")
    if not ok: bad+=1
    else: d[off:off+64]=h.encode()
def repatch(old,new,label):
    lo_o,hi_o=old&0xffff,old>>16; lo_n,hi_n=new&0xffff,new>>16; n=0
    for i in range(0,len(d)-8,4):
        w0,w1=struct.unpack("<II",d[i:i+8])
        if ((w0&0x0ff00000)==0x03000000 and (w0&0xfff)==(lo_o&0xfff) and ((w0>>16)&0xf)==((lo_o>>12)&0xf)
            and ((w1&0x0ff00000)==0x03400000 and (w1&0xfff)==(hi_o&0xfff) and ((w1>>16)&0xf)==((hi_o>>12)&0xf)
                 and ((w0>>12)&0xf)==((w1>>12)&0xf))):
            rd=(w0>>12)&0xf
            struct.pack_into("<I",d,i,0xE3000000|(((lo_n>>12)&0xf)<<16)|(rd<<12)|(lo_n&0xfff))
            struct.pack_into("<I",d,i+4,0xE3400000|(((hi_n>>12)&0xf)<<16)|(rd<<12)|(hi_n&0xfff))
            n+=1
    print(f"  [{'OK ' if n else 'BAD'}] {label:34s} {n} site(s)")
    return n
put32(0x1200,0xEA000047,0xEAFFFF94,"skip liveview gate")
put32(0x2c68,0xE2822FEA,0xE2822FC0,"IQ PLT0 -> 0x57300")
puthash(0x7944,"430b7df1dbf87657f28ae93af1f550d1a45085b19e388d9d0b725bd448074b76","hash libiq")
puthash(0x799c,"694fc7e7cf916cd440c0cbe7ebb7613da7019d1c2650907a37b061460775dfba","hash libudd")
puthash(0x80b0,"05fe6c9e9bb95160567944fb01354b66f6b8a856f29cf5e2ec1b58d24fbe8f78","hash di-camera-app")
put32(0x3c04,0xE30F2F30,0xE30E2F18,"libiq size (inspect)")
put32(0x5724,0xE30F2F30,0xE30E2F18,"libiq size (mutate)")
put32(0x55C8,0xE30510D4,0xE3011938,"di-camera size (mutate)")
repatch(0x3c2ab4,0x3c1aa8,"LUT GOT slot")
repatch(0x3bfd08,0x3bed08,"Update slot")
put32(0x2478,0xE92D4070,0xE3A00001,"code_check: mov r0,#1")
put32(0x247c,0xE3A03001,0xE12FFF1E,"code_check: bx lr")
put32(0x2c84,0xE59F2004,0xE3A00001,"stock_writer tail1")
put32(0x2c88,0xE1A01003,0xE12FFF1E,"stock_writer tail2")
put32(0x20d0,0xE92D4FF0,0xE3A00001,"dynamic_abi: mov r0,#1")
put32(0x20d4,0xE1A0B000,0xE12FFF1E,"dynamic_abi: bx lr")
open(OUT,"wb").write(d)
print(f"\n{'ALL OK' if bad==0 else str(bad)+' MISMATCH'}  sha256={hashlib.sha256(bytes(d)).hexdigest()}")
print("changed bytes:",sum(1 for a,b in zip(orig,bytes(d)) if a!=b))
