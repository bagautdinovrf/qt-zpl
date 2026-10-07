#!/usr/bin/env python3
"""Add licensed outlines for oracle-unsupported characters to a separate font.

This is an unvalidated character-coverage extension, not a parity measurement.
It reads no bitmaps and preserves every existing recipe record unchanged.
Requires fontTools; sources and prepared donors must already be cached locally.
"""
from __future__ import annotations

import argparse
import copy
import math
from pathlib import Path
from types import SimpleNamespace

from fontTools.ttLib import TTFont

import generate_font0_native as generator

FAMILY = "QtZpl Font Zero Extended"
MEASUREMENT_STATUS = "oracle-unsupported; unvalidated extension"
PREFERRED_DONORS = (
    "roboto-condensed-wght700", "noto-sans-wdth75-wght700",
    "stix-two-math-bold", "noto-sans-math-bold",
)


def codepoint(text: str) -> int:
    if not isinstance(text, str) or not text.startswith("U+"):
        raise ValueError(f"Invalid codepoint: {text!r}")
    cp = int(text[2:], 16)
    if not 0 <= cp <= 0x10FFFF or 0xD800 <= cp <= 0xDFFF or text != f"U+{cp:04X}":
        raise ValueError(f"Invalid canonical Unicode scalar: {text}")
    return cp


def donor_priority(donor):
    identifier = donor["id"]
    return (PREFERRED_DONORS.index(identifier) if identifier in PREFERRED_DONORS
            else len(PREFERRED_DONORS), identifier)


def extend_recipe(recipe_path: Path, candidates_path: Path, sources_path: Path):
    sources = generator.verified_sources(sources_path)
    source_by_id = {item["id"]: item for item in sources["fonts"]}
    candidates = generator.read_json(candidates_path)
    recipe = generator.read_json(recipe_path)
    if candidates.get("sourceManifestSha256") != generator.sha256(sources_path):
        raise ValueError("Candidate source manifest checksum mismatch")
    if recipe.get("sourceManifestSha256") != generator.sha256(sources_path) or recipe.get("candidatesManifestSha256") != generator.sha256(candidates_path):
        raise ValueError("Base recipe provenance checksum mismatch")
    if recipe.get("extension") or any(item.get("measurementStatus") == MEASUREMENT_STATUS for item in recipe["glyphs"].values()):
        raise ValueError("Input must be the measured base recipe, not an extension")
    unsupported = recipe.get("oracleUnsupported", [])
    if len(set(unsupported)) != len(unsupported):
        raise ValueError("Duplicate oracle-unsupported codepoint")
    unsupported = sorted(unsupported, key=codepoint)
    for text in recipe["glyphs"]:
        codepoint(text)
    overlap = set(unsupported) & recipe["glyphs"].keys()
    if overlap:
        raise ValueError(f"Unsupported codepoints are already present in the base: {sorted(overlap)}")
    donors = sorted(candidates["candidates"], key=donor_priority)
    if len({item["id"] for item in donors}) != len(donors):
        raise ValueError("Duplicate prepared donor identifier")
    fonts = {}
    try:
        for donor in donors:
            source = source_by_id.get(donor["sourceId"])
            if source is None or donor["sourceSha256"] != source["sha256"]:
                raise ValueError(f"Candidate source provenance mismatch: {donor['id']}")
            path = candidates_path.parent / donor["file"]
            if generator.sha256(path) != donor["sha256"]:
                raise ValueError(f"Prepared donor checksum mismatch: {path}")
            font = TTFont(path, recalcTimestamp=False)
            fonts[donor["id"]] = font
            if donor.get("unitsPerEm") != generator.UPM or font["head"].unitsPerEm != generator.UPM:
                raise ValueError(f"Prepared donor must use {generator.UPM} units per em: {donor['id']}")
        result = copy.deepcopy(recipe)
        result["family"] = FAMILY
        result["status"] = "research; contains unvalidated oracle-unsupported extension"
        added = []
        missing = []
        for text in unsupported:
            cp = codepoint(text)
            for donor in donors:
                font = fonts[donor["id"]]
                glyph_name = font.getBestCmap().get(cp)
                if not glyph_name or glyph_name == ".notdef":
                    continue
                # An encoded blank/tofu slot is not proof of a usable outline.
                glyph = generator.draw_glyph(font, cp)
                if not glyph.numberOfContours or not len(glyph.coordinates):
                    continue
                source_cp = donor.get("glyphAliases", {}).get(text, text)
                codepoint(source_cp)
                advance = font["hmtx"][glyph_name][0] / generator.UPM
                if not math.isfinite(advance) or not 0 <= advance <= 5:
                    raise ValueError(f"Invalid donor advance: {donor['id']} {text}")
                result["glyphs"][text] = {
                    "donor": donor["id"], "donorSha256": donor["sha256"],
                    "sourceId": donor["sourceId"], "sourceSha256": donor["sourceSha256"],
                    "sourceCodepoint": source_cp,
                    "advanceEm": advance, "advanceSource": "prepared donor hmtx / 2048",
                    "adjustment": {"scaleX": 1.0, "scaleY": 1.0, "offsetX": 0.0, "offsetY": 0.0},
                    "adjustmentDerivation": "Identity on prepared 2048 UPM donor; no oracle fit or training corrections",
                    "sourcePointCount": len(glyph.coordinates), "pointEdits": [],
                    "measurementStatus": MEASUREMENT_STATUS,
                }
                added.append(text)
                break
            else:
                missing.append(text)
        if missing:
            raise ValueError("No licensed nonempty outline for extension: " + ", ".join(missing))
        result["glyphs"] = dict(sorted(result["glyphs"].items(), key=lambda item: codepoint(item[0])))
        result["extension"] = {
            "schemaVersion": 1, "baseRecipeSha256": generator.sha256(recipe_path),
            "baseFamily": recipe.get("family", "QtZpl Font Zero Research"),
            "baseGlyphCount": len(recipe["glyphs"]), "addedGlyphCount": len(added),
            "totalGlyphCount": len(result["glyphs"]), "addedCodepoints": added,
            "selectionOrder": [item["id"] for item in donors],
            "selectionRule": "First prepared donor with a mapped, nonempty outline; preferred IDs then lexical ID",
            "measurementStatus": MEASUREMENT_STATUS,
            "validationScope": "cmap and outline presence only; no Labelary parity claim",
            "scalePolicy": "Identity on normalized prepared fonts; offsets zero; donor horizontal advance",
            "generatorSha256": generator.sha256(Path(__file__).resolve()),
        }
        return result
    finally:
        for font in fonts.values():
            font.close()


