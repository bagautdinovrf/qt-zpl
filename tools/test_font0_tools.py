#!/usr/bin/env python3
"""Offline regressions for the native Font 0 donor/recipe generator.

Run: py -3 tools/test_font0_tools.py
Requires fontTools. All donor fonts are tiny synthetic FontBuilder fixtures;
the tests neither download fonts nor inspect installed or proprietary fonts.
"""

from __future__ import annotations

import contextlib
import copy
import hashlib
import io
import json
from pathlib import Path
import shutil
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import TTFont

import generate_font0_native as generator
import generate_font0_symbols as geometric
import fetch_font0_sources as source_fetcher
import research_font0 as research
import extend_font0_research as extension


LATIN = set(range(0x20, 0x7F))
RUSSIAN = set(range(0x0410, 0x0450)) | {0x0401, 0x0451}
ALL_CHARACTERS = LATIN | RUSSIAN


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def save_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def outline(cp: int) -> list[tuple[int, int]]:
    if cp == 0x20:
        return []
    if cp == ord("H"):
        return [(20, 0), (20, 768), (120, 768), (120, 430),
                (480, 430), (480, 768), (580, 768), (580, 0),
                (480, 0), (480, 330), (120, 330), (120, 0)]
    # Each encoded character has its own independently checkable triangle.
    return [(20 + cp % 17, 0), (180 + cp % 13, 700 + cp % 47),
            (470 + cp % 29, 0)]


def glyph_for(points: list[tuple[int, int]]):
    pen = TTGlyphPen(None)
    if points:
        pen.moveTo(points[0])
        for point in points[1:]:
            pen.lineTo(point)
        pen.closePath()
    return pen.glyph()


def synthetic_font(path: Path, characters: set[int], family: str) -> None:
    names = {cp: f"g{cp:04X}" for cp in sorted(characters)}
    builder = FontBuilder(1024, isTTF=True)
    builder.setupGlyphOrder([".notdef", *names.values()])
    builder.setupCharacterMap(names)
    # Tofu is a solid box, visibly and structurally distinct from every letter.
    glyphs = {".notdef": glyph_for([(0, 0), (0, 768), (640, 768), (640, 0)])}
    glyphs.update({name: glyph_for(outline(cp)) for cp, name in names.items()})
    builder.setupGlyf(glyphs)
    # TrueType glyph-set pens apply (lsb - xMin), so fixtures must describe
    # their side bearings consistently with the independent outline data.
    builder.setupHorizontalMetrics({name: (700, getattr(glyph, "xMin", 0))
                                    for name, glyph in glyphs.items()})
    builder.setupHorizontalHeader(ascent=900, descent=-124)
    builder.setupNameTable({"familyName": family, "styleName": "Bold",
                            "fullName": family + " Bold", "psName": family + "-Bold",
                            "uniqueFontIdentifier": family, "version": "Version 1.0"})
    builder.setupOS2(sTypoAscender=900, sTypoDescender=-124, usWinAscent=900, usWinDescent=124)
    builder.setupPost()
    builder.setupMaxp()
    builder.setupHead(unitsPerEm=1024, created=3786912000, modified=3786912000)
    builder.font.recalcTimestamp = False
    builder.save(path)


def points_in(font: TTFont, cp: int) -> list[tuple[int, int]]:
    glyph = font["glyf"][font.getBestCmap()[cp]]
    return list(glyph.coordinates) if glyph.numberOfContours else []


class FontZeroToolsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixture_temporary = tempfile.TemporaryDirectory(prefix="qtzpl-font0-fixture-")
        cls.fixture = Path(cls.fixture_temporary.name)
        sources = []
        for source_id, characters in (("latin", LATIN), ("cyrillic", RUSSIAN | {ord("H")})):
            font_path = cls.fixture / (source_id + ".ttf")
            synthetic_font(font_path, characters, "ReservedFixture" + source_id)
            license_path = cls.fixture / (source_id + "-OFL.txt")
            notice = f'Copyright 2026 {source_id} License Owner; Reserved Font Name "ReservedFixture{source_id}".'
            license_path.write_text(notice + "\nSIL OPEN FONT LICENSE Version 1.1\n", encoding="utf-8")
            sources.append({
                "id": source_id, "file": font_path.name, "sha256": digest(font_path),
                "license_spdx": "OFL-1.1", "license_file": license_path.name,
                "license_sha256": digest(license_path), "axes": [], "default_instance": {},
                "copyright": [f"Copyright 2025 {source_id} Outline Owner."],
                "license_notice": notice,
            })
        save_json(cls.fixture / "sources.json", {"schema_version": 1, "fonts": sources})
        save_json(cls.fixture / "atlas.json", {"characters": [f"U+{cp:04X}" for cp in sorted(ALL_CHARACTERS)]})
        with contextlib.redirect_stdout(io.StringIO()):
            generator.prepare(SimpleNamespace(sources=cls.fixture / "sources.json",
                                               manifest=cls.fixture / "atlas.json",
                                               output=cls.fixture / "prepared", weights=[600, 700, 800]))

    @classmethod
    def tearDownClass(cls):
        cls.fixture_temporary.cleanup()

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="qtzpl-font0-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        shutil.copytree(self.fixture, self.root, dirs_exist_ok=True)
        self.sources_path = self.root / "sources.json"
        self.candidates_path = self.root / "prepared/candidates.json"
        self.recipe_path = self.root / "recipe.json"
        self.output_path = self.root / "result.ttf"
        self.sources = json.loads(self.sources_path.read_text(encoding="utf-8"))
        self.candidates = json.loads(self.candidates_path.read_text(encoding="utf-8"))
        source_hashes = {source["id"]: source["sha256"] for source in self.sources["fonts"]}
        self.recipe = {"schemaVersion": 1,
                       "sourceManifestSha256": digest(self.sources_path),
                       "candidatesManifestSha256": digest(self.candidates_path), "glyphs": {
            f"U+{cp:04X}": {"donor": "latin-bold" if cp in LATIN else "cyrillic-bold",
                             "sourceId": "latin" if cp in LATIN else "cyrillic",
                             "sourceSha256": source_hashes["latin" if cp in LATIN else "cyrillic"],
                             "advanceEm": 0.7}
            for cp in sorted(ALL_CHARACTERS)
        }}

    def build(self, recipe=None):
        save_json(self.recipe_path, self.recipe if recipe is None else recipe)
        with contextlib.redirect_stdout(io.StringIO()):
            generator.build(SimpleNamespace(sources=self.sources_path, recipe=self.recipe_path,
                                             candidates=self.candidates_path, output=self.output_path))
        return self.output_path

    def one_glyph(self, cp=ord("A"), **changes):
        key = f"U+{cp:04X}"
        spec = copy.deepcopy(self.recipe["glyphs"][key])
        spec.update(changes)
        return {**self.recipe, "glyphs": {key: spec}}

    def test_extended_family_is_used_and_external_or_reserved_names_are_rejected(self):
        self.recipe["family"] = extension.FAMILY
        with TTFont(self.build()) as font:
            self.assertEqual(font["name"].getDebugName(1), extension.FAMILY)
            self.assertEqual(font["name"].getDebugName(6), "QtZplFontZeroExtended")
        for family in ("Roboto Condensed", "QtZpl", "QtZpl ", "QtZpl\nEvil", 17):
            self.recipe["family"] = family
            with self.subTest(family=family), self.assertRaisesRegex(ValueError, "QtZpl name"):
                self.build()
        self.sources["fonts"][0]["reserved_font_names"] = ["ReservedFixture"]
        save_json(self.sources_path, self.sources)
        self.candidates["sourceManifestSha256"] = digest(self.sources_path)
        save_json(self.candidates_path, self.candidates)
        self.recipe.update(sourceManifestSha256=digest(self.sources_path),
                           candidatesManifestSha256=digest(self.candidates_path),
                           family="QtZpl ReservedFixture Extended")
        with self.assertRaisesRegex(ValueError, "Reserved Font Name"):
            self.build()

    def extension_args(self, missing=(0x41, 0x0410)):
        self.recipe["oracleUnsupported"] = [f"U+{cp:04X}" for cp in missing]
        for text in self.recipe["oracleUnsupported"]:
            self.recipe["glyphs"].pop(text, None)
        save_json(self.recipe_path, self.recipe)
        return SimpleNamespace(recipe=self.recipe_path, candidates=self.candidates_path,
                               sources=self.sources_path, outputdir=self.root / "extended")

    def test_extension_is_deterministic_preserves_base_and_adds_real_donor_outlines(self):
        args = self.extension_args()
        with contextlib.redirect_stdout(io.StringIO()):
            result, report = extension.extend(args)
        first = {path.name: path.read_bytes() for path in args.outputdir.iterdir()}
        with contextlib.redirect_stdout(io.StringIO()):
            extension.extend(args)
        self.assertEqual(first, {path.name: path.read_bytes() for path in args.outputdir.iterdir()})
        self.assertEqual(len(result["glyphs"]), len(ALL_CHARACTERS))
        self.assertEqual(report["glyphCount"], len(ALL_CHARACTERS))
        self.assertFalse(report["pixelParityEvaluated"])
        self.assertEqual(report["extensionGlyphCount"], 2)
        self.assertEqual(report["outlineCount"], len(ALL_CHARACTERS) - 1)
        for text, spec in self.recipe["glyphs"].items():
            self.assertEqual(result["glyphs"][text], spec)
        with TTFont(args.outputdir / "QtZplFontZeroExtended.ttf") as font:
            for cp in (0x41, 0x0410):
                text = f"U+{cp:04X}"
                spec = result["glyphs"][text]
                self.assertEqual(spec["measurementStatus"], extension.MEASUREMENT_STATUS)
                self.assertEqual(spec["sourceCodepoint"], text)
                self.assertEqual(spec["advanceEm"], 1400 / 2048)
                self.assertEqual(spec["adjustment"], {"scaleX": 1.0, "scaleY": 1.0,
                                                       "offsetX": 0.0, "offsetY": 0.0})
                self.assertEqual(points_in(font, cp), [(2 * x, 2 * y) for x, y in outline(cp)])
                self.assertEqual(len(spec["sourceSha256"]), 64)
                self.assertEqual(len(spec["donorSha256"]), 64)

    def test_extension_priority_uses_preferred_donors_then_stable_identifier(self):
        donor = next(item for item in self.candidates["candidates"] if item["id"] == "latin-bold")
        for identifier in reversed(extension.PREFERRED_DONORS):
            self.candidates["candidates"].append({**donor, "id": identifier})
        save_json(self.candidates_path, self.candidates)
        self.recipe["candidatesManifestSha256"] = digest(self.candidates_path)
        args = self.extension_args((0x41,))
        result = extension.extend_recipe(args.recipe, args.candidates, args.sources)
        self.assertEqual(result["glyphs"]["U+0041"]["donor"], "roboto-condensed-wght700")
        self.assertEqual(result["extension"]["selectionOrder"][:4], list(extension.PREFERRED_DONORS))

    def test_extension_rejects_missing_and_blank_outlines_without_writing_partial_output(self):
        for cp in (0x1F600, 0x20):
            with self.subTest(codepoint=cp):
                args = self.extension_args((cp,))
                with self.assertRaisesRegex(ValueError, "No licensed nonempty outline"):
                    extension.extend(args)
                self.assertFalse(args.outputdir.exists())

    def test_extension_enforces_provenance_and_refuses_double_extension(self):
        args = self.extension_args()
        result = extension.extend_recipe(args.recipe, args.candidates, args.sources)
        save_json(args.recipe, result)
        with self.assertRaisesRegex(ValueError, "not an extension"):
            extension.extend_recipe(args.recipe, args.candidates, args.sources)
        save_json(args.recipe, self.recipe)
        self.recipe["sourceManifestSha256"] = "0" * 64
        save_json(args.recipe, self.recipe)
        with self.assertRaisesRegex(ValueError, "checksum"):
            extension.extend_recipe(args.recipe, args.candidates, args.sources)
        self.recipe["sourceManifestSha256"] = digest(args.sources)
        save_json(args.recipe, self.recipe)
        font_path = self.root / "prepared/latin-bold.ttf"
        data = font_path.read_bytes()
        font_path.write_bytes(data[:-1] + bytes([data[-1] ^ 1]))
        with self.assertRaisesRegex(ValueError, "checksum"):
            extension.extend_recipe(args.recipe, args.candidates, args.sources)

    def test_repeated_prepare_and_build_are_byte_identical(self):
        first = self.build().read_bytes()
        self.assertEqual(first, self.build().read_bytes())
        with TTFont(self.output_path) as font:
            self.assertEqual(font["head"].created, 3786912000)
            self.assertEqual(font["head"].modified, 3786912000)
        second = self.root / "prepared-again"
        with contextlib.redirect_stdout(io.StringIO()):
            generator.prepare(SimpleNamespace(sources=self.sources_path, manifest=self.root / "atlas.json",
                                               output=second, weights=[600, 700, 800]))
        for existing in (self.root / "prepared").iterdir():
            self.assertEqual(existing.read_bytes(), (second / existing.name).read_bytes(), existing.name)

    def test_full_ascii_and_russian_cmap_is_preserved(self):
        with TTFont(self.build()) as font:
            self.assertEqual(set(font.getBestCmap()), ALL_CHARACTERS)
            self.assertEqual(len(set(font.getBestCmap().values())), len(ALL_CHARACTERS))
            self.assertEqual(font["head"].unitsPerEm, 2048)

    def test_every_selected_letter_has_its_original_outline_not_tofu(self):
        with TTFont(self.build()) as font:
            for cp in sorted(ALL_CHARACTERS):
                self.assertEqual(points_in(font, cp), [(2 * x, 2 * y) for x, y in outline(cp)],
                                 f"U+{cp:04X} must keep the selected donor's outline")
                self.assertNotEqual(font.getBestCmap()[cp], ".notdef")

    def test_offsets_use_upward_y_and_keep_negative_bearings(self):
        for dy in (-0.125, 0.125):
            with self.subTest(offsetY=dy):
                recipe = self.one_glyph(adjustment={"scaleX": 0.5, "scaleY": 1.25,
                                                    "offsetX": -0.25, "offsetY": dy})
                expected = [(round(x - 512), round(2.5 * y + dy * 2048)) for x, y in outline(ord("A"))]
                with TTFont(self.build(recipe)) as font:
                    self.assertEqual(points_in(font, ord("A")), expected)
                    glyph = font["glyf"][font.getBestCmap()[ord("A")]]
                    self.assertLess(glyph.xMin, 0)
                    self.assertEqual(font["hmtx"]["uni0041"], (round(0.7 * 2048), glyph.xMin))
                    self.assertEqual(glyph.yMin, round(dy * 2048))

    def test_point_edit_changes_only_requested_point_and_updates_bounds(self):
        recipe = self.one_glyph(pointEdits=[{"point": 0, "dx": -512, "dy": -17}])
        expected = [(2 * x, 2 * y) for x, y in outline(ord("A"))]
        expected[0] = (expected[0][0] - 512, expected[0][1] - 17)
        with TTFont(self.build(recipe)) as font:
            self.assertEqual(points_in(font, ord("A")), expected)
            self.assertEqual(font["glyf"]["uni0041"].xMin, expected[0][0])
            self.assertEqual(font["hmtx"]["uni0041"][1], expected[0][0])
            self.assertEqual(font["glyf"]["uni0041"].yMin, -17)

    def test_out_of_range_point_edits_are_rejected(self):
        for point in (-1, 3, 500):
            with self.subTest(point=point), self.assertRaises(ValueError):
                self.build(self.one_glyph(pointEdits=[{"point": point, "dx": 1, "dy": 0}]))
        self.assertFalse(self.output_path.exists())

    def test_noninteger_point_edits_are_rejected(self):
        for point in (True, 1.5, "1"):
            with self.subTest(point=point), self.assertRaises(ValueError):
                self.build(self.one_glyph(pointEdits=[{"point": point, "dx": 1, "dy": 0}]))

    def test_blank_glyph_cannot_receive_an_outline_point_edit(self):
        with self.assertRaises(ValueError):
            self.build(self.one_glyph(0x20, pointEdits=[{"point": 0, "dx": 1, "dy": 0}]))

    def test_missing_donor_character_is_rejected_instead_of_using_tofu(self):
        with self.assertRaises((ValueError, KeyError)):
            self.build(self.one_glyph(0x0410, donor="latin-bold", sourceId="latin",
                                      sourceSha256=self.sources["fonts"][0]["sha256"]))
        self.assertFalse(self.output_path.exists())

    def test_source_and_prepared_font_checksums_are_enforced(self):
        for path in (self.root / "latin.ttf", self.root / "prepared/latin-bold.ttf"):
            with self.subTest(path=path.name):
                original = path.read_bytes()
                path.write_bytes(original[:-1] + bytes([original[-1] ^ 0x01]))
                try:
                    with self.assertRaisesRegex(ValueError, "checksum"):
                        self.build()
                finally:
                    path.write_bytes(original)
        self.assertFalse(self.output_path.exists())

    def test_source_license_checksum_is_enforced(self):
        path = self.root / "latin-OFL.txt"
        path.write_text("replaced license", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "checksum"):
            self.build()

    def test_candidate_manifest_and_per_donor_provenance_are_enforced(self):
        mutations = [
            lambda data: data.update(sourceManifestSha256="0" * 64),
            lambda data: data["candidates"][0].update(sourceSha256="0" * 64),
            lambda data: data["candidates"][0].update(sourceId="unidentified-font"),
        ]
        for index, mutate in enumerate(mutations):
            with self.subTest(mutation=index):
                candidates = copy.deepcopy(self.candidates)
                mutate(candidates)
                save_json(self.candidates_path, candidates)
                self.recipe["candidatesManifestSha256"] = digest(self.candidates_path)
                with self.assertRaises(ValueError):
                    self.build()
        self.assertFalse(self.output_path.exists())

    def test_recipe_manifest_and_per_glyph_provenance_are_enforced(self):
        mutations = [
            lambda data: data.update(sourceManifestSha256="0" * 64),
            lambda data: data.update(candidatesManifestSha256="0" * 64),
            lambda data: data["glyphs"]["U+0041"].update(sourceSha256="0" * 64),
            lambda data: data["glyphs"]["U+0041"].update(sourceId="unidentified-font"),
        ]
        for index, mutate in enumerate(mutations):
            with self.subTest(mutation=index):
                recipe = copy.deepcopy(self.recipe)
                mutate(recipe)
                with self.assertRaises(ValueError):
                    self.build(recipe)
        self.assertFalse(self.output_path.exists())

    def test_non_ofl_donor_is_rejected(self):
        self.sources["fonts"][0]["license_spdx"] = "GPL-2.0-only"
        save_json(self.sources_path, self.sources)
        with self.assertRaisesRegex(ValueError, "license"):
            self.build()

    def test_all_original_copyright_and_license_notices_survive(self):
        with TTFont(self.build()) as font:
            copyrights = "\n".join(record.toUnicode() for record in font["name"].names if record.nameID == 0)
            license_info = "\n".join(record.toUnicode() for record in font["name"].names if record.nameID in (13, 14))
            for source in self.sources["fonts"]:
                for notice in source["copyright"] + [source["license_notice"]]:
                    self.assertIn(notice, copyrights)
            self.assertIn("Open Font License", license_info)
        for candidate in self.candidates["candidates"]:
            with TTFont(self.root / "prepared" / candidate["file"]) as font:
                notices = "\n".join(record.toUnicode() for record in font["name"].names if record.nameID == 0)
                source = next(source for source in self.sources["fonts"] if source["id"] == candidate["sourceId"])
                self.assertIn(source["license_notice"], notices)
                names = [record.toUnicode() for record in font["name"].names if record.nameID in (1, 3, 4, 6, 16)]
                self.assertTrue(all("ReservedFixture" not in name for name in names))

    def test_nonfinite_or_negative_transforms_are_rejected(self):
        for adjustment in ({"scaleX": 0}, {"scaleY": -1}, {"scaleX": 6},
                           {"offsetY": float("nan")}, {"offsetX": float("inf")}):
            with self.subTest(adjustment=adjustment), self.assertRaises(ValueError):
                self.build(self.one_glyph(adjustment=adjustment))

    def test_negative_or_nonfinite_advances_are_rejected(self):
        for advance in (-0.1, float("nan"), float("inf")):
            with self.subTest(advance=advance), self.assertRaises(ValueError):
                self.build(self.one_glyph(advanceEm=advance))

    def test_explicit_alias_keeps_original_codepoint_provenance(self):
        aliases = self.root / "semantic-mappings.json"
        save_json(aliases, {"schemaVersion": 1, "aliases": {
            "U+0041": {"sourceCodepoint": "U+0048", "reason": "Synthetic explicit mapping"}}})
        output = self.root / "aliased"
        with contextlib.redirect_stdout(io.StringIO()):
            generator.prepare(SimpleNamespace(sources=self.sources_path, manifest=self.root / "atlas.json",
                                               semantic_mappings=aliases, output=output, weights=[700]))
        candidates = json.loads((output / "candidates.json").read_text(encoding="utf-8"))
        self.assertEqual(candidates["semanticMappingsSha256"], digest(aliases))
        for candidate in candidates["candidates"]:
            self.assertEqual(candidate["glyphAliases"]["U+0041"], "U+0048")
            with TTFont(output / candidate["file"]) as font:
                self.assertEqual(points_in(font, ord("A")), [(2 * x, 2 * y) for x, y in outline(ord("H"))])


