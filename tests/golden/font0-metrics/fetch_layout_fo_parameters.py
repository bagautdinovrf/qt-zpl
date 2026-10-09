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
for height,width,spacing,text in ((14,31,0,'A'),(37,13,1,'Test12'),(19,47,5,'Мир')):
    for direction,orientations in (('R','NRIB'),('V','RI')):
        for orientation in orientations:
            for justification in (0,1):
                cases.append(dict(anchor='FO',orientation=orientation,direction=direction,justification=justification,
                                  fontHeight=height,fontWidth=width,spacing=spacing,text=text))
for direction in 'HVR':
    for justification in (0,1):
        cases.append(dict(anchor='FT',orientation='N',direction=direction,justification=justification,
                          fontHeight=26,fontWidth=17,spacing=3,text=''))
source='^XA^CI28^PW2400^LL2970'
for index,case in enumerate(cases):
    col,row=index%4,index//4
    case.update(cell=[col*600,row*270,600,270],anchorX=col*600+300,
                anchorY=row*270+(80 if not case['text'] else (40 if case['direction']=='V' else 120)))
    text=''.join(f'_{b:02X}' for b in case['text'].encode('utf8'))
    field=f"^{case['anchor']}{case['anchorX']},{case['anchorY']},{case['justification']}^A0{case['orientation']},{case['fontHeight']},{case['fontWidth']}^FP{case['direction']},{case['spacing']}^FH_^FD{text}^FS"
    source+=field+f'^FT,{row*270+20}^GB3,11,3^FS'
    source+=field+f'^FT{col*600+20},^GB3,11,3^FS'
source=(source+'^XZ\n').encode('ascii')
sp=OUT/'layout-fo-parameter-holdout.zpl';pp=OUT/'layout-fo-parameter-holdout-labelary-bitonal.png'
if sp.exists() and sp.read_bytes()!=source:raise ValueError('Source changed')
sp.write_bytes(source)
if not pp.exists():
    if not args.fetch: raise ValueError('Missing original response; pass --fetch for manual acquisition')
    url=f'https://api.labelary.com/v1/printers/8dpmm/labels/{2400.25/203:.9f}x{2970.25/203:.9f}/0/'
    headers={'Accept':'image/png','Content-Type':'application/x-www-form-urlencoded','X-Quality':'Bitonal','X-Linter':'On'}
    with urllib.request.urlopen(urllib.request.Request(url,data=source,headers=headers),timeout=45) as response:
        data=response.read()
        provenance=dict(url=url,headers=headers,retrievedUtc=datetime.now(timezone.utc).isoformat(),responseDate=response.headers.get('Date',''),warnings=response.headers.get('X-Warnings',''),pngSha256=hashlib.sha256(data).hexdigest(),zplSha256=hashlib.sha256(source).hexdigest())
    png_rows(data,(2400,2970));pp.write_bytes(data)
    (OUT/'layout-fo-parameter-holdout.json').write_text(json.dumps(dict(cases=cases,response=provenance),ensure_ascii=False,indent=2)+'\n',encoding='utf8')
saved=json.loads((OUT/'layout-fo-parameter-holdout.json').read_text(encoding='utf8'))
for previous,current in zip(saved['cases'],cases):
    previous.pop('probeRegions',None);current.pop('probeRegions',None)
    previous.pop('markerRects',None);current.pop('markerRects',None)
assert saved['cases']==cases
assert saved['response']['zplSha256']==hashlib.sha256(source).hexdigest()
assert saved['response']['pngSha256']==hashlib.sha256(pp.read_bytes()).hexdigest()
png_rows(pp.read_bytes(),(2400,2970))
image=Image.open(pp).convert('L')
seen=set()
for index,case in enumerate(cases):
    x,y,w,h=case['cell'];px=x+20
    runs=[];start=None
    for row in range(image.height+1):
        black=row<image.height and all(image.getpixel((px+k,row))<128 for k in range(3))
        if black and start is None:start=row
        if not black and start is not None:
            if row-start==11:runs.append((start,row))
            start=None
    target=case['anchorY']
    if case['direction']=='V' and case['orientation']=='I':target+=(len(case['text'])-1)*case['fontHeight']
    marker=min(runs,key=lambda r:abs(r[1]-target))
    assert (px,marker[0]) not in seen,(index,marker)
    seen.add((px,marker[0]))
    top_candidates=[];ty=y+9
    for tx in range(x+40,x+w-3):
        if not all(image.getpixel((tx+k,ty+j))<128 for j in range(11) for k in range(3)):continue
        perimeter=[(tx-1,ty+j) for j in range(11)]+[(tx+3,ty+j) for j in range(11)]
        perimeter += [(tx+k,ty-1) for k in range(3)]+[(tx+k,ty+11) for k in range(3)]
        if all(image.getpixel(p)>=128 for p in perimeter):top_candidates.append(tx)
    assert len(top_candidates)==1,(index,top_candidates)
    case['markerRects']=[[top_candidates[0],y+9,3,11],[px,marker[0],3,11]]
    for x,y,w,h in case['markerRects']:
        region=image.crop((x,y,x+w,y+h)).point(lambda p:255-p)
        box=region.getbbox()
        assert box and (box[2]-box[0],box[3]-box[1])==(3,11),(index,case,box)
print(f'Checked {len(cases)} cases and {len(cases)*2} isolated original graphic markers')
saved['cases']=cases
(OUT/'layout-fo-parameter-holdout.json').write_text(json.dumps(saved,ensure_ascii=False,indent=2)+'\n',encoding='utf8')
