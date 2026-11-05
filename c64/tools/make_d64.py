#!/usr/bin/env python3
"""Makes a .d64 disk image holding one program - for testing the disk drive.

    python3 c64/tools/make_d64.py out.d64 NAME program.prg

The disk is called "PS5 TEST DISK"; the program is NAME on it (LOAD"NAME",8
or LOAD"*",8,1). The menu attaches the disk and types LOAD"*",8,1 and RUN.
"""
import sys
out,name,prg=sys.argv[1],sys.argv[2].upper(),open(sys.argv[3],'rb').read()
spt=lambda t:21 if t<=17 else 19 if t<=24 else 18 if t<=30 else 17
off=lambda t,s: (sum(spt(x) for x in range(1,t))+s)*256
img=bytearray(683*256)
used={}
# file sectors on track 17 then 16...
chain=[];data=prg;t=17;s=0
while data:
    chain.append((t,s)); data=data[254:]; s+=1
    if s>=spt(t): t-=1; s=0
data=prg
for i,(t,s) in enumerate(chain):
    part=data[:254]; data=data[254:]
    o=off(t,s)
    if i+1<len(chain): img[o]=chain[i+1][0]; img[o+1]=chain[i+1][1]
    else: img[o]=0; img[o+1]=len(part)+1
    img[o+2:o+2+len(part)]=part
    used.setdefault(t,set()).add(s)
used.setdefault(18,set()).update({0,1})
# BAM
o=off(18,0); img[o]=18; img[o+1]=1; img[o+2]=0x41
for t in range(1,36):
    free=[x for x in range(spt(t)) if x not in used.get(t,set())]
    b=0
    for x in free: b|=1<<x
    img[o+4*t]=len(free); img[o+4*t+1]=b&255; img[o+4*t+2]=(b>>8)&255; img[o+4*t+3]=(b>>16)&255
dn=b'PS5 TEST DISK'.ljust(16,b'\xa0'); img[o+0x90:o+0xa0]=dn; img[o+0xa0:o+0xa2]=b'\xa0\xa0'; img[o+0xa2:o+0xa4]=b'PS'; img[o+0xa4]=0xa0; img[o+0xa5:o+0xa7]=b'2A'; img[o+0xa7:o+0xab]=b'\xa0'*4
# directory
o=off(18,1); img[o]=0; img[o+1]=0xff
e=o+2; img[e]=0x82; img[e+1]=chain[0][0]; img[e+2]=chain[0][1]
img[e+3:e+19]=name.encode().ljust(16,b'\xa0'); img[e+28]=len(chain)&255; img[e+29]=len(chain)>>8
open(out,'wb').write(img)
print('ok',len(chain),'blocks')