class AuthoredGeometryTests(unittest.TestCase):
    def test_ballot_cross_is_its_own_solid_outline_not_a_multiplication_alias(self):
        with TTFont(io.BytesIO(geometric.font_bytes())) as font:
            cmap = font.getBestCmap()
            cross = font["glyf"][cmap[0x2717]]
            self.assertNotEqual(cmap[0x2717], cmap[0x00D7])
            self.assertEqual(cross.numberOfContours, 1)
            self.assertEqual(len(cross.coordinates), 12)
            self.assertEqual((cross.xMin, cross.yMin, cross.xMax, cross.yMax), (64, 0, 1280, 1536))
            self.assertEqual(font["hmtx"][cmap[0x2717]], (1344, 64))
            self.assertNotEqual(points_in(font, 0x2717), points_in(font, 0x00D7))

    def test_generation_is_byte_identical_to_the_committed_original(self):
        first = geometric.font_bytes()
        self.assertEqual(first, geometric.font_bytes())
        self.assertEqual(first, geometric.FONT_PATH.read_bytes())

    def test_cap_geometry_spaces_and_descenders_have_expected_units(self):
        with TTFont(io.BytesIO(geometric.font_bytes())) as font:
            cmap = font.getBestCmap()
            self.assertEqual(font["head"].unitsPerEm, 2048)
            self.assertEqual(font["glyf"][cmap[ord("H")]].yMax, 1536)
            self.assertEqual(font["glyf"][cmap[ord(".")]].yMax, 256)
            self.assertEqual(font["glyf"][cmap[ord(".")]].yMin, 0)
            for cp in (0x20, 0xA0):
                self.assertEqual(font["glyf"][cmap[cp]].numberOfContours, 0)
                self.assertEqual(font["hmtx"][cmap[cp]], (512, 0))
            for character in "HITLEF\u041d\u0422\u0415\u0413\u041f\u0428":
                self.assertIn(ord(character), cmap)
            self.assertLess(font["glyf"][cmap[ord(",")]].yMin, 0)
            self.assertLess(font["glyf"][cmap[ord("_")]].yMax, 0)
            for cp, name in cmap.items():
                glyph = font["glyf"][name]
                if glyph.numberOfContours:
                    self.assertEqual(font["hmtx"][name][1], glyph.xMin, f"U+{cp:04X}")
                    self.assertLessEqual(glyph.yMax, font["OS/2"].usWinAscent)
                    self.assertGreaterEqual(glyph.yMin, -font["OS/2"].usWinDescent)

    def test_mirrored_symbols_keep_solid_contour_orientation(self):
        with TTFont(io.BytesIO(geometric.font_bytes())) as font:
            for left, right, extent in (("/", "\\", 1088), ("<", ">", 1120), ("[", "]", 704)):
                with self.subTest(left=left):
                    expected = {(extent - x, y) for x, y in points_in(font, ord(left))}
                    self.assertEqual(set(points_in(font, ord(right))), expected)
            for name in font.getGlyphOrder():
                glyph = font["glyf"][name]
                start = 0
                for end in glyph.endPtsOfContours if glyph.numberOfContours else []:
                    points = list(glyph.coordinates[start:end + 1])
                    area = sum(a[0] * b[1] - b[0] * a[1]
                               for a, b in zip(points, points[1:] + points[:1]))
                    self.assertLess(area, 0, name)
                    start = end + 1

    def test_missing_authored_asset_is_never_downloaded(self):
        manifest = json.loads(geometric.MANIFEST_PATH.read_text(encoding="utf-8"))
        authored = next(entry for entry in manifest["fonts"] if entry["id"] == "qtzpl-geometric")
        self.assertEqual(authored["origin"], "authored")
        self.assertEqual(authored["generator_sha256"], digest(Path(geometric.__file__)))
        self.assertNotIn("source_url", authored)
        self.assertNotIn("license_url", authored)
        manifest["fonts"] = [authored]
        with tempfile.TemporaryDirectory(prefix="qtzpl-authored-cache-") as folder:
            path = Path(folder) / "sources.json"
            save_json(path, manifest)
            error = io.StringIO()
            with patch.object(sys, "argv", ["fetch_font0_sources.py", "--manifest", str(path)]), \
                    patch.object(source_fetcher, "urlopen", side_effect=AssertionError("unexpected download")), \
                    contextlib.redirect_stderr(error):
                self.assertEqual(source_fetcher.main(), 1)
            self.assertIn("Regenerate locally", error.getvalue())
            self.assertFalse((path.parent / "sources").exists())

    def test_authored_generator_checksum_cannot_be_bypassed(self):
        manifest = json.loads(geometric.MANIFEST_PATH.read_text(encoding="utf-8"))
        authored = next(entry for entry in manifest["fonts"] if entry["id"] == "qtzpl-geometric")
        authored["generator_sha256"] = "0" * 64
        manifest["fonts"] = [authored]
        with tempfile.TemporaryDirectory(prefix="qtzpl-authored-integrity-") as folder:
            path = Path(folder) / "sources.json"
            save_json(path, manifest)
            with self.assertRaisesRegex(ValueError, "generator checksum"):
                source_fetcher.load_manifest(path)


