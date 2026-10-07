"""Build an offline HTML inspector and PNG contact sheet from native Font 0 probes.

Requires Pillow. No network, rendering service, installed comparison font, or
JavaScript dependency is used. Input rasters remain unchanged. Run the native
probe without --no-atlas-images before invoking this tool.
"""
from __future__ import annotations

import argparse
from collections import defaultdict
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import unicodedata
from urllib.parse import quote, unquote

from PIL import Image, ImageChops, ImageDraw, ImageFont


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def ink(image):
    return image.convert("L").point(lambda value: 255 if value < 128 else 0)


def overlay(expected, actual):
    oracle_ink, actual_ink = ink(expected), ink(actual)
    diff = ImageChops.difference(oracle_ink, actual_ink)
    output = Image.new("RGB", expected.size, "white")
    output.paste((28, 33, 42), mask=ImageChops.multiply(oracle_ink, actual_ink))
    output.paste((217, 48, 75), mask=ImageChops.subtract(actual_ink, oracle_ink))
    output.paste((37, 106, 221), mask=ImageChops.subtract(oracle_ink, actual_ink))
    return output, diff


def empty_statistics():
    return dict(cases=0, comparisons=0, exact=0, missing=0, differentPixels=0, intersectionPixels=0, unionPixels=0)


def statistics(report, profiles, unsupported):
    result = defaultdict(empty_statistics)
    for case in report["cases"]:
        name = case["case"]
        if name not in profiles:
            raise ValueError(f"Report references a case absent from manifest: {name}")
        role = profiles[name].get("role", "train")
        item = result[role]
        item["cases"] += 1
        for glyph in case["glyphs"]:
            if glyph.get("oracleUnsupported", glyph["codepoint"] in unsupported):
                continue
            item["comparisons"] += 1
            item["exact"] += glyph["differentPixels"] == 0 and not glyph.get("missingGlyph", False)
            item["missing"] += bool(glyph.get("missingGlyph", False))
            for key in ("differentPixels", "intersectionPixels", "unionPixels"):
                item[key] += int(glyph.get(key, 0))
    for item in result.values():
        item["iou"] = item["intersectionPixels"] / item["unionPixels"] if item["unionPixels"] else 1.0
    return dict(result)


def ui_font(size, bold=False):
    candidates = [Path(os.environ.get("WINDIR", "C:/Windows")) / "Fonts" / ("segoeuib.ttf" if bold else "segoeui.ttf"),
                  Path("/usr/share/fonts/truetype/dejavu") / ("DejaVuSans-Bold.ttf" if bold else "DejaVuSans.ttf")]
    for path in candidates:
        if path.exists():
            return ImageFont.truetype(str(path), size)
    try:
        return ImageFont.truetype("DejaVuSans.ttf", size)
    except OSError:
        return ImageFont.load_default(size=size)


