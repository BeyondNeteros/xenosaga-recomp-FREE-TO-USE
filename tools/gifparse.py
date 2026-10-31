import struct, sys, collections
data=open(sys.argv[1],'rb').read()
REG={0:'PRIM',1:'RGBAQ',2:'ST',3:'UV',4:'XYZF2',5:'XYZ2',6:'TEX0_1',7:'TEX0_2',8:'CLAMP_1',9:'CLAMP_2',10:'FOG',12:'XYZF3',13:'XYZ3',14:'AD',15:'NOP'}
off=0; npk=0; prims=collections.Counter(); xs=[]; ys=[]; zs=[]; adregs=collections.Counter(); qs=[]; alphas=[]
show=int(sys.argv[2]) if len(sys.argv)>2 else 3
while off+4<=len(data):
    size=struct.unpack_from('<I',data,off)[0]; off+=4; pk=data[off:off+size]; off+=size; npk+=1
    p=0
    while p+16<=len(pk):
        lo,hi=struct.unpack_from('<QQ',pk,p); p+=16
        nloop=lo&0x7fff; eop=(lo>>15)&1; pre=(lo>>46)&1; prim=(lo>>47)&0x7ff; flg=(lo>>58)&3; nreg=(lo>>60)&0xf or 16
        regs=[(hi>>(4*i))&0xf for i in range(nreg)]
        if npk<=show: print(f'pk{npk} tag nloop={nloop} eop={eop} pre={pre} prim={prim:#x} flg={flg} regs={[REG.get(r,r) for r in regs]}')
        if pre: prims[prim&7]+=1
        if flg==0:
            for l in range(nloop):
                for r in regs:
                    a,b=struct.unpack_from('<QQ',pk,p); p+=16
                    if r==5 or r==4:
                        xs.append(a&0xffff); ys.append((a>>32)&0xffff); zs.append(b&0xffffffff if r==5 else (b>>4)&0xffffff)
                    elif r==1:
                        alphas.append((a>>32)&0xff); qs.append(struct.unpack('<f',struct.pack('<I',b&0xffffffff))[0])
                    elif r==14:
                        adregs[b&0xff]+=1
                        if npk<=show: print(f'   AD reg {b&0xff:#x} = {a:#018x}')
                    elif r==0: prims[a&7]+=1
        elif flg==1:
            n=nloop*nreg; 
            for k in range(n):
                v=struct.unpack_from('<Q',pk,p+8*k)[0]; r=regs[k%nreg]
                if r==5 or r==4: xs.append(v&0xffff); ys.append((v>>16)&0xffff); zs.append(v>>32)
                elif r==1: alphas.append((v>>24)&0xff)
                elif r==0: prims[v&7]+=1
            p+=((n+1)//2)*16
        else:
            p+=nloop*16
        if eop: break
print('packets',npk,'prim types',dict(prims),'AD regs',{hex(k):v for k,v in adregs.items()})
import statistics as st
if xs:
    print('X/16 range', min(xs)/16, max(xs)/16, 'median', st.median(xs)/16)
    print('Y/16 range', min(ys)/16, max(ys)/16, 'median', st.median(ys)/16)
    print('Z range', min(zs), max(zs), 'nverts', len(xs))
if alphas: print('alpha range', min(alphas), max(alphas), 'Q sample', qs[:5])
