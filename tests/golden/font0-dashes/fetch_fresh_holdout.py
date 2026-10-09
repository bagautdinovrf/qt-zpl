"""Freeze and acquire a fresh ASCII-hyphen validation page after model selection.

No renderer is executed, and the original three plans/responses are untouched.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
import math
from pathlib import Path
from urllib.request import Request, urlopen

from fetch import HEADERS, OUT, png_rows, sha, validate

NAME = 'fresh-holdout'
MANIFEST = OUT / (NAME + '-manifest.json')


def rotate(x: float, y: float, orientation: str) -> tuple[float, float]:
    return {'N': (x, y), 'R': (-y, x), 'I': (-x, -y), 'B': (y, -x)}[orientation]


def scenario() -> tuple[dict, bytes]:
    cells = []
    patterns = ((0, 1, 0), (1, 2, 1), (2, 3, 2), (3, 2, 3),
                (3, 1, 2), (2, 2, 0), (1, 3, 3), (0, 2, 1))
    for profile, (height, width) in enumerate(((33, 71), (77, 19), (99, 53),
                                              (127, 31), (219, 83), (347, 41))):
        for local, (anchor, orientation) in enumerate((a, o) for a in ('FO', 'FT') for o in 'NRIB'):
            direction = 'H' if profile < 4 else ('R' if profile == 4 else 'V')
            leading, repeat, spacing = patterns[local] if direction == 'H' else (0, 1, local % 4)
            # Layout uses pre-existing advance/anchor metrics, never candidate
            # dash contours or raster quantization. A broad 1-em rectangle
            # around every visible glyph gives a conservative isolation bound.
            dash_advance = 1850 * width / 2048
            space_advance = 604 * width / 2048
            nominal_cell = math.floor(1548 * width / 2048 + .5)
            if direction == 'H':
                first = leading * (space_advance + spacing)
                last = first + (repeat - 1) * (dash_advance + spacing)
            elif direction == 'R':
                first = last = nominal_cell - dash_advance - spacing
            else:
                first = last = 0
            n = leading + repeat
            extent = leading * space_advance + repeat * dash_advance + n * spacing
            ascent = math.floor(.75 * height)
            baseline = (0, 0)
            if anchor == 'FO':
                span = math.floor(extent) if direction == 'H' else nominal_cell
                baseline = {'N': (0, ascent),
                            'R': (0 if direction == 'V' else height - ascent, 0),
                            'I': (span, height - ascent),
                            'B': (ascent, span)}[orientation]
            corners = [rotate(gx, gy, orientation)
                       for gx in (first - .05 * width, last + width)
                       for gy in (-height, 0)]
            left = min(p[0] for p in corners) + baseline[0]
            right = max(p[0] for p in corners) + baseline[0]
            top = min(p[1] for p in corners) + baseline[1]
            bottom = max(p[1] for p in corners) + baseline[1]
            if max(right - left, bottom - top) > 368:
                raise ValueError('Scenario does not fit its pre-request isolation cell')
            index = len(cells)
            x, y = (index % 7) * 400, (index // 7) * 400
            ox = math.floor(200 - (left + right) / 2)
            oy = math.floor(200 - (top + bottom) / 2)
            cells.append(dict(fieldIndex=index, fontHeight=height, fontWidth=width,
                              anchor=anchor, orientation=orientation, direction=direction,
                              leadingSpaces=leading, dashCount=repeat, spacing=spacing,
                              text=' ' * leading + '-' * repeat,
                              x=x, y=y, width=400, height=400,
                              anchorX=x + ox, anchorY=y + oy,
                              conservativeDesignBounds=[x + ox + left, y + oy + top,
                                                        right - left, bottom - top]))
    page = dict(name=NAME, width=2800, height=2800, dpi=203,
                role='fresh-post-selection-validation', cells=cells)
    fields = ['^XA^CI28^PW2800^LL2800']
    for cell in cells:
        data = ''.join(f'_{byte:02X}' for byte in cell['text'].encode('ascii'))
        fields.append(f"^{cell['anchor']}{cell['anchorX']},{cell['anchorY']}"
                      f"^A0{cell['orientation']},{cell['fontHeight']},{cell['fontWidth']}"
                      f"^FP{cell['direction']},{cell['spacing']}^FH_^FD{data}^FS")
    source = ('\n'.join(fields) + '\n^XZ\n').encode('ascii')
    page['zplSha256'] = sha(source)
    return page, source


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fetch', action='store_true', help='fetch only the missing original PNG')
    parser.add_argument('--check', action='store_true', help='require and check saved data offline')
    args = parser.parse_args()
    page, source = scenario()
    plan_sha = sha(json.dumps(page, sort_keys=True, ensure_ascii=True).encode('ascii'))
    old = json.loads(MANIFEST.read_text(encoding='utf8')) if MANIFEST.exists() else {}
    if old and old['frozenPlanSha256'] != plan_sha:
        raise ValueError('Fresh holdout scenarios changed after plan freeze')
    source_path = OUT / (NAME + '.zpl')
    if source_path.exists() and source_path.read_bytes() != source:
        raise ValueError('Fresh holdout source changed after plan freeze')
    original_files = [OUT / 'manifest.json', OUT / 'fetch.py']
    for name in ('small-matrix', 'large-and-phases', 'boundary-probe'):
        original_files.extend((OUT / (name + '.zpl'), OUT / (name + '-labelary-bitonal.png')))
    original_hashes = {p.name: sha(p.read_bytes()) for p in original_files}
    if old and old['unchangedPriorFixtureHashes'] != original_hashes:
        raise ValueError('An original three-page fixture changed')
    manifest = dict(schemaVersion=1, purpose='Fresh validation acquired after dash model selection; never used to select parameters',
                    oracle='Labelary original X-Quality: Bitonal PNG',
                    frozenPlanSha256=plan_sha,
                    planFrozenUtc=old.get('planFrozenUtc', datetime.now(timezone.utc).isoformat()),
                    visibleCharacters=['U+002D'], invisibleCharacters=['U+0020'],
                    cases=[page], unchangedPriorFixtureHashes=original_hashes,
                    responses=old.get('responses', {}), validation=old.get('validation', {}))
    source_path.write_bytes(source)
    MANIFEST.write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf8')
    png_path = OUT / (NAME + '-labelary-bitonal.png')
    if not png_path.exists() and args.fetch:
        url = f"https://api.labelary.com/v1/printers/8dpmm/labels/{(2800 + .25) / 203:.9f}x{(2800 + .25) / 203:.9f}/0/"
        with urlopen(Request(url, data=source, headers=HEADERS), timeout=45) as response:
            data = response.read()
            record = dict(url=url, requestHeaders=HEADERS,
                          retrievedUtc=datetime.now(timezone.utc).isoformat(),
                          responseDate=response.headers.get('Date', ''),
                          warnings=response.headers.get('X-Warnings', ''),
                          zplSha256=page['zplSha256'], pngSha256=sha(data), pngBytes=len(data))
        png_rows(data, (2800, 2800))
        png_path.write_bytes(data)
        manifest['responses'][NAME] = record
        MANIFEST.write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf8')
        manifest['validation'][NAME] = validate(page, data)
        MANIFEST.write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf8')
        print('Fetched fresh-holdout original 2800x2800 bitonal PNG')
    if png_path.exists():
        data = png_path.read_bytes()
        record = manifest['responses'][NAME]
        if record['zplSha256'] != page['zplSha256'] or record['pngSha256'] != sha(data):
            raise ValueError('Fresh response hash or source provenance changed')
        if validate(page, data) != manifest['validation'][NAME]:
            raise ValueError('Fresh field isolation measurements changed')
        print('Checked fresh-holdout: 48 isolated fields; original three pages unchanged')
    elif args.check:
        raise ValueError('Missing original fresh holdout PNG')
    else:
        print(f'Frozen fresh-holdout before request: {plan_sha}')


if __name__ == '__main__':
    main()
