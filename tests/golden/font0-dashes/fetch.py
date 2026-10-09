"""Independent Font 0 ASCII-hyphen holdouts; original bitonal PNGs, opt-in network.

Scenario dimensions, origins, phases and spacing are fixed before acquisition.
Only U+002D and U+0020 appear in field data. Normal C++ tests use saved resources.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import sys
import time
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parents[3]
OUT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / 'tools'))
from fetch_font_goldens import ink_points, png_rows

HEADERS = {'Accept': 'image/png', 'Content-Type': 'application/x-www-form-urlencoded',
           'X-Quality': 'Bitonal', 'X-Linter': 'On'}


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def scenarios() -> list[dict]:
    small = []
    # Each profile independently covers FO/FT and all four orientations.
    patterns = ((0,1,0), (1,1,0), (2,2,1), (3,3,2),
                (2,2,0), (3,3,1), (0,1,0), (1,1,3))
    profiles = ((13,17), (17,13), (23,29), (29,23),
                (37,61), (61,37), (25,25), (25,37))
    for height, width in profiles:
        for index, (anchor, orientation) in enumerate((a,o) for a in ('FO','FT') for o in 'NRIB'):
            leading, repeat, spacing = patterns[index]
            small.append(dict(fontHeight=height, fontWidth=width, anchor=anchor,
                              orientation=orientation, leadingSpaces=leading,
                              dashCount=repeat, spacing=spacing))

    large = []
    for height, width in ((113,113), (257,257), (113,257), (257,113)):
        for index, (anchor, orientation) in enumerate((a,o) for a in ('FO','FT') for o in 'NRIB'):
            # Large profiles have only one visible dash and at most one space.
            large.append(dict(fontHeight=height, fontWidth=width, anchor=anchor,
                              orientation=orientation, leadingSpaces=(0,1,0,1,1,0,1,0)[index],
                              dashCount=1, spacing=(0,0,1,3,0,1,2,0)[index]))
    for leading in range(4):
        for anchor in ('FO','FT'):
            for repeat, spacing in ((1,0), (3,2)):
                large.append(dict(fontHeight=25, fontWidth=25, anchor=anchor, orientation='N',
                                  leadingSpaces=leading, dashCount=repeat, spacing=spacing))

    boundary = []
    for height,width in ((25,257),(257,25),(101,25),(102,25),(25,101),(25,102),
                         (101,101),(102,102),(100,100),(103,103)):
        for anchor in ('FO','FT'):
            for leading in (0,1):
                boundary.append(dict(fontHeight=height,fontWidth=width,anchor=anchor,orientation='N',
                                     leadingSpaces=leading,dashCount=1,spacing=0))
    for height,width in ((102,25),(25,102)):
        for anchor in ('FO','FT'):
            for orientation in 'RIB':
                for leading in (0,1):
                    boundary.append(dict(fontHeight=height,fontWidth=width,anchor=anchor,orientation=orientation,
                                         leadingSpaces=leading,dashCount=1,spacing=0))
    pages = []
    for name, cells, size, columns, rows in (
        ('small-matrix', small, 320, 8, 8),
        ('large-and-phases', large, 400, 7, 7),
        ('boundary-probe', boundary, 350, 8, 8),
    ):
        page = dict(name=name, width=size*columns, height=size*rows, dpi=203, cells=cells,
                    role='quantization-boundary-research' if name=='boundary-probe' else 'independent-validation')
        fields = [f"^XA^CI28^PW{page['width']}^LL{page['height']}"]
        for index, cell in enumerate(cells):
            x, y = (index % columns)*size, (index // columns)*size
            offsets={'N':(16,size//2),'R':(size//2,16),'I':(size-16,size//2),'B':(size//2,size-16)}
            ox,oy=(16,16) if cell['anchor']=='FO' else offsets[cell['orientation']]
            cell.update(x=x, y=y, width=size, height=size, anchorX=x+ox,
                        anchorY=y+oy, fieldIndex=index)
            cell['text'] = ' '*cell['leadingSpaces'] + '-'*cell['dashCount']
            # Hex escaping makes leading spaces and U+002D explicit byte data.
            data = ''.join(f'_{byte:02X}' for byte in cell['text'].encode('ascii'))
            fields.append(f"^{cell['anchor']}{cell['anchorX']},{cell['anchorY']}"
                          f"^A0{cell['orientation']},{cell['fontHeight']},{cell['fontWidth']}"
                          f"^FPH,{cell['spacing']}^FH_^FD{data}^FS")
        source = ('\n'.join(fields) + '\n^XZ\n').encode('ascii')
        page['source'] = source
        page['zplSha256'] = sha(source)
        pages.append(page)
    return pages


def validate(page: dict, data: bytes) -> dict:
    rows = png_rows(data, (page['width'], page['height']))
    checks = []
    for cell in page['cells']:
        points = ink_points(rows, cell)
        if not points:
            raise ValueError(f"{page['name']} field {cell['fieldIndex']}: blank dash")
        left, right = min(x for x,y in points), max(x for x,y in points)
        top, bottom = min(y for x,y in points), max(y for x,y in points)
        clearance = min(left-cell['x'], top-cell['y'], cell['x']+cell['width']-1-right,
                        cell['y']+cell['height']-1-bottom)
        if clearance < 4:
            raise ValueError(f"{page['name']} field {cell['fieldIndex']}: unsafe cell clearance {clearance}")
        checks.append(dict(fieldIndex=cell['fieldIndex'], inkPixels=len(points),
                           inkBounds=[left,top,right-left+1,bottom-top+1], clearanceDots=clearance))
    # All page widths are divisible by eight, so there are no padding bits.
    total = sum(8-byte.bit_count() for row in rows for byte in row)
    if sum(check['inkPixels'] for check in checks) != total:
        raise ValueError('Unexpected ink outside assigned field cells')
    return dict(totalInkPixels=total, cells=checks,
                minimumCellClearanceDots=min(check['clearanceDots'] for check in checks))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fetch', action='store_true', help='acquire missing original PNGs; never overwrite cached responses')
    parser.add_argument('--check', action='store_true', help='require and validate all original PNGs offline')
    parser.add_argument('--accept-layout-update', action='store_true', help='record the initial anchor-only isolation repair and separately requested boundary research page')
    args = parser.parse_args()
    path = OUT / 'manifest.json'
    saved = json.loads(path.read_text(encoding='utf8')) if path.exists() else {}
    pages = scenarios()
    plan = [{k:v for k,v in p.items() if k!='source'} for p in pages]
    plan_sha = sha(json.dumps(plan, sort_keys=True, ensure_ascii=True).encode('ascii'))
    history=saved.get('layoutHistory',[])
    if saved and saved['frozenPlanSha256'] != plan_sha:
        if not args.accept_layout_update:raise ValueError('Scenarios differ from the frozen independent plan')
        if saved.get('responses'):raise ValueError('Cannot relocate a plan with saved original responses; create a new fixture instead')
        def parameters(p):
            return {k:([{a:b for a,b in c.items() if a not in ('anchorX','anchorY')} for c in v] if k=='cells' else v)
                    for k,v in p.items() if k not in ('zplSha256','role')}
        if [parameters(p) for p in saved['cases']] != [parameters(p) for p in plan[:len(saved['cases'])]]:
            raise ValueError('Independent scenarios changed beyond integer anchors')
        history.append(dict(previousPlanSha256=saved['frozenPlanSha256'],updatedUtc=datetime.now(timezone.utc).isoformat(),
                            reason='The first isolation check found centered fields crossing cell boundaries. Only integer anchors were relocated; h/w, phases, text and spacing of both validation pages are unchanged. A separately requested boundary-research page was appended.',
                            previousZplHashes={p['name']:p['zplSha256'] for p in saved['cases']}))
    manifest = dict(schemaVersion=1, purpose='Two independent ASCII U+002D validation pages plus separately requested quantization-boundary research',
                    oracle='Labelary original X-Quality: Bitonal PNG', dpi=203,
                    frozenPlanSha256=plan_sha,
                    planFrozenUtc=saved.get('planFrozenUtc',datetime.now(timezone.utc).isoformat()),
                    visibleCharacters=['U+002D'], invisibleCharacters=['U+0020'], cases=plan,layoutHistory=history,
                    responses=saved.get('responses',{}), validation=saved.get('validation',{}))
    for page in pages:
        source_path = OUT / (page['name']+'.zpl')
        if source_path.exists() and source_path.read_bytes()!=page['source'] and not args.accept_layout_update:
            raise ValueError('Cached source differs from the frozen plan')
        source_path.write_bytes(page['source'])
    # Freeze the source and scenario hashes before any network request.
    path.write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n',encoding='utf8')
    for page in pages:
        png_path = OUT / (page['name']+'-labelary-bitonal.png')
        if not png_path.exists() and args.fetch:
            url = (f"https://api.labelary.com/v1/printers/8dpmm/labels/"
                   f"{(page['width']+.25)/203:.9f}x{(page['height']+.25)/203:.9f}/0/")
            with urlopen(Request(url,data=page['source'],headers=HEADERS),timeout=45) as response:
                data = response.read()
                record = dict(url=url, requestHeaders=HEADERS, retrievedUtc=datetime.now(timezone.utc).isoformat(),
                              responseDate=response.headers.get('Date',''), warnings=response.headers.get('X-Warnings',''),
                              zplSha256=page['zplSha256'], pngSha256=sha(data), pngBytes=len(data))
            png_rows(data,(page['width'],page['height']))
            png_path.write_bytes(data)
            manifest['responses'][page['name']] = record
            path.write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n',encoding='utf8')
            manifest['validation'][page['name']] = validate(page,data)
            path.write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n',encoding='utf8')
            print(f"Fetched {page['name']}: original {page['width']}x{page['height']} bitonal PNG",flush=True)
            time.sleep(1.3)
        if png_path.exists():
            data = png_path.read_bytes()
            if manifest['responses'][page['name']]['zplSha256']!=page['zplSha256']:
                raise ValueError('Original response belongs to a different ZPL source')
            if sha(data)!=manifest['responses'][page['name']]['pngSha256']:
                raise ValueError('Original PNG hash differs from provenance')
            if validate(page,data)!=manifest['validation'][page['name']]:
                raise ValueError('Saved field isolation measurements differ')
            print(f"Checked {page['name']}: {len(page['cells'])} isolated fields",flush=True)
        elif args.check:
            raise ValueError(f'Missing original PNG: {png_path}')
        else:
            print(f"Planned {page['name']}: {len(page['cells'])} fields; use --fetch",flush=True)


if __name__=='__main__':
    main()