def check_coverage(font_path: Path, recipe):
    expected = {codepoint(text) for text in recipe["glyphs"]}
    added = set(recipe["extension"]["addedCodepoints"])
    records = []
    with TTFont(font_path) as font:
        cmap = font.getBestCmap()
        if set(cmap) != expected or font["head"].unitsPerEm != generator.UPM:
            raise ValueError("Extended font cmap or units per em mismatch")
        for cp in sorted(expected):
            text = f"U+{cp:04X}"
            name = cmap[cp]
            glyph = font["glyf"][name]
            has_outline = bool(glyph.numberOfContours and len(glyph.coordinates))
            # The census contains legitimate blank SPACE and NO-BREAK SPACE.
            blank_allowed = cp in (0x20, 0xA0) and text not in added
            if name == ".notdef" or (not has_outline and not blank_allowed):
                raise ValueError(f"Extended font has no usable outline: {text}")
            records.append({"codepoint": text, "mapped": True, "hasOutline": has_outline,
                            "intentionalWhitespace": blank_allowed,
                            "measurementStatus": MEASUREMENT_STATUS if text in added else "inherited base recipe; not remeasured"})
    return {"schemaVersion": 1, "family": FAMILY, "fontSha256": generator.sha256(font_path),
            "unitsPerEm": generator.UPM, "glyphCount": len(expected),
            "outlineCount": sum(item["hasOutline"] for item in records),
            "extensionGlyphCount": len(added), "validationScope": "cmap and outline presence only",
            "pixelParityEvaluated": False, "glyphs": records}


def extend(args):
    result = extend_recipe(args.recipe, args.candidates, args.sources)
    args.outputdir.mkdir(parents=True, exist_ok=True)
    recipe_path = args.outputdir / "glyph-recipe-extended.json"
    font_path = args.outputdir / "QtZplFontZeroExtended.ttf"
    generator.write_json(recipe_path, result)
    generator.build(SimpleNamespace(recipe=recipe_path, candidates=args.candidates,
                                    sources=args.sources, output=font_path))
    report = check_coverage(font_path, result)
    report["recipeSha256"] = generator.sha256(recipe_path)
    generator.write_json(args.outputdir / "coverage.json", report)
    print(f"Coverage verified: {report['glyphCount']} mapped glyphs, {report['outlineCount']} outlines; "
          f"{report['extensionGlyphCount']} unvalidated additions. Pixel parity was not evaluated.")
    return result, report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--recipe", type=Path, required=True, help="Final measured base recipe")
    parser.add_argument("--candidates", type=Path, required=True)
    parser.add_argument("--sources", type=Path, default=generator.SOURCE_MANIFEST)
    parser.add_argument("--outputdir", type=Path, required=True)
    extend(parser.parse_args())


if __name__ == "__main__":
    main()
