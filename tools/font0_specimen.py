"""Create a local specimen of the saved extended Font Zero, with no external assets."""
import argparse
import json
from pathlib import Path


HTML = '''<!doctype html><html lang="ru"><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>QtZpl Font Zero · 371 глиф</title>
<style>
@font-face{font-family:Zero;src:url('QtZplFontZeroExtended.ttf') format('truetype')}
*{box-sizing:border-box}body{margin:0;background:#f3f5f8;color:#172335;font:15px system-ui,sans-serif}
main{max-width:1400px;margin:auto;padding:28px}h1{font-size:27px;margin:0 0 10px}p{line-height:1.55}
.toolbar{position:sticky;top:0;background:#fff;padding:16px;border:1px solid #cbd3dc;border-radius:10px;display:flex;flex-wrap:wrap;gap:14px;align-items:center;z-index:2}
input,select,textarea{font:inherit;border:1px solid #aebccc;border-radius:5px;padding:8px}input[type=number]{width:86px}input[type=range]{width:230px}
textarea{display:block;width:100%;height:85px;margin:16px 0;background:#fff}button{font:inherit;cursor:pointer}
.sample{font-family:Zero;font-size:var(--size,48px);white-space:pre-wrap;overflow-wrap:anywhere;background:#fff;padding:24px;border-radius:10px;min-height:120px}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(120px,1fr));gap:8px;margin-top:22px}
.glyph{background:#fff;border:1px solid #cbd3dc;border-radius:8px;min-height:124px;padding:10px;text-align:center}
.glyph span{display:block;font:52px/1.2 Zero;min-height:65px}.glyph small{display:block;font-size:11px;color:#536579;overflow-wrap:anywhere}
.glyph.extra{border-color:#c18b26;background:#fff9e9}.glyph:focus{outline:3px solid #477bc6}
#detail{background:#172335;color:#fff;padding:16px;border-radius:8px;line-height:1.5;min-height:62px;margin-top:16px}
#missing{color:#9c3018}label{display:flex;gap:7px;align-items:center}.muted{color:#536579}.count{font-weight:650}
</style><main>
<h1>QtZpl Font Zero · набор глифов</h1>
<p>371 символ: английский и русский алфавиты, цифры, пунктуация и выбранные специальные знаки.
Жёлтые карточки — 54 дополнения без эталона Labelary. 317 остальных глифов измерены, но полное пиксельное совпадение пока не достигнуто.</p>
<div class="toolbar"><label>Размер <input id="size" type="number" min="4" max="512" value="48"></label>
<input id="slider" aria-label="Размер шрифта" type="range" min="4" max="512" value="48">
<label><input id="clamp" type="checkbox" checked>Минимум Labelary: 10 точек</label><span id="effective"></span>
<select id="filter" aria-label="Группа символов"><option value="all">Все символы</option><option value="latin">Английские буквы</option><option value="russian">Русские буквы</option><option value="symbols">Цифры и знаки</option><option value="extra">Без эталона Labelary</option></select></div>
<textarea id="text" aria-label="Текст для просмотра">Съешь ещё этих мягких французских булок, да выпей чаю.
The quick brown fox jumps over the lazy dog. 0123456789 ₽ € № ± ∞ ✗</textarea>
<p class="muted">Экранный просмотр TTF использует сглаживание браузера. Точное bitonal-сравнение находится в отдельном исследовательском отчёте.</p>
<p id="missing"></p><div id="sample" class="sample"></div>
<div id="detail" aria-live="polite">Выберите карточку, чтобы увидеть источник глифа.</div>
<p class="count" id="count"></p><div id="grid" class="grid"></div>
<p><a href="QtZplFontZeroExtended.ttf">TTF · 371 глиф</a> · <a href="QtZplFontZeroResearch.ttf">TTF · 317 глифов</a> · <a href="glyph-recipe-extended.json">Рецепт</a> · <a href="svg/manifest.json">Индекс SVG</a></p>
</main><script>
const records=__DATA__, byChar=new Map(records.map(g=>[g.character,g]));
const $=id=>document.getElementById(id);
function updateSize(value){let n=Number(value);if(!Number.isFinite(n))n=48;n=Math.max(4,Math.min(512,Math.round(n)));$('size').value=n;$('slider').value=n;const actual=$('clamp').checked?Math.max(10,n):n;document.documentElement.style.setProperty('--size',actual+'px');$('effective').textContent='Показано: '+actual+' px';}
function updateText(){$('sample').textContent=$('text').value;const missing=[...new Set([...$('text').value].filter(c=>!byChar.has(c)&&!['\\n','\\r','\\t'].includes(c)))];$('missing').textContent=missing.length?'Вне набора (возможна подстановка браузером): '+missing.join(' '):'';}
function showGrid(){const kind=$('filter').value;let total=0;$('grid').replaceChildren();for(const g of records){const cp=g.value,latin=(cp>=65&&cp<=90)||(cp>=97&&cp<=122),russian=(cp>=1040&&cp<=1103)||cp===1025||cp===1105;if(kind==='latin'&&!latin||kind==='russian'&&!russian||kind==='symbols'&&(latin||russian)||kind==='extra'&&!g.extra)continue;const b=document.createElement('button');b.className='glyph'+(g.extra?' extra':'');b.type='button';const glyph=document.createElement('span');glyph.textContent=g.character;const name=document.createElement('small');name.textContent=g.codepoint;const source=document.createElement('small');source.textContent=g.source;b.append(glyph,name,source);b.addEventListener('click',()=>{$('detail').replaceChildren(document.createTextNode(g.codepoint+' '+g.character+' · '+g.source+' · '+(g.extra?'Дополнение без эталона Labelary':'Исследовательский контур; совпадение не полное')+' · '));const link=document.createElement('a');link.href='svg/u'+cp.toString(16).padStart(4,'0')+'.svg';link.textContent='Открыть SVG';link.style.color='#a9cfff';$('detail').append(link);});$('grid').append(b);total++;}$('count').textContent='Показано глифов: '+total;}
$('size').addEventListener('input',e=>updateSize(e.target.value));$('slider').addEventListener('input',e=>updateSize(e.target.value));$('clamp').addEventListener('change',()=>updateSize($('size').value));$('text').addEventListener('input',updateText);$('filter').addEventListener('change',showGrid);updateSize(48);updateText();showGrid();
</script></html>'''


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, default=Path(__file__).resolve().parents[1] / "third_party/font0-native/research")
    args = parser.parse_args()
    recipe = json.loads((args.directory / "glyph-recipe-extended.json").read_text(encoding="utf-8"))
    records = [dict(codepoint=cp, value=int(cp[2:], 16), character=chr(int(cp[2:], 16)), source=spec["sourceId"],
                    extra=spec.get("measurementStatus", "").startswith("oracle-unsupported"))
               for cp, spec in recipe["glyphs"].items()]
    data = json.dumps(records, ensure_ascii=False).replace("<", "\\u003c").replace(">", "\\u003e").replace("&", "\\u0026")
    output = args.directory / "specimen.html"
    output.write_text(HTML.replace("__DATA__", data), encoding="utf-8", newline="\n")
    print(f"Wrote {output}: {len(records)} glyphs")
