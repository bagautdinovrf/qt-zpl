"""Opt-in original Labelary bitonal ruler acquisition and offline measurement."""
from pathlib import Path
from datetime import datetime, timezone
import argparse, hashlib, json, sys, time, urllib.request
from PIL import Image

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'tests/golden/font0-metrics'
sys.path.insert(0, str(ROOT / 'tools'))
from fetch_font_goldens import png_rows

def sha(data): return hashlib.sha256(data).hexdigest()
def save(path, value): path.write_text(json.dumps(value, ensure_ascii=False, indent=2)+'\n', encoding='utf8')

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--fetch', action='store_true')
    args=parser.parse_args()
    measured=json.loads((OUT/'measurements.json').read_text(encoding='utf8'))
    metrics=measured['glyphs']
    OUT.mkdir(exist_ok=True)
    manifest_path=OUT/'manifest.json'
    manifest=json.loads(manifest_path.read_text(encoding='utf8')) if manifest_path.exists() else {'schemaVersion':1,'cases':[]}
    old={c['name']:c for c in manifest['cases']}
    cases=[]
    for width,repeat,subset in ((300,8,None),(100,24,None),(200,12,['U+2030','U+2116'])):
        cps=list(metrics) if subset is None else subset
        for page,start in enumerate(range(0,len(cps),56),1):
            repeats=lambda cp: repeat if cp!='U+2030' else (10 if width==200 else repeat//8*7)
            cells=[{'text':'|','repeat':0}]+[{'text':chr(int(cp[2:],16))*repeats(cp)+'|',
                  'codepoint':cp,'repeat':repeats(cp)} for cp in cps[start:start+56]]
            name=f'advance-h30-w{width}-r{repeat}-page{page}'
            case={'name':name,'fontHeight':30,'fontWidth':width,'width':2999,'height':48*len(cells)+16,'cells':cells}
            zpl=f"^XA^CI28^PW{case['width']}^LL{case['height']}"
            for row,c in enumerate(cells):
                c.update(anchorX=30,anchorY=row*48+35,top=row*48,bottom=(row+1)*48)
                zpl+=f"^FT30,{c['anchorY']}^A0N,30,{width}^FH_^FD"+''.join(f'_{b:02X}' for b in c['text'].encode('utf8'))+'^FS'
            source=(zpl+'^XZ\n').encode('ascii')
            sp=OUT/f'{name}.zpl'; pp=OUT/f'{name}-labelary-bitonal.png'
            case['zplSha256']=sha(source)
            if sp.exists() and sp.read_bytes()!=source: raise ValueError(f'Cached source differs: {sp}')
            sp.write_bytes(source)
            if name in old:
                if old[name]['zplSha256']!=case['zplSha256']: raise ValueError('provenance source changed')
                case['response']=old[name]['response']
            if not pp.exists():
                if not args.fetch: raise ValueError(f'Missing {pp}; pass --fetch')
                url=f"https://api.labelary.com/v1/printers/8dpmm/labels/{(case['width']+.25)/203:.9f}x{(case['height']+.25)/203:.9f}/0/"
                headers={'Accept':'image/png','Content-Type':'application/x-www-form-urlencoded','X-Quality':'Bitonal','X-Linter':'On'}
                with urllib.request.urlopen(urllib.request.Request(url,data=source,headers=headers),timeout=45) as response:
                    data=response.read()
                    info={'url':url,'headers':headers,'retrievedUtc':datetime.now(timezone.utc).isoformat(),'responseDate':response.headers.get('Date',''),
                          'warnings':response.headers.get('X-Warnings',''),'pngSha256':sha(data)}
                png_rows(data,(case['width'],case['height']))
                pp.write_bytes(data); case['response']=info
                print(f'Fetched {name} ({len(data)} bytes)',flush=True)
                # Save provenance immediately so interrupted runs are reusable.
                partial=[c for c in manifest['cases'] if c['name']!=name]+[case]
                manifest['cases']=partial;save(manifest_path,manifest)
                time.sleep(1.3)
            data=pp.read_bytes(); png_rows(data,(case['width'],case['height']))
            if sha(data)!=case['response']['pngSha256']: raise ValueError('PNG provenance mismatch')
            cases.append(case)
    manifest.update(cases=cases,source='Labelary API original X-Quality: Bitonal responses',dpi=203,
                    method='The rightmost pipe is matched exactly to the standalone pipe; subtract its original bearing. Repeat spans 2400 nominal font-width units (2100 for per-mille, or 2000 in its width200 confirmation, to avoid clipping).')
    save(manifest_path,manifest)
    measurements={cp:dict(hmtxAdvance=m['hmtxAdvance'],unitsPerEm=m['unitsPerEm'],observations=[]) for cp,m in metrics.items()}
    for case in cases:
        image=Image.open(OUT/f"{case['name']}-labelary-bitonal.png").convert('L')
        inverse=image.point(lambda p:255-p)
        reference=case['cells'][0]
        ref=inverse.crop((0,reference['top'],case['width'],reference['bottom']))
        rb=ref.getbbox(); template=ref.crop(rb); bearing=rb[0]-reference['anchorX']
        for c in case['cells'][1:]:
            cell=inverse.crop((0,c['top'],case['width'],c['bottom']))
            b=cell.getbbox(); marker_left=b[2]-template.width
            actual=cell.crop((marker_left,rb[1],b[2],rb[3]))
            if actual.tobytes()!=template.tobytes(): raise ValueError(f"Marker mismatch {case['name']} {c['codepoint']}")
            # Everything at and after the marker must be the same complete shape.
            if cell.crop((marker_left,0,case['width'],48)).getbbox()!=(0,rb[1],template.width,rb[3]): raise ValueError('Ambiguous marker extent')
            distance=marker_left-c['anchorX']-bearing
            m=measurements[c['codepoint']]
            predicted=m['hmtxAdvance']/m['unitsPerEm']*c['repeat']*case['fontWidth']
            omitted=(c['codepoint'] in ('U+2030','U+2116') and case['fontWidth']==300 and distance==0)
            if not omitted and abs(distance-predicted)>.500001:
                raise ValueError(f"Advance mismatch {case['name']} {c['codepoint']}: {distance-predicted}")
            m['observations'].append({'case':case['name'],'fontWidth':case['fontWidth'],'repeat':c['repeat'],
                'markerDistanceDots':distance,'predictedExactHmtxDistanceDots':predicted,'residualDots':distance-predicted,
                'uncertaintyEm':1/(c['repeat']*case['fontWidth']),
                'status':'oracle-omits-glyph-at-width300' if omitted else 'validated'})
    save(OUT/'measurements.json',{'schemaVersion':1,'unitsPerEm':2048,'glyphs':measurements,
        'numericalMetadataSource':measured['numericalMetadataSource'],
        'numericalMetadataMethod':'Source character order paired with PDF Tj CID operands; CIDToGIDMap maps to numeric hmtx advance. Only advance integers retained; no font program or outlines redistributed.',
        'rasterMethod':manifest['method']})
    outliers=[(cp,[o['residualDots'] for o in m['observations']]) for cp,m in measurements.items() if any(abs(o['residualDots'])>1.01 for o in m['observations'])]
    if any(len({(o['fontWidth'],o['repeat']) for o in m['observations'] if o['status']=='validated'})<2 for m in measurements.values()):
        raise ValueError('Every exact advance must have two independent raster observations')
    print(json.dumps({'glyphs':len(measurements),'observations':sum(len(m['observations']) for m in measurements.values()),
        'maxAbsResidualDots':max(abs(o['residualDots']) for m in measurements.values() for o in m['observations']), 'outliers':outliers},indent=2),flush=True)

if __name__=='__main__': main()
