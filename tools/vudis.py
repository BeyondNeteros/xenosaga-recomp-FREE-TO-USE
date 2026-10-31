import struct,sys
def dest(i): 
    d=(i>>21)&0xf; return '.'+''.join(c for c,b in zip('xyzw',(8,4,2,1)) if d&b)
def ft(i): return (i>>16)&31
def fs(i): return (i>>11)&31
def fd(i): return (i>>6)&31
BC='xyzw'
UP={0:'add',1:'sub',2:'madd',3:'msub',4:'max',5:'mini',6:'mul'}
def upper(i):
    op=i&63; flags=('I' if i>>31&1 else '')+('E' if i>>30&1 else '')+('M' if i>>29&1 else '')+('D' if i>>28&1 else '')+('T' if i>>27&1 else '')
    f=f'[{flags}]' if flags else ''
    if op<0x1c: return f'{UP[op>>2]}{BC[op&3]}{dest(i)} vf{fd(i)}, vf{fs(i)}, vf{ft(i)}{BC[op&3]}'+f
    names={0x1c:'mulq',0x1d:'maxi',0x1e:'muli',0x1f:'minii',0x20:'addq',0x21:'maddq',0x22:'addi',0x23:'maddi',0x24:'subq',0x25:'msubq',0x26:'subi',0x27:'msubi',0x28:'add',0x29:'madd',0x2a:'mul',0x2b:'max',0x2c:'sub',0x2d:'msub',0x2e:'opmsub',0x2f:'mini'}
    if op in names: return f'{names[op]}{dest(i)} vf{fd(i)}, vf{fs(i)}, vf{ft(i)}'+f
    if op>=0x3c:
        s=(i&3)|((i>>4)&0x7c)
        sn={0x10:'itof0',0x11:'itof4',0x12:'itof12',0x13:'itof15',0x14:'ftoi0',0x15:'ftoi4',0x16:'ftoi12',0x17:'ftoi15',0x1c:'mulaq',0x1d:'abs',0x1e:'mulai',0x1f:'clip',0x20:'addaq',0x21:'maddaq',0x22:'addai',0x23:'maddai',0x24:'subaq',0x25:'msubaq',0x26:'subai',0x27:'msubai',0x28:'adda',0x29:'madda',0x2a:'mula',0x2c:'suba',0x2d:'msuba',0x2e:'opmula',0x2f:'nop',0x30:'nop'}
        if s<0x10: return f"{['adda','suba','madda','msuba'][s>>2]}{BC[s&3]}{dest(i)} ACC, vf{fs(i)}, vf{ft(i)}{BC[s&3]}"+f
        if 0x18<=s<=0x1b: return f'mula{BC[s&3]}{dest(i)} ACC, vf{fs(i)}, vf{ft(i)}{BC[s&3]}'+f
        n=sn.get(s,f'?up{s:#x}')
        if n=='nop': return 'nop'+f
        if n.startswith(('itof','ftoi','abs')): return f'{n}{dest(i)} vf{ft(i)}, vf{fs(i)}'+f
        return f'{n}{dest(i)} vf{fs(i)}, vf{ft(i)}'+f
    return f'?upper {i:08x}'+f
def imm11(i):
    v=i&0x7ff; return v-0x800 if v&0x400 else v
