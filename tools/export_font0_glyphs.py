"""Export editable SVG outlines from an assembled research TTF, without raster input."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from xml.sax.saxutils import escape

from fontTools.pens.boundsPen import BoundsPen
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.ttLib import TTFont


def export(font_path: Path, recipe_path: Path, output: Path):
    recipe = json.loads(recipe_path.read_text(encoding="utf-8"))
    output.mkdir(parents=True, exist_ok=True)
    manifest = {"schemaVersion": 1, "fontSha256": hashlib.sha256(font_path.read_bytes()).hexdigest(),
                "recipeSha256": hashlib.sha256(recipe_path.read_bytes()).hexdigest(), "glyphs": {}}
    with TTFont(font_path, recalcTimestamp=False) as font:
        glyphs, cmap = font.getGlyphSet(), font.getBestCmap()
        upm = font["head"].unitsPerEm
        if set(cmap) != {int(cp[2:], 16) for cp in recipe["glyphs"]}:
            raise ValueError("Font coverage does not match the recipe")
        manifest["unitsPerEm"] = upm
        for cp, name in sorted(cmap.items()):
            key = f"U+{cp:04X}"
            path_pen, bounds_pen = SVGPathPen(glyphs), BoundsPen(glyphs)
            glyphs[name].draw(path_pen)
            glyphs[name].draw(bounds_pen)
            advance = font["hmtx"][name][0]
            bounds = bounds_pen.bounds or (0, 0, advance, upm * .75)
            x0, y0, x1, y1 = bounds
            margin = upm / 32
            viewbox = (x0 - margin, -y1 - margin, max(1, x1 - x0) + 2 * margin, max(1, y1 - y0) + 2 * margin)
            metadata = json.dumps({"codepoint": key, "advance": advance, "unitsPerEm": upm,
                                   "sourceId": recipe["glyphs"][key]["sourceId"],
                                   "fontSha256": manifest["fontSha256"]}, ensure_ascii=False)
            svg = ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="' + " ".join(f"{n:g}" for n in viewbox)
                   + '">\n<title>' + escape(key + " " + chr(cp)) + '</title>\n<metadata>' + escape(metadata)
                   + '</metadata>\n<path fill="black" transform="scale(1,-1)" d="'
                   + escape(path_pen.getCommands(), {'"': "&quot;"}) + '"/>\n</svg>\n')
            filename = f"u{cp:04x}.svg"
            (output / filename).write_text(svg, encoding="utf-8", newline="\n")
            manifest["glyphs"][key] = {"file": filename, "advance": advance, "bounds": list(bounds),
                                       "sha256": hashlib.sha256(svg.encode()).hexdigest()}
    (output / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(f"Exported {len(manifest['glyphs'])} vector glyphs to {output}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--font", type=Path, required=True)
    parser.add_argument("--recipe", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    export(args.font, args.recipe, args.output)