def contact_sheet(data, output, assets):
    highlights = list(dict.fromkeys("AAabeg012ЖДФЫёйµ¢№!?%&()-+"))
    selected = []
    for height in (47, 96):
        group = next((g for g in data["groups"] if g["height"] == height and g["width"] == height
                      and g["orientation"] == "N" and g["anchor"] == "FT" and g["role"] == "validation"), None)
        if group:
            selected.append(group)
    if not selected:
        selected = data["groups"][:2]
    panels = [(g, g["glyphs"][f"U+{ord(char):04X}"]) for g in selected for char in highlights
              if f"U+{ord(char):04X}" in g["glyphs"]]
    columns, panel_width, panel_height, header = 4, 480, 206, 148
    rows = max(1, math.ceil(len(panels) / columns))
    canvas = Image.new("RGB", (columns * panel_width, header + rows * panel_height + 30), "#e9edf2")
    draw = ImageDraw.Draw(canvas)
    title, text, small = ui_font(31, True), ui_font(18), ui_font(14)
    draw.text((24, 18), "Font 0 · Labelary / native candidate", font=title, fill="#182236")
    draw.text((24, 62), "Чёрный: совпало · Красный: лишнее у кандидата · Синий: отсутствует у кандидата", font=text, fill="#344058")
    draw.text((24, 94), "Фрагменты выровнены по исходным координатам; масштаб nearest-neighbor. Полный набор — в HTML.", font=text, fill="#344058")
    atlas_cache, cached_case = {}, None
    for index, (group, glyph) in enumerate(panels):
        x, y = index % columns * panel_width + 8, header + index // columns * panel_height
        draw.rounded_rectangle((x, y, x + panel_width - 16, y + panel_height - 10), radius=10, fill="white")
        label = f'{chr(int(glyph["codepoint"][2:],16))}  {glyph["codepoint"]} · {group["height"]}×{group["width"]} {group["anchor"]}/{group["orientation"]}'
        draw.text((x + 12, y + 8), label, font=text, fill="#182236")
        source = data["atlases"][glyph["case"]]
        if cached_case != glyph["case"]:
            atlas_cache.clear()
            cached_case = glyph["case"]
        modes = [("expected", "Labelary")]
        if "before" in source and "before" in glyph:
            modes += [("before", "До"), ("beforeDiff", "Δ до")]
        modes += [("actual", "После" if data["hasBefore"] else "Кандидат"), ("diff", "Δ после" if data["hasBefore"] else "Разница")]
        region = glyph["area"]
        box = (region[0], region[1], region[0] + region[2], region[1] + region[3])
        crops = []
        for mode, label in modes:
            relative = source[mode]
            if relative not in atlas_cache:
                atlas_cache[relative] = Image.open(assets[relative]).convert("RGB")
            crops.append(atlas_cache[relative].crop(box))
        union = Image.new("L", crops[0].size, 0)
        for crop in crops:
            union = ImageChops.lighter(union, ink(crop))
        bounds = union.getbbox() or (0, 0, min(30, crops[0].width), min(30, crops[0].height))
        bounds = (max(0, bounds[0]-5), max(0, bounds[1]-5), min(crops[0].width, bounds[2]+5), min(crops[0].height, bounds[3]+5))
        tile_width = (panel_width - 32) // len(modes)
        for n, ((mode, label), crop) in enumerate(zip(modes, crops)):
            left = x + 12 + n * tile_width
            draw.text((left, y + 38), label, font=small, fill="#647087")
            crop = crop.crop(bounds)
            scale = min((tile_width - 8) / crop.width, 108 / crop.height, 4)
            crop = crop.resize((max(1, round(crop.width * scale)), max(1, round(crop.height * scale))), Image.Resampling.NEAREST)
            canvas.paste(crop, (left + (tile_width - crop.width) // 2, y + 65 + (108 - crop.height) // 2))
        difference = f'Δ {glyph["differentPixels"]} px · IoU {glyph["iou"]:.3%}'
        if "before" in glyph:
            difference = f'Δ {glyph["before"]["differentPixels"]} → {glyph["differentPixels"]} px'
        draw.text((x + 12, y + 178), difference, font=small, fill="#344058")
    canvas.save(output)


HTML = r'''<!doctype html>
<html lang="ru"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Font 0 — проверка глифов</title>
<style>
:root{color-scheme:light;font-family:system-ui,"Segoe UI",sans-serif;color:#1b2941;background:#edf1f6}
*{box-sizing:border-box}body{margin:0}header{background:#12223a;color:#f3f7ff;padding:28px max(24px,calc((100vw - 1500px)/2)) 26px}
h1{font-size:28px;margin:0 0 8px;font-weight:650}header p{margin:7px 0;color:#bccbe0;line-height:1.5;max-width:1100px}
main{max-width:1548px;margin:auto;padding:22px 24px 48px}.stats{display:grid;grid-template-columns:repeat(auto-fit,minmax(290px,1fr));gap:12px;margin-bottom:20px}
.stat{background:white;padding:18px;border:1px solid #dbe2ed;border-radius:12px}.stat strong{display:block;font-size:26px;margin-top:6px}.stat small{color:#68768a;display:block;margin-top:6px}
.controls{position:sticky;top:0;z-index:2;display:flex;gap:10px;flex-wrap:wrap;align-items:end;background:#edf1f6ee;backdrop-filter:blur(8px);padding:14px 0;border-bottom:1px solid #d4dfed}
label{display:flex;flex-direction:column;gap:5px;font-size:12px;color:#53627b}label.checkbox{flex-direction:row;align-items:center;padding:10px 0}
select,input[type=search]{font:inherit;font-size:14px;color:#1b2941;background:white;border:1px solid #bbc8dc;border-radius:7px;padding:10px;min-height:40px}#query{width:235px}#profile{max-width:400px}
.legend{display:flex;gap:18px;flex-wrap:wrap;font-size:13px;color:#4b5c76;margin:18px 0 8px}.dot{display:inline-block;width:9px;height:9px;margin-right:5px;border-radius:2px}
#counter{font-size:14px;color:#5b6d86;margin:10px 0 16px}.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(345px,1fr));gap:12px}.glyph{background:white;border:1px solid #dbe2ed;border-radius:10px;overflow:hidden;content-visibility:auto;contain-intrinsic-size:360px 270px}
.glyph.failed{border-top:3px solid #d9304b}.glyph.exact{border-top:3px solid #218664}.glyph.unknown{border-top:3px solid #9aa6b7}.top{padding:13px 14px 7px;display:flex;gap:12px;align-items:center}.character{font-size:27px;min-width:30px;font-weight:600}.identity{flex:1;font-size:12px;line-height:1.4;min-width:0}.identity b{font-size:14px}.name{color:#768299;font-size:10px;overflow-wrap:anywhere}
.badge{font-size:11px;white-space:nowrap;color:#5a6981}.tiles{display:grid;grid-template-columns:repeat(var(--columns),minmax(0,1fr));padding:8px 8px 2px;gap:3px}.tile{min-width:0;text-align:center;font-size:10px;color:#64728b}.tile canvas{display:block;width:100%;height:125px;image-rendering:pixelated;background:white;margin-top:5px}.metrics{font-size:12px;color:#465973;padding:8px 14px 13px;line-height:1.6}.pending{padding:25px 14px;color:#71809a;font-size:13px}.provenance{font-size:11px;padding:0 14px 12px;overflow-wrap:anywhere}.provenance pre{white-space:pre-wrap;max-height:220px;overflow:auto}a{color:#2465b9;text-decoration:none}a:hover{text-decoration:underline}.files{font-size:13px;display:flex;gap:18px;flex-wrap:wrap;margin:15px 0}details.metadata{font-size:12px;margin:18px 0;color:#52637c}details.metadata pre{white-space:pre-wrap;overflow-wrap:anywhere;background:white;padding:14px;border-radius:8px}button{font:inherit}
@media(max-width:600px){header{padding:22px}main{padding:14px}.grid{grid-template-columns:1fr}.controls{position:static}select,#query{max-width:100%;width:100%}label{flex:1}}
</style>
<header><h1>Font 0 · проверка каждого глифа</h1><p id="intro"></p><p>Labelary Bitonal — эталон. Все изображения ниже получены из сохранённых PNG, без подстановки системного шрифта.</p></header>
<main><div class="stats" id="stats"></div><div class="files" id="files"></div>
<div class="controls"><label>Размер / привязка / ориентация<select id="profile"></select></label><label>Символ, U+0416 или Unicode name<input id="query" type="search" placeholder="Ж, U+0416, CYRILLIC" autocomplete="off"></label><label>Результат<select id="filter"><option value="all">Все символы</option><option value="errors">Есть отличия</option><option value="exact">Пиксель в пиксель</option><option value="missing">Нет глифа у кандидата</option><option value="unmeasured">Не измерено</option></select></label><label>Масштаб<select id="zoom"><option value="fit">Вписать</option><option value="1">До 1×</option><option value="2" selected>До 2×</option><option value="4">До 4×</option></select></label><label class="checkbox"><input type="checkbox" id="unsupported">Нет у Labelary</label></div>
<div class="legend"><span><i class="dot" style="background:#1c212a"></i>Совпавшие пиксели</span><span><i class="dot" style="background:#d9304b"></i>Лишние у кандидата</span><span><i class="dot" style="background:#256add"></i>Отсутствующие у кандидата</span></div><div id="counter"></div><div class="grid" id="grid"></div>
<details class="metadata"><summary>Происхождение, параметры и границы проверки</summary><pre id="metadata"></pre></details></main>
<script id="report-data" type="application/json">__DATA__</script>
<script>
'use strict';
const D=JSON.parse(document.getElementById('report-data').textContent),$=id=>document.getElementById(id);
const images=new Map();let ticket=0;
function node(tag,text,cls){const n=document.createElement(tag);if(text!==undefined)n.textContent=text;if(cls)n.className=cls;return n;}
function image(path){if(!images.has(path))images.set(path,new Promise((resolve,reject)=>{const i=new Image();i.onload=()=>resolve(i);i.onerror=()=>reject(new Error('Не загружен '+path));i.src=path;}));return images.get(path);}
function percent(v){return (100*v).toFixed(3)+'%';}
$('intro').textContent=`${D.supportedCount} поддерживаемых кодовых точек · ${D.unsupportedCount} отсутствуют у Labelary · ${D.groups.length} профилей размеров. В карточках показаны реальные формы, в заголовках — подписи Unicode.`;
const roleLabels={'train':'Обучающие страницы · train','validation':'Независимые страницы · validation','legacy-validation':'Исторические страницы · legacy-validation'};
for(const role of ['train','validation','legacy-validation']){const s=D.statistics[role];if(!s)continue;const card=node('div',undefined,'stat');card.append(node('div',roleLabels[role]),node('strong',`${s.exact.toLocaleString()} / ${s.comparisons.toLocaleString()} точных`),node('small',`IoU ${percent(s.iou)} · Δ ${s.differentPixels.toLocaleString()} px · отсутствует ${s.missing}`));const b=D.beforeStatistics?.[role];if(b)card.append(node('small',`До: ${b.exact.toLocaleString()} / ${b.comparisons.toLocaleString()} точных · IoU ${percent(b.iou)}`));if(role==='legacy-validation')card.append(node('small','Исторические компактные ячейки могут обрезать глифы или захватывать соседей. Исключены из подбора и чистой проверки.'));$('stats').append(card);}
for(const [label,url] of D.links){const a=node('a',label);a.href=url;$('files').append(a);}
for(const g of D.groups){const o=node('option',`${g.height}×${g.width} · ${g.anchor}/${g.orientation} · ${g.role} · ${g.supportedCount}/${D.supportedCount}`);o.value=g.id;$('profile').append(o);}
$('profile').value=D.defaultGroup;$('metadata').textContent=JSON.stringify(D.provenance,null,2);
function makeTile(label,path,area,commonBox,myTicket){const el=node('div',undefined,'tile'),canvas=document.createElement('canvas');el.append(node('span',label),canvas);image(path).then(img=>{if(myTicket!==ticket)return;const crop=commonBox||area;const width=Math.max(80,Math.round(canvas.getBoundingClientRect().width)),height=125;canvas.width=width*devicePixelRatio;canvas.height=height*devicePixelRatio;const ctx=canvas.getContext('2d');ctx.scale(devicePixelRatio,devicePixelRatio);ctx.fillStyle='white';ctx.fillRect(0,0,width,height);ctx.imageSmoothingEnabled=false;const wanted=$('zoom').value;const fitting=Math.min((width-6)/crop[2],(height-6)/crop[3]);const scale=wanted==='fit'?fitting:Math.min(Number(wanted),fitting);ctx.drawImage(img,...crop,(width-crop[2]*scale)/2,(height-crop[3]*scale)/2,crop[2]*scale,crop[3]*scale);}).catch(error=>{el.replaceChildren(node('span',error.message));});return el;}
function render(){const mine=++ticket,g=D.groups.find(x=>x.id===$('profile').value),q=$('query').value.trim(),u=q.toUpperCase(),filter=$('filter').value,showUnsupported=$('unsupported').checked;const fragment=document.createDocumentFragment();let shown=0,measured=0;
for(const symbol of D.symbols){if(symbol.unsupported&&!showUnsupported)continue;if(q&&([...q].length===1?symbol.character!==q:!symbol.codepoint.includes(u)&&!symbol.name.includes(u)))continue;const m=g.glyphs[symbol.codepoint],unsupported=symbol.unsupported,exact=m&&m.differentPixels===0&&!m.missingGlyph&&!unsupported;if(filter==='errors'&&(!m||unsupported||exact))continue;if(filter==='exact'&&!exact)continue;if(filter==='missing'&&(!m||!m.missingGlyph||unsupported))continue;if(filter==='unmeasured'&&m)continue;shown++;if(m)measured++;const card=node('article',undefined,'glyph '+(!m||unsupported?'unknown':exact?'exact':'failed')),top=node('div',undefined,'top'),identity=node('div',undefined,'identity');identity.append(node('b',symbol.codepoint),node('div',symbol.name,'name'));top.append(node('span',symbol.character===' '?'␠':symbol.character,'character'),identity,node('span',!m?'не измерено':unsupported?'нет у Labelary':exact?'точно':'есть отличия','badge'));card.append(top);
if(!m){card.append(node('div','В этом профиле нет сохранённого измерения. Выберите проверочную страницу с полным набором.','pending'));fragment.append(card);continue;}
const atlas=D.atlases[m.case],modes=[['Labelary','expected']];if(D.hasBefore&&m.before&&atlas.before)modes.push(['До','before'],['Δ до','beforeDiff']);modes.push([D.hasBefore?'После':'Кандидат','actual'],[D.hasBefore?'Δ после':'Разница','diff']);const tiles=node('div',undefined,'tiles');tiles.style.setProperty('--columns',modes.length);for(const [label,key]of modes)tiles.append(makeTile(label,atlas[key],m.area,m.displayArea,mine));card.append(tiles);let line=`Δ ${m.differentPixels} px · IoU ${percent(m.iou)}`;if(m.before)line+=` · до Δ ${m.before.differentPixels} px`;if(m.missingGlyph)line+=' · глиф отсутствует у кандидата';if(unsupported)line='Нет глифа у Labelary; эта запись исключена из чисел точного совпадения.';card.append(node('div',line,'metrics'));
const provenance=D.glyphRecipes?.[symbol.codepoint];if(provenance){const details=node('details',undefined,'provenance');details.append(node('summary','Источник и преобразования'),node('pre',JSON.stringify(provenance,null,2)));card.append(details);}fragment.append(card);}
$('grid').replaceChildren(fragment);$('counter').textContent=`Показано ${shown}; измерено ${measured}. Размеры заданы в точках принтера. Сводные числа сверху считают все позиции в отчёте, включая повторные символы на разных страницах.`;const hash=new URLSearchParams({profile:g.id,q,filter});history.replaceState(null,'','#'+hash.toString());}
const initial=new URLSearchParams(location.hash.slice(1));if(D.groups.some(g=>g.id===initial.get('profile')))$('profile').value=initial.get('profile');if(initial.has('q'))$('query').value=initial.get('q');if(initial.has('filter'))$('filter').value=initial.get('filter');for(const id of ['profile','filter','zoom','unsupported'])$(id).addEventListener('change',render);$('query').addEventListener('input',render);render();
</script></html>'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="output .html; adjacent -assets directory and -contact.png are generated")
    parser.add_argument("--before-report", type=Path)
    parser.add_argument("--recipe", type=Path)
    args = parser.parse_args()
    args.manifest, args.report, args.output = args.manifest.resolve(), args.report.resolve(), args.output.resolve()
    if args.output.suffix.lower() != ".html":
        parser.error("--output must name an .html file")
    manifest, report = read_json(args.manifest), read_json(args.report)
    before = read_json(args.before_report) if args.before_report else None
    recipe = read_json(args.recipe) if args.recipe else None
    profiles = {case["name"]: case for case in manifest["cases"]}
    unsupported = set(manifest.get("unsupportedCharacters", []))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    asset_dir = args.output.parent / (args.output.stem + "-assets")
    asset_dir.mkdir(exist_ok=True)
    assets = {}

    def relative(path):
        url = quote(path.relative_to(args.output.parent).as_posix())
        assets[url] = path
        return url

    def copy(source, name):
        destination = asset_dir / name
        if source.resolve() != destination.resolve():
            shutil.copyfile(source, destination)
        return relative(destination)

    atlases, groups, before_cases = {}, {}, {c["case"]: c for c in before["cases"]} if before else {}
    for case in report["cases"]:
        name = case["case"]
        if not re.fullmatch(r"[A-Za-z0-9_-]+", name) or name not in profiles:
            raise ValueError(f"Unsafe or unknown case name: {name}")
        profile = profiles[name]
        expected_path = args.manifest.parent / (name + "-labelary-bitonal.png")
        actual_path = args.report.parent / (name + "-actual.png")
        if not actual_path.exists():
            raise FileNotFoundError(f"Missing actual atlas: {actual_path}. Rerun native probe without --no-atlas-images.")
        with Image.open(expected_path) as image:
            expected = image.convert("L")
        with Image.open(actual_path) as image:
            actual = image.convert("L")
        size = (profile.get("width", manifest["width"]), profile.get("height", manifest["height"]))
        if expected.size != size or actual.size != size:
            raise ValueError(f"Atlas dimensions disagree for {name}")
        diff, mask = overlay(expected, actual)
        diff_path = asset_dir / (name + "-diff.png")
        diff.save(diff_path)
        paths = {"expected": copy(expected_path, name + "-labelary.png"), "actual": copy(actual_path, name + "-actual.png"), "diff": relative(diff_path)}
        prior = before_cases.get(name)
        prior_glyphs = {g["codepoint"]: g for g in prior["glyphs"]} if prior else {}
        prior_image = None
        if prior:
            before_path = args.before_report.parent / (name + "-actual.png")
            with Image.open(before_path) as image:
                prior_image = image.convert("L")
            if prior_image.size != size:
                raise ValueError(f"Before atlas dimensions disagree for {name}")
            before_diff, before_mask = overlay(expected, prior_image)
            before_diff_path = asset_dir / (name + "-before-diff.png")
            before_diff.save(before_diff_path)
            paths.update(before=copy(before_path, name + "-before.png"), beforeDiff=relative(before_diff_path))
        atlases[name] = paths
        role, height, width = profile.get("role", "train"), profile["fontHeight"], profile["fontWidth"]
        anchor, orientation = profile.get("anchor", "FT"), profile.get("orientation", "N")
        key = f"{role}-{height}-{width}-{anchor}-{orientation}"
        group = groups.setdefault(key, dict(id=key, role=role, height=height, width=width, anchor=anchor, orientation=orientation, glyphs={}))
        for glyph in case["glyphs"]:
            cp = glyph["codepoint"]
            if not re.fullmatch(r"U\+[0-9A-F]{4,6}", cp):
                raise ValueError(f"Invalid code point: {cp}")
            x, y, w, h = glyph["area"]
            if not (0 <= x < x+w <= size[0] and 0 <= y < y+h <= size[1]):
                raise ValueError(f"Invalid crop for {name}/{cp}")
            box = (x, y, x+w, y+h)
            measured = mask.crop(box).histogram()[255]
            if measured != glyph["differentPixels"]:
                raise ValueError(f"Stale/inconsistent probe report {name}/{cp}: reported {glyph['differentPixels']}, PNGs have {measured} different pixels")
            entry = {k: glyph.get(k, False if k == "missingGlyph" else 0) for k in ("codepoint", "differentPixels", "iou", "missingGlyph")}
            entry.update(case=name, area=[x, y, w, h])
            union = ImageChops.lighter(ink(expected.crop(box)), ink(actual.crop(box)))
            if cp in prior_glyphs:
                old = prior_glyphs[cp]
                if old["area"] != glyph["area"] or before_mask.crop(box).histogram()[255] != old["differentPixels"]:
                    raise ValueError(f"Before probe geometry/report differs for {name}/{cp}")
                entry["before"] = {k: old.get(k, 0) for k in ("differentPixels", "iou", "missingGlyph")}
                union = ImageChops.lighter(union, ink(prior_image.crop(box)))
            bounds = union.getbbox()
            if bounds:
                left, top, right, bottom = max(0, bounds[0]-7), max(0, bounds[1]-7), min(w, bounds[2]+7), min(h, bounds[3]+7)
                entry["displayArea"] = [x+left, y+top, right-left, bottom-top]
            # Base/special atlases can contain the same CP at the same size;
            # retain one card, while aggregate statistics retain all placements.
            group["glyphs"].setdefault(cp, entry)
        print(f"Validated and copied {name}: {len(case['glyphs'])} glyph comparisons", flush=True)

    symbols = []
    for cp in manifest["characters"]:
        value = int(cp[2:], 16)
        symbols.append(dict(codepoint=cp, character=chr(value), name=unicodedata.name(chr(value), "UNNAMED"), unsupported=cp in unsupported))
    all_groups = sorted(groups.values(), key=lambda g: (g["role"] != "validation", g["height"], g["width"], g["anchor"], g["orientation"]))
    for group in all_groups:
        group["supportedCount"] = sum(cp not in unsupported for cp in group["glyphs"])
    if not all_groups:
        raise ValueError("No cases in probe report")
    default = next((g["id"] for g in all_groups if g["role"] == "validation" and g["height"] == g["width"] == 47), all_groups[0]["id"])
    contact = args.output.with_name(args.output.stem + "-contact.png")
    links = [("PNG · подборка символов", quote(contact.name)), ("Исходный отчёт кандидата", copy(args.report, "candidate-report.json")), ("Манифест эталонов", copy(args.manifest, "manifest.json"))]
    if args.before_report:
        links.append(("Исходный отчёт до изменений", copy(args.before_report, "before-report.json")))
    if args.recipe:
        links.append(("Рецепт генерации", copy(args.recipe, "recipe.json")))
    data = dict(symbols=symbols, groups=all_groups, atlases=atlases, defaultGroup=default,
                supportedCount=sum(not s["unsupported"] for s in symbols), unsupportedCount=len(unsupported),
                statistics=statistics(report, profiles, unsupported), beforeStatistics=statistics(before, profiles, unsupported) if before else {},
                hasBefore=before is not None, links=links,
                provenance=dict(candidateFont=report.get("font"), candidateFontSha256=report.get("fontSha256"),
                                probeReportSha256=sha256(args.report), manifestSha256=sha256(args.manifest),
                                rasterizer=report.get("rasterizer"), freeTypeVersion=report.get("freeTypeVersion"),
                                parameters=report.get("parameters"), recipeSha256=sha256(args.recipe) if args.recipe else None,
                                notes=["Aggregate scores exclude oracle-unsupported code points.",
                                       "Exact means zero differing bitonal pixels and no missing candidate glyph.",
                                       "Counts represent glyph placements, not unique characters. Duplicate base/special cells appear once per selected profile.",
                                       "Roles are taken from the manifest. Legacy-validation is historical evidence with known crop defects and is excluded from fitting and clean validation. This tool does not fit or modify a font."]))
    if isinstance(recipe, dict):
        records = recipe.get("glyphs", {})
        if isinstance(records, dict):
            data["glyphRecipes"] = records
        elif isinstance(records, list):
            data["glyphRecipes"] = {r["codepoint"]: r for r in records if isinstance(r, dict) and "codepoint" in r}
    contact_sheet(data, contact, assets)
    encoded = json.dumps(data, ensure_ascii=False, separators=(",", ":")).replace("<", "\\u003c").replace(">", "\\u003e").replace("&", "\\u0026")
    args.output.write_text(HTML.replace("__DATA__", encoded), encoding="utf-8")
    # Verify every generated relative image/download URL before handing off.
    for url in [path for item in atlases.values() for path in item.values()] + [url for label, url in links]:
        resolved = (args.output.parent / unquote(url)).resolve()
        if not resolved.is_file():
            raise FileNotFoundError(f"Generated HTML has a missing local dependency: {url}")
    print(json.dumps(dict(html=str(args.output), contactSheet=str(contact), assets=str(asset_dir),
                          statistics=data["statistics"], relativePathsChecked=len(assets)+1), ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