def vit(i): return (i>>16)&15
def vis(i): return (i>>11)&15
def vid(i): return (i>>6)&15
def lower(i,pc):
    if i in (0,0x8000033c): return 'nop'
    op=(i>>25)&0x7f
    tgt=lambda: f'0x{(pc+8+imm11(i)*8)&0x3fff:04x}'
    m={0:f'lq{dest(i)} vf{ft(i)}, {imm11(i)}(vi{vis(i)})',1:f'sq{dest(i)} vf{fs(i)}, {imm11(i)}(vi{vit(i)})',4:f'ilw{dest(i)} vi{vit(i)}, {imm11(i)}(vi{vis(i)})',5:f'isw{dest(i)} vi{vit(i)}, {imm11(i)}(vi{vis(i)})',
       8:f'iaddiu vi{vit(i)}, vi{vis(i)}, {(i&0x7ff)|((i>>10)&0x7800)}',9:f'isubiu vi{vit(i)}, vi{vis(i)}, {(i&0x7ff)|((i>>10)&0x7800)}',
       0x10:f'fceq vi1, {i&0xffffff:#x}',0x11:f'fcset {i&0xffffff:#x}',0x12:f'fcand vi1, {i&0xffffff:#x}',0x13:f'fcor vi1, {i&0xffffff:#x}',0x14:'fseq',0x15:'fsset',0x16:f'fsand vi{vit(i)}',0x17:'fsor',0x18:f'fmeq vi{vit(i)}, vi{vis(i)}',0x1a:f'fmand vi{vit(i)}, vi{vis(i)}',0x1b:f'fmor vi{vit(i)}, vi{vis(i)}',0x1c:f'fcget vi{vit(i)}',
       0x20:f'b {tgt()}',0x21:f'bal vi{vit(i)}, {tgt()}',0x24:f'jr vi{vis(i)}',0x25:f'jalr vi{vit(i)}, vi{vis(i)}',0x28:f'ibeq vi{vit(i)}, vi{vis(i)}, {tgt()}',0x29:f'ibne vi{vit(i)}, vi{vis(i)}, {tgt()}',0x2c:f'ibltz vi{vis(i)}, {tgt()}',0x2d:f'ibgtz vi{vis(i)}, {tgt()}',0x2e:f'iblez vi{vis(i)}, {tgt()}',0x2f:f'ibgez vi{vis(i)}, {tgt()}'}
    if op in m: return m[op]
    if op==0x40:
        f=i&63
        if f==0x30: return f'iadd vi{vid(i)}, vi{vis(i)}, vi{vit(i)}'
        if f==0x31: return f'isub vi{vid(i)}, vi{vis(i)}, vi{vit(i)}'
        if f==0x32: 
            v=(i>>6)&31; v=v-32 if v&16 else v; return f'iaddi vi{vit(i)}, vi{vis(i)}, {v}'
        if f==0x34: return f'iand vi{vid(i)}, vi{vis(i)}, vi{vit(i)}'
        if f==0x35: return f'ior vi{vid(i)}, vi{vis(i)}, vi{vit(i)}'
        if f>=0x3c:
            s=(i&3)|((i>>4)&0x7c)
            fsf='xyzw'[(i>>21)&3]; ftf='xyzw'[(i>>23)&3]
            n={0x30:f'move{dest(i)} vf{ft(i)}, vf{fs(i)}',0x31:f'mr32{dest(i)} vf{ft(i)}, vf{fs(i)}',0x34:f'lqi{dest(i)} vf{ft(i)}, (vi{vis(i)}++)',0x35:f'sqi{dest(i)} vf{fs(i)}, (vi{vit(i)}++)',0x36:f'lqd{dest(i)} vf{ft(i)}, (--vi{vis(i)})',0x37:f'sqd{dest(i)} vf{fs(i)}, (--vi{vit(i)})',
               0x38:f'div Q, vf{fs(i)}{fsf}, vf{ft(i)}{ftf}',0x39:f'sqrt Q, vf{ft(i)}{ftf}',0x3a:f'rsqrt Q, vf{fs(i)}{fsf}, vf{ft(i)}{ftf}',0x3b:'waitq',0x3c:f'mtir vi{vit(i)}, vf{fs(i)}{fsf}',0x3d:f'mfir{dest(i)} vf{ft(i)}, vi{vis(i)}',0x3e:f'ilwr{dest(i)} vi{vit(i)}, (vi{vis(i)})',0x3f:f'iswr{dest(i)} vi{vit(i)}, (vi{vis(i)})',
               0x40:'rnext',0x41:'rget',0x42:'rinit',0x43:'rxor',0x64:f'mfp{dest(i)} vf{ft(i)}, P',0x68:f'xtop vi{vit(i)}',0x69:f'xitop vi{vit(i)}',0x6c:f'xgkick vi{vis(i)}',0x70:'esadd',0x71:'ersadd',0x72:'eleng',0x73:'erleng',0x74:'eatanxy',0x75:'eatanxz',0x76:'esum',0x77:'ersqrt',0x78:'esqrt',0x79:'esin',0x7a:f'ercpr P, vf{fs(i)}{fsf}',0x7b:'waitp',0x7c:'eatan',0x7d:'eexp'}.get(s)
            if n: return n
    return f'?lower {i:08x}'
def dis(code, start, end):
    for pc in range(start,end,8):
        lo,up=struct.unpack_from('<II',code,pc)
        l = f'loi {struct.unpack("<f",struct.pack("<I",lo))[0]:.6g}' if up>>31 else lower(lo,pc)
        print(f'{pc:04x}: {upper(up):42s} | {l}')
if __name__=='__main__':
    d=open(sys.argv[1],'rb').read(); rec=int(sys.argv[2]); ssz=664
    off=16+rec*(16+ssz+32768)+16+ssz
    code=d[off:off+16384]
    dis(code,int(sys.argv[3],0),int(sys.argv[4],0))
