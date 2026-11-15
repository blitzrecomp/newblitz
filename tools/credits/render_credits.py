#!/usr/bin/env python3
"""The credits page (512x384, shown once per run after the Players Inc screen): every word is an editable string
below. Writes art/credits/credits.png and credits_text.json; tools/credits/make_credits.py then makes the game image.

  .venv/bin/python tools/credits/render_credits.py && .venv/bin/python tools/credits/make_credits.py

Needs Pillow and macOS's Arial Black and Arial Narrow Bold (/System/Library/Fonts/Supplemental).
"""
# BRANDING: the credits page wording (project and app names, the credit line, the notice)
from pathlib import Path
from PIL import Image,ImageDraw,ImageFont,ImageFilter
import json,textwrap,re,hashlib
HERE=Path(__file__).resolve().parents[2]/'art'/'credits'
FONT_DIR=Path('/System/Library/Fonts/Supplemental')
TITLE='NEW BLITZ 2027'
SUBTITLE='A BLITZ RECOMP PROJECT. NFL BLITZ 2000, REBUILT TO RUN NATIVELY'
CREDITS=[
'NFL BLITZ 2000 - MIDWAY AND ITS ORIGINAL DEVELOPERS',
'N64RECOMP - WISEGUY',
'RT64 - DARIO AND THE RT64 CONTRIBUTORS',
'N64 MODERN RUNTIME AND RECOMP FRONTEND - THE ZELDA 64: RECOMPILED TEAM',
'TEAM EXTENSION RESEARCH - ZOINKITY',
'THE NFL BLITZ COMMUNITY - NFLBLITZFANS.COM AND NFL BLITZ RESURRECTION',
'ARMIPS - KINGCOM   LLVM   SDL   RMLUI',
'BUILT BY THE BLITZ RECOMP TEAM']
NOTICE='NEW BLITZ IS AN UNOFFICIAL, NON-COMMERCIAL PRESERVATION PROJECT. IT IS NOT\nAFFILIATED WITH, ENDORSED OR SPONSORED BY THE NFL, THE NFL PLAYERS ASSOCIATION,\nNFL PLAYERS INC., ANY NFL CLUB, OR MIDWAY. ALL NAMES, LOGOS AND TRADEMARKS\nBELONG TO THEIR OWNERS. NO GAME ASSETS ARE INCLUDED: IT NEEDS YOUR OWN COPY\nOF THE GAME. NOT FOR SALE.'

def font(name,size):return ImageFont.truetype(str(FONT_DIR/name),size)
def mask(text,f):
 box=f.getbbox(text);m=Image.new('L',(int(f.getlength(text))+8,box[3]-box[1]+6));ImageDraw.Draw(m).text((3,3-box[1]),text,font=f,fill=255);return m.crop(m.getbbox())
def heading(out,text,y,size):
 m=mask(text,font('Arial Black.ttf',size));extra=round(m.height*.20)
 m=m.transform((m.width+extra,m.height),Image.Transform.AFFINE,(1,.20,-extra,0,1,0),Image.Resampling.BICUBIC)
 # Thin black keyline and lower black shadow at native resolution.
 x=(512-m.width)//2;edge=m.filter(ImageFilter.MaxFilter(3));out.paste((0,0,0),(x+1,y+2),edge);out.paste((0,0,0),(x,y),edge);out.paste((255,237,0),(x,y),m)
 return m.height

def center_text(out,text,y,f,color=(248,248,250),maxwidth=480):
 m=mask(text,f);assert m.width<=maxwidth,(text,m.width,maxwidth)
 x=(512-m.width)//2;out.paste((0,0,0),(x+1,y+1),m);out.paste(color,(x,y),m)
 return (x,y,x+m.width,y+m.height)

def render():
 out=Image.new('RGB',(512,384));d=ImageDraw.Draw(out)
 # Original purple/steel art, made as editable geometric layers; no licensing screen pixels.
 for y in range(384):
  c=(15+int(9*(1-y/384)),12+int(6*(1-y/384)),28+int(22*(1-y/384)));d.line((0,y,511,y),fill=c)
 d.polygon([(0,0),(170,0),(16,175),(0,175)],fill=(35,30,55));d.polygon([(512,160),(512,384),(370,384)],fill=(42,43,59))
 for x in range(-100,650,48):d.line((x,384,x+250,0),fill=(30,25,47))
 # Framed glass title bar, with clipped metal corners and purple edge glow.
 d.polygon([(10,8),(502,8),(502,68),(10,68)],fill=(10,16,32),outline=(96,78,145))
 d.polygon([(10,8),(38,8),(28,16),(17,16),(17,34),(10,40)],fill=(184,191,207))
 d.polygon([(502,8),(474,8),(484,16),(495,16),(495,34),(502,40)],fill=(184,191,207))
 d.line((18,68,494,68),fill=(114,57,177),width=2)
 heading(out,TITLE,18,31)
 center_text(out,SUBTITLE,54,font('Arial Narrow Bold.ttf',13))
 heading(out,'MADE POSSIBLE BY',79,16)
 body=font('Arial Narrow Bold.ttf',13)
 bounds=[]
 for i,line in enumerate(CREDITS):bounds.append(center_text(out,line,106+i*14,body))
 d.line((20,226,492,226),fill=(105,91,139))
 heading(out,'NOTICE',236,17)
 nf=font('Arial Narrow Bold.ttf',13)
 # Wrap only at spaces; line breaks may change, words and punctuation may not.
 words=NOTICE.split();lines=[];line=''
 for word in words:
  trial=(line+' '+word).strip()
  if mask(trial,nf).width>476:
   assert line;lines.append(line);line=word
  else:line=trial
 if line:lines.append(line)
 assert ' '.join(lines)==' '.join(NOTICE.split())
 assert len(lines)<=8
 notice_bounds=[]
 for i,line in enumerate(lines):notice_bounds.append(center_text(out,line,264+i*14,nf))
 assert max(b[3] for b in notice_bounds)<376
 out.convert('RGBA').save(HERE/'credits.png')
 (HERE/'credits_text.json').write_text(json.dumps({'title':TITLE,'subtitle':SUBTITLE,'credit_heading':'MADE POSSIBLE BY','credits':CREDITS,'notice_heading':'NOTICE','notice_verbatim':NOTICE,'notice_render_lines':lines,'body_bounds':bounds,'notice_bounds':notice_bounds,'notice_words_unchanged':True,'size':[512,384],'opaque':True},indent=2)+'\n')
 return out
if __name__=='__main__':render()
