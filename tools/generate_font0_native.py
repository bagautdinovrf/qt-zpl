"""Build a reproducible OFL Font 0 research derivative from identified donors.

No proprietary font, extracted PDF font, or oracle outline is an input. Labelary
PNGs are only measurements for the separate fitter. All coordinate changes and
the original source of each glyph are recorded in the recipe.

Requires fontTools 4.63 or later. Normal library builds do not run this tool.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path

from fontTools.fontBuilder import FontBuilder
from fontTools.pens.boundsPen import BoundsPen
from fontTools.pens.cu2quPen import Cu2QuPen
from fontTools.pens.transformPen import TransformPen
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont

ROOT = Path(__file__).resolve().parents[1]
SOURCE_MANIFEST = ROOT / "third_party/font0-native/sources.json"
ATLAS_MANIFEST = ROOT / "tests/golden/font-glyphs/manifest.json"
SEMANTIC_MAPPINGS = ROOT / "third_party/font0-native/semantic-mappings.json"
EPOCH = 3786912000  # 2024-01-01, the OpenType epoch is 1904.
UPM = 2048


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_json(path: Path):
    return json.loads(path.read_text(encoding="utf-8"))


def write_json(path: Path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def verified_sources(path: Path):
    manifest = read_json(path)
    for source in manifest["fonts"]:
        if source["license_spdx"] != "OFL-1.1":
            raise ValueError(f"Incompatible donor license: {source['id']}")
        for field, digest in (("file", "sha256"), ("license_file", "license_sha256")):
            if sha256(path.parent / source[field]) != source[digest]:
                raise ValueError(f"Source checksum mismatch: {source[field]}")
    return manifest


def draw_glyph(font: TTFont, codepoint: int, transform=(1, 0, 0, 1, 0, 0)):
    glyphs = font.getGlyphSet()
    name = font.getBestCmap()[codepoint]
    pen = TTGlyphPen(None)
    # Decompose references using the glyph set; support CFF donors too.
    from fontTools.pens.recordingPen import DecomposingRecordingPen
    recording = DecomposingRecordingPen(glyphs)
    glyphs[name].draw(recording)
    recording.replay(TransformPen(Cu2QuPen(pen, max_err=0.25, reverse_direction=False), transform))
    glyph = pen.glyph()
    if glyph.numberOfContours:
        glyph.recalcBounds(None)
    return glyph


def make_font(path: Path, glyphs, advances, family: str, copyright_text: str):
    order = [".notdef"] + [f"uni{cp:04X}" for cp in sorted(glyphs)]
    blank = TTGlyphPen(None).glyph()
    mapped = {".notdef": blank, **{f"uni{cp:04X}": glyph for cp, glyph in glyphs.items()}}
    metrics = {".notdef": (UPM // 2, 0)}
    for cp, glyph in glyphs.items():
        metrics[f"uni{cp:04X}"] = (max(0, round(advances[cp])), getattr(glyph, "xMin", 0))
    builder = FontBuilder(UPM, isTTF=True)
    builder.setupGlyphOrder(order)
    builder.setupCharacterMap({cp: f"uni{cp:04X}" for cp in glyphs})
    builder.setupGlyf(mapped)
    builder.setupHorizontalMetrics(metrics)
    builder.setupHorizontalHeader(ascent=UPM, descent=-UPM // 3)
    builder.setupNameTable({
        "familyName": family, "styleName": "Regular", "fullName": family,
        "psName": family.replace(" ", ""), "uniqueFontIdentifier": family + "-0.1",
        "version": "Version 0.100; research candidate", "copyright": copyright_text,
        "licenseDescription": "SIL Open Font License 1.1. See accompanying source licenses and provenance.",
        "licenseInfoURL": "https://openfontlicense.org/",
    })
    builder.setupOS2(sTypoAscender=UPM, sTypoDescender=-UPM // 3, sTypoLineGap=0,
                     usWinAscent=UPM * 2, usWinDescent=UPM, sCapHeight=UPM * 3 // 4,
                     sxHeight=UPM // 2, usWeightClass=700, fsType=0)
    builder.setupPost()
    builder.setupMaxp()
    builder.setupHead(unitsPerEm=UPM, created=EPOCH, modified=EPOCH)
    builder.font.recalcTimestamp = False
    path.parent.mkdir(parents=True, exist_ok=True)
    builder.save(path)


def prepare(args):
    manifest = verified_sources(args.sources)
    census = read_json(args.manifest)
    characters = {int(cp[2:], 16) for cp in census["characters"]}
    alias_path = getattr(args, "semantic_mappings", None)
    mapping = read_json(alias_path) if alias_path else {}
    common_aliases = {int(cp[2:], 16): int(value["sourceCodepoint"][2:], 16)
                      for cp, value in mapping.get("aliases", {}).items()}
    # Missing-oracle characters stay in donor coverage, but fitting excludes
    # them; they can never win with an empty/fallback glyph by accident.
    output = args.output.resolve()
    candidates = []
    for source_index, source in enumerate(manifest["fonts"]):
        aliases = dict(common_aliases)
        aliases.update({int(cp[2:], 16): int(value["sourceCodepoint"][2:], 16)
                        for cp, value in mapping.get("sourceAliases", {}).get(source["id"], {}).items()})
        original = TTFont(args.sources.parent / source["file"], recalcTimestamp=False)
        weights = args.weights if any(a["tag"] == "wght" for a in source["axes"]) else [700]
        instances = []
        for weight in weights:
            axes = dict(source["default_instance"])
            for axis in source["axes"]:
                if axis["tag"] == "wght":
                    axes["wght"] = max(axis["minimum"], min(axis["maximum"], weight))
            if axes in instances:
                continue
            instances.append(axes)
            donor = instantiateVariableFont(original, axes, inplace=False) if "fvar" in original else original
            cmap = donor.getBestCmap()
            codepoints = sorted(cp for cp in characters if aliases.get(cp, cp) in cmap)
            bounds = BoundsPen(donor.getGlyphSet())
            donor.getGlyphSet()[cmap[ord("H")]].draw(bounds)
            cap = bounds.bounds[3] if bounds.bounds else donor["head"].unitsPerEm * .75
            scale = UPM * .75 / cap
            glyphs = {cp: draw_glyph(donor, aliases.get(cp, cp), (scale, 0, 0, scale, 0, 0)) for cp in codepoints}
            advances = {cp: donor["hmtx"][cmap[aliases.get(cp, cp)]][0] * scale for cp in codepoints}
            identifier = source["id"] + "-" + "-".join(f"{k}{v:g}" for k, v in sorted(axes.items())) if axes else source["id"] + "-bold"
            filename = identifier + ".ttf"
            make_font(output / filename, glyphs, advances,
                      f"QtZpl Candidate {source_index + 1} Instance {len(instances)}",
                      "\n".join(dict.fromkeys(source["copyright"] + [source["license_notice"]])))
            candidates.append({"id": identifier, "file": filename, "sha256": sha256(output / filename),
                               "sourceId": source["id"], "sourceSha256": source["sha256"], "axes": axes,
                               "sourceOrigin": source.get("origin", "upstream"),
                               "glyphAliases": {f"U+{cp:04X}": f"U+{aliases[cp]:04X}" for cp in codepoints if cp in aliases},
                               "normalizationScale": scale, "unitsPerEm": UPM,
                               "codepoints": [f"U+{cp:04X}" for cp in codepoints]})
            print(f"Prepared {identifier}: {len(codepoints)} glyphs", flush=True)
            if donor is not original:
                donor.close()
        original.close()
    write_json(output / "candidates.json", {"schemaVersion": 1, "sourceManifestSha256": sha256(args.sources),
               "censusSha256": sha256(args.manifest),
               "semanticMappingsSha256": sha256(alias_path) if alias_path else None, "candidates": candidates})


def build(args):
    sources = verified_sources(args.sources)
    recipe = read_json(args.recipe)
    candidates = read_json(args.candidates)
    if candidates["sourceManifestSha256"] != sha256(args.sources):
        raise ValueError("Candidate source manifest checksum mismatch")
    if recipe.get("sourceManifestSha256") != sha256(args.sources) or recipe.get("candidatesManifestSha256") != sha256(args.candidates):
        raise ValueError("Glyph recipe provenance checksum mismatch")
    source_by_id = {source["id"]: source for source in sources["fonts"]}
    family = recipe.get("family", "QtZpl Font Zero Research")
    if not isinstance(family, str) or not family.startswith("QtZpl ") or not family[6:].strip() or len(family) > 80 or any(not (c.isascii() and (c.isalnum() or c == " ")) for c in family):
        raise ValueError("Derivative family must use an ASCII QtZpl name")
    for source in sources["fonts"]:
        for reserved in source.get("reserved_font_names", []):
            if reserved and reserved.casefold() in family.casefold():
                raise ValueError(f"Derivative family contains Reserved Font Name: {reserved}")
    for donor in candidates["candidates"]:
        if donor["sourceId"] not in source_by_id or donor["sourceSha256"] != source_by_id[donor["sourceId"]]["sha256"]:
            raise ValueError(f"Candidate source provenance mismatch: {donor['id']}")
    donors = {candidate["id"]: candidate for candidate in candidates["candidates"]}
    fonts = {}
    glyphs = {}
    advances = {}
    used = set()
    for cp_text, spec in sorted(recipe["glyphs"].items()):
        cp = int(cp_text[2:], 16)
        donor = donors[spec["donor"]]
        if spec.get("sourceId") != donor["sourceId"] or spec.get("sourceSha256") != donor["sourceSha256"]:
            raise ValueError(f"Glyph source provenance mismatch: {cp_text}")
        if spec.get("sourceCodepoint", cp_text) != donor.get("glyphAliases", {}).get(cp_text, cp_text):
            raise ValueError(f"Glyph source codepoint mismatch: {cp_text}")
        path = args.candidates.parent / donor["file"]
        if donor["id"] not in fonts:
            if sha256(path) != donor["sha256"]:
                raise ValueError(f"Prepared donor checksum mismatch: {path}")
            fonts[donor["id"]] = TTFont(path, recalcTimestamp=False)
        font = fonts[donor["id"]]
        adjustment = spec.get("adjustment", {})
        sx, sy = adjustment.get("scaleX", 1), adjustment.get("scaleY", 1)
        dx, dy = adjustment.get("offsetX", 0) * UPM, adjustment.get("offsetY", 0) * UPM
        if not all(math.isfinite(v) for v in (sx, sy, dx, dy)) or not 0 < sx <= 5 or not 0 < sy <= 5 or max(abs(dx), abs(dy)) > UPM * 5:
            raise ValueError(f"Invalid glyph transform for {cp_text}")
        glyph = draw_glyph(font, cp, (sx, 0, 0, sy, dx, dy))
        if "sourcePointCount" in spec and spec["sourcePointCount"] != (len(glyph.coordinates) if glyph.numberOfContours else 0):
            raise ValueError(f"Edited outline point count changed for {cp_text}")
        for edit in spec.get("pointEdits", []):
            point = edit["point"]
            if type(point) is not int or point < 0 or point >= len(glyph.coordinates):
                raise ValueError(f"Point index out of range for {cp_text}: {point}")
            if not all(type(edit[key]) in (int, float) and math.isfinite(edit[key]) and abs(edit[key]) <= UPM
                       for key in ("dx", "dy")):
                raise ValueError(f"Invalid control point displacement for {cp_text}")
            x, y = glyph.coordinates[point]
            glyph.coordinates[point] = (round(x + edit["dx"]), round(y + edit["dy"]))
        if glyph.numberOfContours:
            glyph.recalcBounds(None)
        glyphs[cp] = glyph
        advance = spec["advanceEm"]
        if type(advance) not in (int, float) or not math.isfinite(advance) or not 0 <= advance <= 5:
            raise ValueError(f"Invalid advance for {cp_text}")
        advances[cp] = advance * UPM
        used.add(donor["sourceId"])
    copyrights = [text for source in sources["fonts"] if source["id"] in used
                  for text in source["copyright"] + [source["license_notice"]]]
    copyrights = list(dict.fromkeys(copyrights))
    copyrights.append("Copyright 2026 QtZpl contributors. Per-glyph selection, geometric adjustments and assembly.")
    make_font(args.output, glyphs, advances, family, "\n".join(copyrights))
    for font in fonts.values():
        font.close()
    print(f"Built {args.output}: {len(glyphs)} glyphs, {len(used)} source families, sha256={sha256(args.output)}")


def refine_recipe(recipe, fitted, font_path):
    """Compose native fitter point deltas with existing, traceable edits."""
    if fitted["fontSha256"] != sha256(font_path):
        raise ValueError("Outline fitting report does not match the composed font")
    result = json.loads(json.dumps(recipe))
    changed = 0
    for cp, edits in fitted["glyphs"].items():
        if cp not in result["glyphs"] or edits.get("missingGlyph"):
            continue
        if edits["unitsPerEm"] != UPM:
            raise ValueError("Outline fitting units mismatch")
        spec = result["glyphs"][cp]
        combined = {edit["point"]: dict(edit) for edit in spec.get("pointEdits", [])}
        for edit in edits["pointEdits"]:
            current = combined.setdefault(edit["point"], {"point": edit["point"], "dx": 0, "dy": 0})
            current["dx"] += edit["dx"]
            current["dy"] += edit["dy"]
        spec["pointEdits"] = [edit for _, edit in sorted(combined.items()) if edit["dx"] or edit["dy"]]
        spec["sourcePointCount"] = edits["sourcePointCount"]
        spec["outlineFit"] = {key: edits[key] for key in ("initialLoss", "loss", "differentPixels", "unionPixels", "warp")}
        changed += bool(edits["pointEdits"])
    result["outlineRefinement"] = {"inputFontSha256": fitted["fontSha256"], "changedGlyphs": changed,
                                   "method": fitted.get("method"), "trainingCases": fitted.get("trainingCases", [])}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sources", type=Path, default=SOURCE_MANIFEST)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("prepare", help="Normalize licensed donors into a common, unhinted design grid")
    p.add_argument("--manifest", type=Path, default=ATLAS_MANIFEST)
    p.add_argument("--output", type=Path, default=ROOT / "build_font0_research/donors")
    p.add_argument("--weights", type=float, nargs="+", default=[600, 700, 800])
    p.add_argument("--semantic-mappings", type=Path, default=SEMANTIC_MAPPINGS)
    p.set_defaults(run=prepare)
    p = sub.add_parser("build", help="Assemble a font from a reviewable glyph recipe")
    p.add_argument("--recipe", type=Path, required=True)
    p.add_argument("--candidates", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.set_defaults(run=build)
    args = parser.parse_args()
    args.run(args)


if __name__ == "__main__":
    main()
