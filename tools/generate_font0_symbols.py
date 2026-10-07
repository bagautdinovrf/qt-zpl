#!/usr/bin/env python3
"""Generate original geometric Font 0 donor glyphs under OFL-1.1.

Every outline below is authored from rectangles and polygons in this file.
No installed font, downloaded font outline, raster, or Labelary contour is an
input. Dimensions are design choices, not a claim of printer equivalence.

    py -3 tools/generate_font0_symbols.py --update-manifest
    py -3 tools/generate_font0_symbols.py --check

Requires fontTools. Normal library builds never run this maintenance tool.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
from pathlib import Path

import fontTools
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import TTFont


ROOT = Path(__file__).resolve().parents[1]
DIRECTORY = ROOT / "third_party/font0-native/sources/qtzpl-geometric"
FONT_PATH = DIRECTORY / "QtZplGeometric-Bold.ttf"
MANIFEST_PATH = ROOT / "third_party/font0-native/sources.json"
COPYRIGHT = "Copyright 2026 QtZpl contributors."
UPM = 2048
CAP_HEIGHT = 1536
EPOCH = 3786912000


def rectangle(left: int, bottom: int, right: int, top: int):
    if left >= right or bottom >= top:
        raise ValueError("A rectangle must have positive dimensions")
    return [(left, bottom), (left, top), (right, top), (right, bottom)]


def mirrored(contours, extent: int):
    return [[(extent - x, y) for x, y in contour] for contour in contours]


def design():
    """Return codepoint -> (advance, original polygon contours)."""
    glyphs = {}

    def add(characters: str, advance: int, *contours):
        for character in characters:
            cp = ord(character)
            if cp in glyphs:
                raise ValueError(f"Repeated code point U+{cp:04X}")
            glyphs[cp] = (advance, [list(contour) for contour in contours])

    add(" \u00a0", 512)
    add(".", 448, rectangle(96, 0, 352, 256))
    comma = [(96, 256), (352, 256), (352, -128), (160, -320),
             (80, -240), (192, -96), (96, -96)]
    add(",", 448, comma)
    add(":", 448, rectangle(96, 0, 352, 256), rectangle(96, 768, 352, 1024))
    add(";", 448, comma, rectangle(96, 768, 352, 1024))
    add("-\u2010\u2011", 896, rectangle(64, 576, 832, 800))
    add("\u2013", 1152, rectangle(64, 576, 1088, 800))
    add("\u2014", 1664, rectangle(64, 576, 1600, 800))
    add("_", 1024, rectangle(0, -192, 1024, -16))
    add("|", 544, rectangle(160, -384, 384, 1664))
    add("\u00a6", 544, rectangle(160, -384, 384, 512), rectangle(160, 768, 384, 1664))
    plus = [(400, 64), (400, 464), (0, 464), (0, 688), (400, 688), (400, 1088),
            (624, 1088), (624, 688), (1024, 688), (1024, 464), (624, 464), (624, 64)]
    add("+", 1152, plus)
    add("=", 1152, rectangle(0, 352, 1024, 576), rectangle(0, 832, 1024, 1056))
    add("\u2212", 1152, rectangle(0, 576, 1024, 800))
    add("\u00b1", 1152, plus, rectangle(0, -320, 1024, -96))
    less = [[(1056, 1152), (1056, 896), (352, 576), (1056, 256),
             (1056, 0), (64, 448), (64, 704)]]
    add("<", 1152, *less)
    add(">", 1152, *mirrored(less, 1120))
    slash = [[(32, -256), (320, -256), (1056, 1664), (768, 1664)]]
    add("/", 1088, *slash)
    add("\\", 1088, *mirrored(slash, 1088))
    add("!", 448, rectangle(96, 0, 352, 256), rectangle(96, 512, 352, 1536))
    add("\"", 672, rectangle(80, 1120, 240, 1536), rectangle(432, 1120, 592, 1536))
    add("'", 448, [(96, 1536), (352, 1536), (352, 1216), (176, 1056),
                    (96, 1136), (224, 1280), (96, 1280)])
    add("`", 640, [(128, 1536), (352, 1264), (560, 1264), (352, 1536)])
    bracket = [[(64, -256), (64, 1664), (640, 1664), (640, 1472),
                (256, 1472), (256, -64), (640, -64), (640, -256)]]
    add("[", 704, *bracket)
    add("]", 704, *mirrored(bracket, 704))
    add("\u00d7", 1152, [(0, 160), (160, 0), (1024, 864), (864, 1024)],
        [(0, 864), (160, 1024), (1024, 160), (864, 0)])
    # A separately authored tall ballot cross, with four broad diagonal arms
    # joined into one solid polygon. It is deliberately not the multiplication
    # sign's outline or a Unicode alias, and has no oracle-derived geometry.
    add("\u2717", 1344, [(64, 160), (256, 0), (672, 544), (1088, 0),
                        (1280, 160), (848, 768), (1280, 1376), (1088, 1536),
                        (672, 992), (256, 1536), (64, 1376), (496, 768)])

    # Original rectilinear letter skeletons. The H is also the donor's cap
    # calibration character; its top is exactly 0.75 em in the source font.
    h = [(0, 0), (0, 1536), (256, 1536), (256, 896), (768, 896), (768, 1536),
         (1024, 1536), (1024, 0), (768, 0), (768, 640), (256, 640), (256, 0)]
    add("H\u041d\u0397", 1152, h)
    add("I\u0406\u0399", 384, rectangle(64, 0, 320, 1536))
    t = [(384, 0), (384, 1280), (0, 1280), (0, 1536), (1024, 1536),
         (1024, 1280), (640, 1280), (640, 0)]
    add("T\u0422\u03a4", 1152, t)
    add("L", 1024, [(0, 0), (0, 1536), (256, 1536), (256, 256), (896, 256), (896, 0)])
    e = [(0, 0), (0, 1536), (960, 1536), (960, 1280), (256, 1280), (256, 896),
         (864, 896), (864, 640), (256, 640), (256, 256), (960, 256), (960, 0)]
    add("E\u0415\u0395", 1088, e)
    add("F", 1088, [(0, 0), (0, 1536), (960, 1536), (960, 1280), (256, 1280),
                    (256, 896), (864, 896), (864, 640), (256, 640), (256, 0)])
    add("\u0413", 1088, [(0, 0), (0, 1536), (960, 1536), (960, 1280), (256, 1280), (256, 0)])
    add("\u041f", 1152, [(0, 0), (0, 1536), (1024, 1536), (1024, 0), (768, 0),
                         (768, 1280), (256, 1280), (256, 0)])
    add("\u0428", 1536, [(0, 0), (0, 1536), (256, 1536), (256, 256), (576, 256),
                         (576, 1536), (832, 1536), (832, 256), (1152, 256),
                         (1152, 1536), (1408, 1536), (1408, 0)])
    return glyphs


def polygon_glyph(contours):
    pen = TTGlyphPen(None)
    for original in contours:
        points = list(original)
        # Keep all solid polygons clockwise, including mirrored symbols.
        area = sum(a[0] * b[1] - b[0] * a[1] for a, b in zip(points, points[1:] + points[:1]))
        if area == 0:
            raise ValueError("Degenerate polygon")
        if area > 0:
            points.reverse()
        pen.moveTo(points[0])
        for point in points[1:]:
            pen.lineTo(point)
        pen.closePath()
    return pen.glyph()


def font_bytes() -> bytes:
    glyph_designs = design()
    names = {cp: f"uni{cp:04X}" for cp in sorted(glyph_designs)}
    builder = FontBuilder(UPM, isTTF=True)
    builder.setupGlyphOrder([".notdef", *names.values()])
    builder.setupCharacterMap(names)
    glyphs = {".notdef": TTGlyphPen(None).glyph()}
    glyphs.update({names[cp]: polygon_glyph(contours) for cp, (_, contours) in glyph_designs.items()})
    builder.setupGlyf(glyphs)
    metrics = {".notdef": (512, 0)}
    metrics.update({names[cp]: (advance, getattr(glyphs[names[cp]], "xMin", 0))
                    for cp, (advance, _) in glyph_designs.items()})
    builder.setupHorizontalMetrics(metrics)
    builder.setupHorizontalHeader(ascent=1792, descent=-512)
    builder.setupNameTable({"familyName": "QtZpl Geometric", "styleName": "Bold",
                            "fullName": "QtZpl Geometric Bold", "psName": "QtZplGeometric-Bold",
                            "uniqueFontIdentifier": "QtZplGeometric-Bold-0.100",
                            "version": "Version 0.100; original geometric primitives",
                            "copyright": COPYRIGHT,
                            "licenseDescription": "SIL Open Font License 1.1. See accompanying OFL.txt.",
                            "licenseInfoURL": "https://openfontlicense.org/"})
    builder.setupOS2(sTypoAscender=1792, sTypoDescender=-512, sTypoLineGap=0,
                     usWinAscent=1792, usWinDescent=512, sCapHeight=CAP_HEIGHT,
                     sxHeight=1024, usWeightClass=700, fsType=0)
    builder.setupPost()
    builder.setupMaxp()
    builder.setupHead(unitsPerEm=UPM, created=EPOCH, modified=EPOCH)
    builder.font.recalcTimestamp = False
    output = io.BytesIO()
    builder.save(output)
    return output.getvalue()


def update_manifest() -> None:
    from fetch_font0_sources import font_coverage
    manifest = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
    license_path = DIRECTORY / "OFL.txt"
    binary, license_data = FONT_PATH.read_bytes(), license_path.read_bytes()
    with TTFont(io.BytesIO(binary)) as font:
        coverage = font_coverage(font.getBestCmap())
    entry = {"id": "qtzpl-geometric", "family": "QtZpl Geometric", "style": "Bold",
             "origin": "authored", "file": "sources/qtzpl-geometric/QtZplGeometric-Bold.ttf",
             "sha256": hashlib.sha256(binary).hexdigest(), "size_bytes": len(binary),
             "license_spdx": "OFL-1.1", "license_file": "sources/qtzpl-geometric/OFL.txt",
             "license_sha256": hashlib.sha256(license_data).hexdigest(), "license_size_bytes": len(license_data),
             "generator_file": "tools/generate_font0_symbols.py",
             "generator_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
             "generator_command": "py -3 tools/generate_font0_symbols.py --update-manifest",
             "fonttools_version": fontTools.__version__, "upstream_repository": None,
             "version": ["Version 0.100; original geometric primitives"],
             "copyright": [COPYRIGHT], "license_notice": COPYRIGHT, "reserved_font_names": [],
             "axes": [], "default_instance": {}, "coverage": coverage,
             "authorship": "Original rectangles and polygons authored in the pinned generator. No imported font outlines, raster tracing, or oracle contours."}
    for index, existing in enumerate(manifest["fonts"]):
        if existing["id"] == entry["id"]:
            manifest["fonts"][index] = entry
            break
    else:
        manifest["fonts"].append(entry)
    MANIFEST_PATH.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output", type=Path, default=FONT_PATH)
    action = parser.add_mutually_exclusive_group()
    action.add_argument("--check", action="store_true", help="Compare with the existing font without writing")
    action.add_argument("--update-manifest", action="store_true", help="Refresh authored source provenance after generation")
    args = parser.parse_args()
    if args.update_manifest and args.output.resolve() != FONT_PATH.resolve():
        parser.error("--update-manifest requires the default committed output path")
    data = font_bytes()
    if args.check:
        if not args.output.is_file() or args.output.read_bytes() != data:
            print(f"Generated font differs from {args.output}")
            return 1
    else:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(data)
        if args.update_manifest:
            update_manifest()
    print(f"{len(design())} original glyphs; {len(data)} bytes; sha256={hashlib.sha256(data).hexdigest()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
