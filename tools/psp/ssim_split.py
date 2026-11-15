#!/usr/bin/env python3
"""M6 (docs/PSP_PORT.md 14.11): SSIM split into the field and everything else.

  ssim_split.py <scratch dir> <run dir> [dump numbers]

For each dump, tools/gbi_dump.py --draws --render gives the reference's per-pixel draw ids; pixels drawn by a draw in
detail-texture mode (textdetail=DETAIL: the field) are one group, all others the other. The SSIM map (compare_shots.py's
7 x 7 window) of <run>/rdram_<n>_ref.png against rdram_<n>_psp.png is averaged per group. The id image is scaled to
480 x 272 with nearest sampling, so group edges are approximate. Writes renders into <scratch dir>.
"""
import re
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from compare_shots import load_ref, luma, integral, W, H
from PIL import Image
root=str(Path(__file__).resolve().parents[2]); S=sys.argv[1]; R=sys.argv[2]
for n in [int(x) for x in sys.argv[3:]] or (543,1439,1448,1529,1538):
    dl=re.search(r'dlist=(\w+)',open(f'{root}/build/dumps/rdram_{n}.txt').read()).group(1)
    out=subprocess.run([sys.executable,f'{root}/tools/gbi_dump.py',f'{root}/build/dumps/rdram_{n}.bin',dl,'--draws','--render',f'{S}/sp{n}.png','--render-scale','1'],capture_output=True,text=True).stdout
    field=set(); cols={}
    for blk in out.split('== draw ')[1:]:
        i=int(blk.split()[0]);
        if 'textdetail=DETAIL' in blk: field.add(i)
    for l in out.splitlines():
        m=re.match(r'\s+draw (\d+): (\d+) px\s+#(\w+)',l)
        if m: cols[tuple(int(m.group(3)[k:k+2],16) for k in (0,2,4))]=int(m.group(1))
    idimg=Image.new('RGB',(512,256)); idimg.paste(Image.open(f'{S}/sp{n}_ids.png').convert('RGB'),(0,0)); idimg=idimg.resize((W,H),Image.NEAREST)
    px=idimg.tobytes()
    mask=[cols.get((px[i],px[i+1],px[i+2]),-1) in field for i in range(0,len(px),3)]
    a=luma(load_ref(f'{R}/rdram_{n}_ref.png')); b=luma(Image.open(f'{R}/rdram_{n}_psp.png').convert('RGB'))
    c1,c2=(0.01*255)**2,(0.03*255)**2
    ia,ib,iaa,ibb,iab=integral(a,W,H),integral(b,W,H),integral([x*x for x in a],W,H),integral([x*x for x in b],W,H),integral([x*y for x,y in zip(a,b)],W,H)
    n7=49; cov=49/48; tot={True:[0,0],False:[0,0]}; W1=W+1
    for y in range(3,H-3):
        for x in range(3,W-3):
            y0,y1,x0,x1=(y-3)*W1,(y+4)*W1,x-3,x+4
            s=lambda t:t[y1+x1]-t[y0+x1]-t[y1+x0]+t[y0+x0]
            ma,mb=s(ia)/n7,s(ib)/n7; va=(s(iaa)/n7-ma*ma)*cov; vb=(s(ibb)/n7-mb*mb)*cov; vab=(s(iab)/n7-ma*mb)*cov
            v=((2*ma*mb+c1)*(2*vab+c2))/((ma*ma+mb*mb+c1)*(va+vb+c2))
            k=mask[y*W+x]; tot[k][0]+=v; tot[k][1]+=1
    print(f"rdram_{n}: field (detail-mode draws) {tot[True][1]*100//(tot[True][1]+tot[False][1])}% of pixels ssim={tot[True][0]/tot[True][1]:.4f}; everything else ssim={tot[False][0]/tot[False][1]:.4f}")
