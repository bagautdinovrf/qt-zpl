"""Manually acquire/check the independent Font 0 layout holdout; network is opt-in."""
from pathlib import Path
import argparse,json,sys,hashlib,urllib.request
from datetime import datetime,timezone
from PIL import Image
ROOT=Path(__file__).resolve().parents[3]
OUT=ROOT/'tests/golden/font0-metrics'
sys.path.insert(0,str(ROOT/'tools'))
from fetch_font_goldens import png_rows
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--fetch',action='store_true',help='fetch the original response only when it is absent')
args=parser.parse_args()
cases=[]
for anchor,orientations in (('FT','NRIB'),('FO','N')):
    for orientation in orientations:
        for direction in 'HVR':
            for justification in (0,1):
                cases.append(dict(anchor=anchor,orientation=orientation,direction=direction,justification=justification))
source='^XA^CI28^PW2400^LL2800'
for index,case in enumerate(cases):
    col,row=index%3,index//3
    case.update(cell=[col*800,row*280,800,280],anchorX=col*800+400,anchorY=row*280+140,
                text=['Milk','Тест','Ёжик','Балтк'][index%4],fontHeight=26,fontWidth=17,spacing=3)
    text=''.join(f'_{b:02X}' for b in case['text'].encode('utf8'))
    field=f"^{case['anchor']}{case['anchorX']},{case['anchorY']},{case['justification']}^A0{case['orientation']},26,17^FP{case['direction']},3^FH_^FD{text}^FS"
    source+=field+f'^FT,{row*280+20}^GB3,11,3^FS'
    source+=field+f'^FT{col*800+20},^GB3,11,3^FS'
source=(source+'^XZ\n').encode('ascii')
sp=OUT/'layout-holdout.zpl';pp=OUT/'layout-holdout-labelary-bitonal.png'
if sp.exists() and sp.read_bytes()!=source:raise ValueError('Source changed')
sp.write_bytes(source)
if not pp.exists():
    if not args.fetch: raise ValueError('Missing original response; pass --fetch for manual acquisition')
    url=f'https://api.labelary.com/v1/printers/8dpmm/labels/{2400.25/203:.9f}x{2800.25/203:.9f}/0/'
    headers={'Accept':'image/png','Content-Type':'application/x-www-form-urlencoded','X-Quality':'Bitonal','X-Linter':'On'}
    with urllib.request.urlopen(urllib.request.Request(url,data=source,headers=headers),timeout=45) as response:
        data=response.read()
        provenance=dict(url=url,headers=headers,retrievedUtc=datetime.now(timezone.utc).isoformat(),responseDate=response.headers.get('Date',''),warnings=response.headers.get('X-Warnings',''),pngSha256=hashlib.sha256(data).hexdigest(),zplSha256=hashlib.sha256(source).hexdigest())
    png_rows(data,(2400,2800));pp.write_bytes(data)
    (OUT/'layout-holdout.json').write_text(json.dumps(dict(cases=cases,response=provenance),ensure_ascii=False,indent=2)+'\n',encoding='utf8')
saved=json.loads((OUT/'layout-holdout.json').read_text(encoding='utf8'))
assert saved['cases']==cases
assert saved['response']['zplSha256']==hashlib.sha256(source).hexdigest()
assert saved['response']['pngSha256']==hashlib.sha256(pp.read_bytes()).hexdigest()
png_rows(pp.read_bytes(),(2400,2800))
image=Image.open(pp).convert('L')
for index,case in enumerate(cases):
    x,y,w,h=case['cell']; crop=image.crop((x,y,x+w,y+h));ink={(a,b)for b in range(h)for a in range(w)if crop.getpixel((a,b))<128}
    comps=[]
    while ink:
        start=ink.pop();stack=[start];component={start}
        while stack:
            a,b=stack.pop()
            for p in ((a-1,b),(a+1,b),(a,b-1),(a,b+1)):
                if p in ink:ink.remove(p);component.add(p);stack.append(p)
        if len(component)==33:
            bounds=(min(a for a,b in component),min(b for a,b in component),max(a for a,b in component)+1,max(b for a,b in component)+1)
            if bounds[2]-bounds[0]==3 and bounds[3]-bounds[1]==11: comps.append(bounds)
    assert len(comps)==2, (index,case,comps)
print(f'Checked {len(cases)} cases and {len(cases)*2} isolated original graphic markers')
