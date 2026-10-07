"""Manual Labelary glyph census. Requires fontTools only for fixture creation."""
from pathlib import Path
from urllib.request import Request,urlopen
from urllib.error import HTTPError
from fontTools.ttLib import TTFont
import json,math,struct,time,unicodedata

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'tests/golden/font-glyphs'
OUT.mkdir(exist_ok=True)
font=TTFont(ROOT/'resources/fonts/font0.ttf')
characters=sorted(set(cp for cp in font.getBestCmap() if cp>=32 and unicodedata.category(chr(cp)) not in ('Cc','Cf')) | set(range(32,127)) | set(range(0x410,0x450)) | {0x401,0x451})
specials=sorted(set(map(ord,'№€₽£¥¢¤©®™±×÷°µ§¶•·–—…‰†‡≤≥≠≈√∞∑∏∆Ω→←↑↓↔↕✓✗¼½¾«»‹›“”‘’²³¹')))
# Confirmed by Labelary's X-Warnings headers and empty glyph cells. Preserve
# these cases as explicit missing-glyph diagnostics, never silently skip them.
unsupported={0x037E,0x2044,0xFB01,0xFB02,0x20BD,0x2206,0x220F,0x2211,0x221A,0x221E,0x2248,0x2260,0x2264,0x2265,0x2713,0x2717}
profiles=[(12,12),(16,16),(25,25),(30,30),(40,40),(26,33),(60,30),(70,100)]
previous=json.loads((OUT/'manifest.json').read_text()) if (OUT/'manifest.json').exists() else {}
prior_cases={case['name']:case for case in previous.get('cases',[])}
manifest={'source':'Labelary API','quality':'Bitonal','date':'2026-10-07','dpmm':8,'width':2436,'height':2436,
          'characters':[f'U+{cp:04X}' for cp in sorted(set(characters)|set(specials))],
          'unsupportedCharacters':[f'U+{cp:04X}' for cp in sorted(unsupported)],
          'unsupportedNote':'Labelary X-Warnings reports that the field font cannot display these characters; their atlas cells are empty. Tests require an explicit missing-glyph diagnostic.',
          'cases':[]}
for family,alphabet,height,width in [(family,alphabet,h,w) for family,alphabet in [('font0',characters),('font0-special',specials)] for h,w in profiles]:
    name=f'{family}-h{height}-w{width}'
    cell_width=max(64,width+30);cell_height=max(64,height+30)
    columns=min(24,2400//cell_width)
    fields=[];cells=[]
    for i,cp in enumerate(alphabet):
        x=20+(i%columns)*cell_width;y=20+(i//columns)*cell_height
        assert y+cell_height<2436
        char=chr(cp)
        value=f'_{cp:02X}' if char in '^~_' else char
        fields.append(f'^FT{x},{y+height}^A0N,{height},{width}^FH_^FD{value}^FS')
        cells.append({'codepoint':f'U+{cp:04X}','x':x,'y':y,'width':cell_width,'height':cell_height})
    zpl='^XA^PW2436^LL2436^CI28'+''.join(fields)+'^XZ'
    zp=OUT/(name+'.zpl');png=OUT/(name+'-labelary-bitonal.png')
    warnings=prior_cases.get(name,{}).get('oracleWarnings','')
    if not (png.exists() and zp.exists() and zp.read_text(encoding='utf-8').strip()==zpl):
        req=Request('https://api.labelary.com/v1/printers/8dpmm/labels/12x12/0/',data=zpl.encode(),headers={'Accept':'image/png','X-Quality':'Bitonal','X-Linter':'On','Content-Type':'application/x-www-form-urlencoded'})
        for attempt in range(5):
            try:
                with urlopen(req,timeout=90) as r:data=r.read();warnings=r.headers.get('X-Warnings','')
                break
            except HTTPError as error:
                if error.code!=429 or attempt==4:raise
                time.sleep(3*(attempt+1))
        assert data[:8]==b'\x89PNG\r\n\x1a\n' and struct.unpack('>IIBB',data[16:26])[:3]==(2436,2436,1)
        zp.write_text(zpl+'\n',encoding='utf-8');png.write_bytes(data)
        print(name,warnings,flush=True);time.sleep(1.3)
    manifest['cases'].append({'name':name,'font':'0','fontHeight':height,'fontWidth':width,
                             'unsupportedCharacters':[f'U+{cp:04X}' for cp in alphabet if cp in unsupported],
                             'oracleWarnings':warnings,'cells':cells})
(OUT/'manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
