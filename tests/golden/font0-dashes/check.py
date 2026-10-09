"""Validate both frozen hyphen fixture sets offline without writing any files."""
from __future__ import annotations

import sys

# Import the archived acquisition helpers without creating __pycache__ files.
sys.dont_write_bytecode = True

import json
from pathlib import Path

from fetch import HEADERS, scenarios, sha, validate
from fetch_fresh_holdout import scenario

DIRECTORY = Path(__file__).resolve().parent


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def read_manifest(name: str) -> dict:
    return json.loads((DIRECTORY / name).read_bytes())


def plan_hash(plan: object) -> str:
    return sha(json.dumps(plan, sort_keys=True, ensure_ascii=True).encode('ascii'))


def check_page(page: dict, source: bytes, manifest: dict) -> None:
    name = page['name']
    require((DIRECTORY / (name + '.zpl')).read_bytes() == source,
            f'{name}: original ZPL differs from the frozen scenario')
    require(sha(source) == page['zplSha256'], f'{name}: frozen ZPL hash differs')
    response = manifest['responses'][name]
    require(response['zplSha256'] == page['zplSha256'],
            f'{name}: response belongs to a different ZPL source')
    require(response['requestHeaders'] == HEADERS,
            f'{name}: recorded bitonal request headers differ')
    data = (DIRECTORY / (name + '-labelary-bitonal.png')).read_bytes()
    require(sha(data) == response['pngSha256'] and len(data) == response['pngBytes'],
            f'{name}: original PNG bytes differ from the recorded response')
    # validate also checks PNG dimensions, 1-bit grayscale, chunk CRCs, ink
    # isolation and clearance. No image conversion or network request occurs.
    require(validate(page, data) == manifest['validation'][name],
            f'{name}: recorded field isolation measurements differ')
    print(f'Checked {name}: {len(page["cells"])} isolated fields')


def main() -> None:
    original = read_manifest('manifest.json')
    pages = scenarios()
    plan = [{key: value for key, value in page.items() if key != 'source'}
            for page in pages]
    require(original['cases'] == plan, 'Original scenarios differ from their frozen plan')
    require(original['frozenPlanSha256'] == plan_hash(plan),
            'Original frozen scenario hash differs')
    for page in pages:
        check_page(page, page['source'], original)

    fresh = read_manifest('fresh-holdout-manifest.json')
    page, source = scenario()
    require(fresh['cases'] == [page], 'Fresh holdout differs from its frozen plan')
    require(fresh['frozenPlanSha256'] == plan_hash(page),
            'Fresh holdout frozen scenario hash differs')
    original_names = ['manifest.json', 'fetch.py']
    for old_page in pages:
        original_names.extend((old_page['name'] + '.zpl',
                               old_page['name'] + '-labelary-bitonal.png'))
    original_hashes = {name: sha((DIRECTORY / name).read_bytes())
                       for name in original_names}
    require(fresh['unchangedPriorFixtureHashes'] == original_hashes,
            'Original acquisition files changed after the fresh holdout was frozen')
    check_page(page, source, fresh)
    print('Both frozen manifests and all 224 fields verified; no files written')


if __name__ == '__main__':
    main()