class ResearchSelectionTests(unittest.TestCase):
    """Test orchestration and cache identity without running a native probe."""

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="qtzpl-font0-research-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.probe = self.root / "probe.exe"
        self.probe.write_bytes(b"controlled probe identity")
        self.sources = self.root / "sources.json"
        save_json(self.sources, {"schema_version": 1, "fonts": []})
        self.candidates_path = self.root / "candidates.json"
        self.candidates = []
        # Intentionally reversed order: tie selection must not follow input or
        # asynchronous completion order.
        for candidate_id in ("z-donor", "a-donor"):
            font_path = self.root / (candidate_id + ".ttf")
            synthetic_font(font_path, {ord("A"), ord("H")}, candidate_id)
            self.candidates.append({"id": candidate_id, "file": font_path.name,
                                    "sha256": digest(font_path), "sourceId": candidate_id,
                                    "sourceSha256": digest(font_path), "unitsPerEm": 1024})
        save_json(self.candidates_path, {"schemaVersion": 1,
                                         "sourceManifestSha256": digest(self.sources),
                                         "candidates": self.candidates})
        self.manifest = self.root / "manifest.json"
        save_json(self.manifest, {"width": 200, "height": 200, "characters": ["U+0041"],
                                 "unsupportedCharacters": [],
                                 "cases": [{"name": "training", "role": "train"},
                                           {"name": "holdout", "role": "validation"}]})
        for name in ("training", "holdout"):
            (self.root / (name + "-labelary-bitonal.png")).write_bytes(name.encode())
        self.metrics = self.root / "metrics.json"
        save_json(self.metrics, {"glyphs": {"U+0041": {"advanceEm": 0.7}}})
        self.output = self.root / "output"
        self.calls = []

    def fake_probe(self, args, font, output, extra=()):
        fitting = "--fit" in extra
        self.calls.append((font.name, fitting))
        output.mkdir(parents=True, exist_ok=True)
        report = {"summary": {}, "fontSha256": digest(font), "cases": []}
        if fitting:
            fitted = {"fontSha256": digest(font), "noHinting": True,
                      "global": {"scaleX": 1, "scaleY": 1, "offsetX": 0, "offsetY": 0},
                      "glyphs": {"U+0041": {"scaleX": 1, "scaleY": 1, "offsetX": 0,
                                             "offsetY": 0, "missingGlyph": False,
                                             "loss": 0.25, "differentPixels": 2,
                                             "unionPixels": 8, "samples": 3}}}
            save_json(output / "fitted-adjustments.json", fitted)
        save_json(output / "report.json", report)
        return report

    def run_research(self, *extra):
        command = ["research_font0.py", "--probe", str(self.probe),
                   "--sources", str(self.sources), "--manifest", str(self.manifest),
                   "--candidates", str(self.candidates_path), "--metrics", str(self.metrics),
                   "--output", str(self.output), "--jobs", "2", *extra]
        with patch.object(sys, "argv", command), patch.object(research, "run_probe", self.fake_probe), \
                patch.object(research, "build", lambda args: args.output.write_bytes(b"controlled assembled font")), \
                contextlib.redirect_stdout(io.StringIO()):
            research.main()
        return json.loads((self.output / "glyph-recipe.json").read_text(encoding="utf-8"))

    def test_equal_loss_selects_stable_id_and_select_limits_candidates(self):
        self.assertEqual(self.run_research()["glyphs"]["U+0041"]["donor"], "a-donor")
        self.assertEqual(self.run_research("--select", "z-donor")["glyphs"]["U+0041"]["donor"], "z-donor")

    def test_cache_reuses_fits_but_training_and_probe_changes_invalidate(self):
        self.run_research()
        self.assertEqual(sum(fitting for _, fitting in self.calls), 2)
        self.run_research()
        self.assertEqual(sum(fitting for _, fitting in self.calls), 2)
        (self.root / "holdout-labelary-bitonal.png").write_bytes(b"new independent holdout")
        self.run_research()
        self.assertEqual(sum(fitting for _, fitting in self.calls), 2)
        (self.root / "training-labelary-bitonal.png").write_bytes(b"new training raster")
        self.run_research()
        self.assertEqual(sum(fitting for _, fitting in self.calls), 4)
        self.probe.write_bytes(b"new probe identity")
        self.run_research()
        self.assertEqual(sum(fitting for _, fitting in self.calls), 6)

    def test_missing_cached_report_triggers_refit(self):
        self.run_research()
        (self.output / "fits/a-donor/report.json").unlink()
        self.run_research()
        self.assertEqual(sum(fitting for _, fitting in self.calls), 3)

    def test_scores_keep_roles_and_use_pixel_weighted_iou(self):
        def glyph(different, union, *, unsupported=False, missing=False):
            return {"oracleUnsupported": unsupported, "missingGlyph": missing,
                    "differentPixels": different, "unionPixels": union}
        report = {"cases": [
            {"role": "train", "glyphs": [glyph(1, 2), glyph(1, 8), glyph(900, 900, unsupported=True)]},
            {"role": "validation", "glyphs": [glyph(0, 4), glyph(0, 0, missing=True)]},
        ]}
        score = research.grouped_scores(report)
        self.assertAlmostEqual(score["train"]["iou"], 0.8)
        self.assertEqual(score["train"]["comparisons"], 2)
        self.assertEqual(score["validation"]["comparisons"], 2)
        self.assertEqual(score["validation"]["exact"], 1)
        self.assertEqual(score["validation"]["missing"], 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
