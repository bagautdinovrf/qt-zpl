"""Manual Labelary Font 0 census. Network is opt-in, cached and request-budgeted.

No installed font, fontTools or Pillow is needed. Normal tests only use saved
original bitonal PNGs; they never invoke the network or this maintenance tool.
"""
from __future__ import annotations
import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import struct
import time
import uuid
from urllib.error import HTTPError
from urllib.request import Request, urlopen
import zlib

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "tests/golden/font-glyphs"
# Explicit census independent of the font under test. Soft hyphen U+00AD is a
# formatting control; keep the legacy negative tests, including ligatures.
BASE_EXTRA = "ıŁłŒœŠšŸŽžƒˆˇˉ˘˙˚˛˜˝;ΩμЁЄІЇёєіїҐґ–—‘’‚“”„†‡•…‰‹›⁄€₽№™←↑→↓↔↕∆∏∑−∕∙√∞≈≠≤≥✓✗ﬁﬂ"
COMMON_EXTRA = "‒―‖‗‛‟‣․‥‧′″‴‵‶‷‼‽⁂⁎⁑⁒⁓⁕₩₪₴₹₿℅ℓ℮Ω∂∅∈∉∩∪∫∼≡"
REPERTOIRE = sorted(set(range(0x20, 0x7F)) | (set(range(0xA0, 0x100)) - {0xAD})
                    | set(range(0x410, 0x450)) | set(map(ord, BASE_EXTRA + COMMON_EXTRA)))
HOLDOUTS = ((9, 9, "FT", "N"), (17, 17, "FT", "N"), (47, 47, "FT", "N"), (96, 96, "FT", "N"),
            (23, 31, "FO", "N"), (31, 13, "FO", "R"), (64, 23, "FT", "I"), (11, 17, "FO", "B"))
TRAINING = tuple((height, width, "FT", "N") for height, width in
                 ((12, 12), (16, 16), (25, 25), (30, 30), (40, 40), (26, 33), (60, 30), (70, 100)))


def cp_name(cp):
    return f"U+{cp:04X}"


def load_json(path):
    return json.loads(path.read_text(encoding="utf-8")) if path.exists() else {}


def save_json(path, value):
    temporary = path.with_suffix(path.suffix + "." + uuid.uuid4().hex + ".tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    for attempt in range(4):
        try:
            temporary.replace(path)
            return
        except PermissionError:
            if attempt == 3:
                raise
            time.sleep(.1 * (attempt + 1))


def png_rows(data, size):
    """Validate/decode original 1-bit grayscale PNG, including row filters."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("Labelary did not return PNG")
    offset, compressed, header = 8, bytearray(), None
    while offset < len(data):
        length = struct.unpack_from(">I", data, offset)[0]
        kind = data[offset + 4:offset + 8]
        chunk = data[offset + 8:offset + 8 + length]
        crc = struct.unpack_from(">I", data, offset + 8 + length)[0]
        if zlib.crc32(kind + chunk) & 0xFFFFFFFF != crc:
            raise ValueError("PNG checksum mismatch")
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", chunk)
        elif kind == b"IDAT":
            compressed.extend(chunk)
        offset += length + 12
        if kind == b"IEND":
            break
    if header != (*size, 1, 0, 0, 0, 0):
        raise ValueError(f"Expected original 1-bit grayscale PNG {size}, got {header}")
    stride = (size[0] + 7) // 8
    raw = zlib.decompress(compressed)
    if len(raw) != (stride + 1) * size[1]:
        raise ValueError("Unexpected PNG raster length")
    rows, previous = [], bytes(stride)
    for y in range(size[1]):
        start = y * (stride + 1)
        filter_type = raw[start]
        row = bytearray(raw[start + 1:start + stride + 1])
        if filter_type:
            for x in range(stride):
                left, above, corner = row[x - 1] if x else 0, previous[x], previous[x - 1] if x else 0
                if filter_type == 1:
                    prediction = left
                elif filter_type == 2:
                    prediction = above
                elif filter_type == 3:
                    prediction = (left + above) // 2
                elif filter_type == 4:
                    p = left + above - corner
                    distances = (abs(p - left), abs(p - above), abs(p - corner))
                    prediction = (left, above, corner)[distances.index(min(distances))]
                else:
                    raise ValueError(f"Unknown PNG filter {filter_type}")
                row[x] = (row[x] + prediction) & 255
        rows.append(bytes(row))
        previous = row
    return rows


def ink_points(rows, cell):
    return [(x, y) for y in range(cell["y"], cell["y"] + cell["height"])
            for x in range(cell["x"], cell["x"] + cell["width"])
            if not rows[y][x // 8] & (0x80 >> (x % 8))]


def fields_to_zpl(fields, dots):
    zpl, cells = f"^XA^PW{dots}^LL{dots}^CI28", []
    for field in fields:
        cell = dict(field)
        zpl += (f'^{cell["anchor"]}{cell["anchorX"]},{cell["anchorY"]}'
                f'^A0{cell["orientation"]},{cell["fontHeight"]},{cell["fontWidth"]}^FH_^FD')
        cell["sourceDataOffset"] = len(zpl.encode("utf-8"))
        # ASCII-only UTF-8 hex escapes make linter byte offsets unambiguous.
        text = "".join(f"_{byte:02X}" for byte in cell.pop("text").encode("utf-8"))
        text = cell.pop("rawFieldData", text)
        cell["sourceDataLength"] = len(text.encode("utf-8"))
        zpl += text + "^FS"
        cells.append(cell)
    return zpl + "^XZ", cells


def atlas_cases(profiles, repertoire=REPERTOIRE, role="validation", family="font0-validation", isolated=False):
    result = []
    for height, width, anchor, orientation in profiles:
        # The oracle limits the warning header to 20 entries. Reverse the second
        # extras census so its warnings cover the other end of the repertoire.
        alphabet = list(reversed(repertoire)) if family == "font0-extra" and height == 70 else repertoire
        margin_x = math.ceil(.7 * width) + 12 if isolated else 20
        margin_y = math.ceil(.7 * height) + 12 if isolated else 20
        normal_width = (math.ceil(1.6 * width) + 2 * margin_x if isolated else
                        (math.ceil(1.8 * width) if width >= 80 else width) + 40)
        normal_height = math.ceil(1.35 * height) + 2 * margin_y
        cw, ch = ((normal_height, normal_width) if orientation in "RB" else (normal_width, normal_height))
        cw, ch = max(64, cw), max(64, ch)
        dots = 2436 if (2396 // cw) * (2396 // ch) >= len(repertoire) else 3045
        columns, rows = (dots - 40) // cw, (dots - 40) // ch
        if not columns or not rows:
            raise ValueError("Requested size cannot fit the bounded Labelary atlas")
        capacity = columns * rows
        pages = math.ceil(len(repertoire) / capacity)
        for page in range(pages):
            fields = []
            for i, cp in enumerate(alphabet[page * capacity:(page + 1) * capacity]):
                x, y, descent = 20 + i % columns * cw, 20 + i // columns * ch, math.ceil(.35 * height)
                if anchor == "FT":
                    positions = {"N": (margin_x, height + margin_y), "R": (descent + margin_y, margin_x),
                                 "I": (cw - margin_x, descent + margin_y), "B": (height + margin_y, ch - margin_x)}
                else:
                    # ^FO names the top-left of the rotated field, unlike ^FT.
                    positions = {key: ((margin_y, margin_x) if key in "RB" else (margin_x, margin_y)) for key in "NRIB"}
                ax, ay = positions[orientation]
                fields.append(dict(codepoint=cp_name(cp), x=x, y=y, width=cw, height=ch,
                                   anchorX=x + ax, anchorY=y + ay, text=chr(cp), anchor=anchor,
                                   orientation=orientation, fontHeight=height, fontWidth=width))
            zpl, cells = fields_to_zpl(fields, dots)
            name = f"{family}-h{height}-w{width}-{anchor.lower()}-{orientation.lower()}"
            if page:
                name += f"-page{page + 1}"
            result.append((dict(name=name, font="0", role=role, fontHeight=height, fontWidth=width,
                                anchor=anchor, orientation=orientation, width=dots, height=dots, cells=cells,
                                isolationChecked=isolated, isolationHaloDots=10 if isolated else 0), zpl))
    return result


def validation_cases(profiles):
    result = []
    for profile in profiles:
        repaired = profile == (47, 47, "FT", "N")
        result.extend(atlas_cases([profile], family="font0-validation-isolated" if repaired else "font0-validation",
                                  isolated=repaired))
    return result


def metric_cases():
    result, capacity = [], 239
    for page in range(math.ceil(len(REPERTOIRE) / capacity)):
        fields = []
        for i, cp in enumerate([None] + REPERTOIRE[page * capacity:(page + 1) * capacity]):
            x, y = 20 + i % 6 * 500, 20 + i // 6 * 72
            fields.append(dict(codepoint="marker" if cp is None else cp_name(cp), x=x, y=y, width=500, height=72,
                               anchorX=x + 16, anchorY=y + 46, text="|" if cp is None else chr(cp) * 3 + "|",
                               repeat=0 if cp is None else 3, marker="|", anchor="FT", orientation="N",
                               fontHeight=30, fontWidth=100))
        zpl, cells = fields_to_zpl(fields, 3045)
        result.append((dict(name=f"font0-metrics-h30-w100-page{page + 1}", role="metrics", font="0",
                            fontHeight=30, fontWidth=100, width=3045, height=3045,
                            anchor="FT", orientation="N", cells=cells), zpl))
    return result


def semantic_cases():
    fields = []
    for i, (cp, spelling, data) in enumerate(((0x5C, "hex", "_5C"), (0x5C, "literal", "\\"),
                                             (0xA2, "hex", "_C2_A2"), (0xA2, "literal", "¢"),
                                             (0x2F, "literal", "/"), (0xA5, "literal", "¥"),
                                             (0xA4, "literal", "¤"))):
        x, y = 40 + (i % 2) * 500, 40 + (i // 2) * 160
        fields.append(dict(codepoint=cp_name(cp), spelling=spelling, x=x, y=y, width=300, height=150,
                           anchorX=x+24, anchorY=y+100, text=chr(cp), rawFieldData=data,
                           anchor="FT", orientation="N", fontHeight=70, fontWidth=100))
    zpl, cells = fields_to_zpl(fields, 2436)
    return [(dict(name="font0-semantics-ci28-backslash-cent-h70-w100", role="semantics", font="0",
                  fontHeight=70, fontWidth=100, width=2436, height=2436,
                  anchor="FT", orientation="N", cells=cells), zpl)]


def clamp_cases():
    fields = []
    for row, (height, width) in enumerate(((4, 30), (10, 30), (30, 4), (30, 10), (4, 4), (10, 10))):
        for column, cp in enumerate(map(ord, "HIAa")):
            x, y = 40 + column * 200, 40 + row * 160
            fields.append(dict(codepoint=cp_name(cp), x=x, y=y, width=180, height=140,
                               anchorX=x+60, anchorY=y+80, text=chr(cp), anchor="FT", orientation="N",
                               fontHeight=height, fontWidth=width))
    zpl, cells = fields_to_zpl(fields, 2436)
    return [(dict(name="font0-semantics-minimum-size-clamp", role="semantics", font="0", fontHeight=30,
                  fontWidth=30, width=2436, height=2436, anchor="FT", orientation="N", cells=cells,
                  isolationChecked=True, isolationHaloDots=10), zpl)]


def clamp_observations(manifest):
    observations = []
    for case in manifest.get("cases", []):
        rows = check_case(case, manifest)
        masks = [{(x-c["anchorX"], y-c["anchorY"]) for x, y in ink_points(rows, c)} for c in case["cells"]]
        for first in (0, 8, 16):
            for index in range(4):
                a, b = first + index, first + index + 4
                observations.append(dict(sourceCase=case["name"], codepoint=case["cells"][a]["codepoint"],
                                         firstCell=a, secondCell=b, differentPixels=len(masks[a] ^ masks[b])))
    return observations


def missing_from_warnings(warnings, cells):
    if not warnings:
        return []
    parts, missing = warnings.split("|"), set()
    if len(parts) % 5:
        raise ValueError(f"Unrecognized Labelary warning header: {warnings}")
    for i in range(0, len(parts), 5):
        offset, length, command, parameter, message = parts[i:i + 5]
        if command != "^FD" or "field font cannot display" not in message:
            raise ValueError(f"Unexpected Labelary warning: {parts[i:i + 5]}")
        matches = [c for c in cells if c["sourceDataOffset"] <= int(offset)
                   < c["sourceDataOffset"] + c["sourceDataLength"]]
        if len(matches) != 1:
            raise ValueError(f"Cannot locate warning {offset} in a glyph cell")
        missing.add(matches[0]["codepoint"])
    return sorted(missing)


class Fetcher:
    def __init__(self, budget, interval):
        self.budget, self.interval, self.used, self.last_request = budget, interval, 0, 0.0

    def fetch(self, case, zpl):
        inches = 12 if case["width"] == 2436 else 15
        request = Request(f"https://api.labelary.com/v1/printers/8dpmm/labels/{inches}x{inches}/0/",
                          data=zpl.encode(), headers={"Accept": "image/png", "X-Quality": "Bitonal",
                          "X-Linter": "On", "Content-Type": "application/x-www-form-urlencoded"})
        for attempt in range(3):
            if self.used >= self.budget:
                raise RuntimeError(f"Request budget {self.budget} exhausted; cached results preserved")
            time.sleep(max(0, self.interval - (time.monotonic() - self.last_request)))
            self.used += 1
            self.last_request = time.monotonic()
            try:
                with urlopen(request, timeout=45) as response:
                    return response.read(), response.headers.get("X-Warnings", ""), response.headers.get("Date", "")
            except HTTPError as error:
                if error.code != 429 or attempt == 2:
                    raise
                delay = error.headers.get("Retry-After", "")
                time.sleep(min(30, max(3 * (attempt + 1), int(delay) if delay.isdecimal() else 0)))
        raise AssertionError("unreachable")


def check_case(case, manifest, require_census=False):
    name = case["name"]
    source = (OUT / f"{name}.zpl").read_bytes()
    data = (OUT / f"{name}-labelary-bitonal.png").read_bytes()
    for key, content in (("zplSha256", source), ("pngSha256", data)):
        if key in case and hashlib.sha256(content).hexdigest() != case[key]:
            raise ValueError(f"{name}: {key} mismatch")
    size = (case.get("width", manifest["width"]), case.get("height", manifest["height"]))
    rows = png_rows(data, size)
    isolated_ink = 0
    for cell in case["cells"]:
        if (cell["x"] < 0 or cell["y"] < 0 or cell["x"] + cell["width"] > size[0]
                or cell["y"] + cell["height"] > size[1]):
            raise ValueError(f"{name}: cell outside the image: {cell['codepoint']}")
        if case.get("role") != "metrics" and "sourceDataOffset" in cell:
            points = ink_points(rows, cell)
            isolated_ink += len(points)
            if cell["codepoint"] in case.get("unsupportedCharacters", []) and points:
                raise ValueError(f"{name}: unsupported cell contains ink: {cell['codepoint']}")
            if any(x in (cell["x"], cell["x"] + cell["width"] - 1)
                   or y in (cell["y"], cell["y"] + cell["height"] - 1) for x, y in points):
                raise ValueError(f"{name}: glyph touches cell edge: {cell['codepoint']}")
            halo = case.get("isolationHaloDots", 0)
            if halo:
                # Native probe expands the diagnostic crop to the left/top by
                # ten dots. Require an empty halo on every side, so neither a
                # negative bearing nor a neighbour can cross crop boundaries.
                if any(x < cell["x"] + halo or x >= cell["x"] + cell["width"] - halo
                       or y < cell["y"] + halo or y >= cell["y"] + cell["height"] - halo for x, y in points):
                    raise ValueError(f"{name}: glyph enters required {halo}-dot isolation halo: {cell['codepoint']}")
            if halo or case.get("role") in ("train", "validation"):
                overscan = max(10, halo)
                expanded = dict(x=max(0, cell["x"] - overscan), y=max(0, cell["y"] - overscan),
                                width=cell["width"] + min(overscan, cell["x"]),
                                height=cell["height"] + min(overscan, cell["y"]))
                if len(ink_points(rows, expanded)) != len(points):
                    raise ValueError(f"{name}: neighbour contaminates expanded crop: {cell['codepoint']}")
            if (require_census and case["fontHeight"] >= 17 and case.get("orientation", "N") == "N"
                    and not points and cell["codepoint"] not in ("U+0020", "U+00A0")
                    and cell["codepoint"] not in case.get("unsupportedCharacters", [])):
                raise ValueError(f"{name}: blank glyph lacks explicit oracle warning evidence: {cell['codepoint']}")
    if case.get("role") in ("train", "validation"):
        whole_bytes, tail = divmod(size[0], 8)
        total_ink = sum(sum(8 - byte.bit_count() for byte in row[:whole_bytes])
                        + (tail - (row[whole_bytes] >> (8 - tail)).bit_count() if tail else 0) for row in rows)
        if total_ink != isolated_ink:
            raise ValueError(f"{name}: ink outside isolated cells or overlapping cells ({total_ink} != {isolated_ink})")
    return rows


def update_manifest(manifest):
    for case in manifest["cases"]:
        case.setdefault("role", "train")
        case.setdefault("oracleReportedUnsupportedCharacters", case.get("unsupportedCharacters", []))
    missing = {cp for case in manifest["cases"] for cp in case["oracleReportedUnsupportedCharacters"]}
    for case in manifest["cases"]:
        case["unsupportedCharacters"] = sorted({c["codepoint"] for c in case["cells"]} & missing)
    manifest.update(schemaVersion=2,
                    characters=sorted({c["codepoint"] for case in manifest["cases"] for c in case["cells"]}),
                    unsupportedCharacters=sorted(missing),
                    repertoire="Explicit ASCII, printable Latin-1, Russian including Yo, legacy extended letters, and listed common symbols",
                    repertoireCodepoints=[cp_name(cp) for cp in REPERTOIRE], generationTool="tools/fetch_font_goldens.py")


def measure_advances(manifest):
    measurements = {}
    for case in manifest.get("cases", []):
        rows = check_case(case, manifest)
        reference = case["cells"][0]
        marker = ink_points(rows, reference)
        left, top, right = min(x for x, y in marker), min(y for x, y in marker), max(x for x, y in marker)
        template = {(x - left, y - top) for x, y in marker}
        marker_offset = left - reference["anchorX"]
        for cell in case["cells"][1:]:
            cp = cell["codepoint"]
            if cp in case.get("unsupportedCharacters", []):
                measurements[cp] = {"supported": False, "sourceCase": case["name"]}
                continue
            ink = set(ink_points(rows, cell))
            marker_left = max(x for x, y in ink) - (right - left)
            marker_top = cell["anchorY"] + top - reference["anchorY"]
            actual_marker = {(x - marker_left, y - marker_top) for x, y in ink if x >= marker_left}
            if actual_marker != template:
                raise ValueError(f"{case['name']}: cannot isolate final marker for {cp}")
            distance = marker_left - cell["anchorX"] - marker_offset
            if distance < 0:
                raise ValueError(f"Negative advance for {cp}")
            measurements[cp] = dict(supported=True, advanceEm=distance / (cell["repeat"] * case["fontWidth"]),
                                    uncertaintyEm=1 / (cell["repeat"] * case["fontWidth"]),
                                    advanceDots=distance / cell["repeat"], markerDistanceDots=distance,
                                    sourceCase=case["name"], repeat=cell["repeat"],
                                    fontHeight=case["fontHeight"], fontWidth=case["fontWidth"])
    return dict(source="Labelary Bitonal marker positions", units="nominal ^A font-width units, not font-file em units",
                method="Three repeated glyphs followed by |; subtract standalone | bearing. Estimated advances, not exact font metrics.",
                glyphs=measurements)


def semantic_observations(manifest):
    observations = []
    for case in manifest.get("cases", []):
        rows = check_case(case, manifest)
        masks = [{(x-c["anchorX"], y-c["anchorY"]) for x, y in ink_points(rows, c)} for c in case["cells"]]
        for a, b in ((0, 1), (2, 3), (0, 2)):
            observations.append(dict(sourceCase=case["name"], firstCell=a, secondCell=b,
                                     differentPixels=len(masks[a] ^ masks[b]),
                                     note="Observed oracle behavior only; U+005C and U+00A2 are different Unicode characters."))
    return observations


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fetch", action="store_true", help="fetch only uncached cases; otherwise show a dry plan")
    parser.add_argument("--check", action="store_true", help="validate committed original inputs/PNGs/metrics offline")
    parser.add_argument("--metrics", action="store_true", help="also fetch independent repeated-glyph advance rulers")
    parser.add_argument("--metrics-only", action="store_true", help="plan/fetch only advance rulers")
    parser.add_argument("--extras-only", action="store_true", help="plan/fetch only two training atlases for new common symbols")
    parser.add_argument("--semantics-only", action="store_true", help="plan/fetch literal/hex backslash and currency probes")
    parser.add_argument("--clamp-only", action="store_true", help="plan/fetch independent height/width minimum-size probes")
    parser.add_argument("--clean-training", action="store_true", help="stage isolated full-census training and repaired h47 holdout, then atomically promote")
    parser.add_argument("--profiles", help="comma-separated heights or HEIGHTxWIDTH pairs, e.g. 11,13,23,31,64,26x33")
    parser.add_argument("--size-range", help="inclusive FIRST:LAST[:STEP] sweep, within 1..512 dots")
    parser.add_argument("--anchors", default="FT", help="comma-separated FO,FT for custom profiles")
    parser.add_argument("--orientations", default="N", help="comma-separated N,R,I,B for custom profiles")
    parser.add_argument("--max-requests", type=int, default=12, help="hard budget including retries (default 12)")
    parser.add_argument("--interval", type=float, default=1.5, help="minimum seconds between requests; at least 1.0")
    args = parser.parse_args()
    if args.max_requests < 0 or args.interval < 1:
        parser.error("--max-requests must be nonnegative and --interval at least 1 second")
    manifest_path, metric_path = OUT / "manifest.json", OUT / "metrics-manifest.json"
    manifest, metrics = load_json(manifest_path), load_json(metric_path)
    semantic_path = OUT / "semantics-manifest.json"
    semantics = load_json(semantic_path)
    clamp_path = OUT / "clamp-manifest.json"
    clamps = load_json(clamp_path)
    if args.check:
        total = 0
        for collection in (manifest, metrics, semantics, clamps):
            for case in collection.get("cases", []):
                check_case(case, collection, require_census=True)
                total += 1
                print(f"Checked {case['name']}", flush=True)
        required = {cp_name(cp) for cp in range(0x410, 0x450)} | {"U+0401", "U+0451"}
        if not required <= set(manifest["characters"]):
            raise ValueError("Incomplete Russian alphabet")
        if metrics and measure_advances(metrics) != load_json(OUT / "metrics.json"):
            raise ValueError("metrics.json does not match original ruler rasters")
        if semantics and semantics.get("observations") != semantic_observations(semantics):
            raise ValueError("semantic observations do not match original probe rasters")
        if clamps and clamps.get("observations") != clamp_observations(clamps):
            raise ValueError("clamp observations do not match original probe rasters")
        print(f"Offline validation passed: {total} original bitonal PNG/ZPL pairs")
        return 0
    profiles, sizes = [], []
    if args.profiles:
        for value in args.profiles.split(","):
            pair = value.lower().split("x")
            if len(pair) not in (1, 2):
                parser.error("--profiles expects HEIGHT or HEIGHTxWIDTH")
            sizes.append((int(pair[0]), int(pair[-1])))
    if args.size_range:
        values = list(map(int, args.size_range.split(":")))
        if len(values) not in (2, 3) or values[0] > values[1] or (len(values) == 3 and values[2] <= 0):
            parser.error("--size-range must be FIRST:LAST[:POSITIVE_STEP]")
        if not 1 <= values[0] <= values[1] <= 512:
            parser.error("--size-range must stay within 1..512 dots")
        sizes.extend((n, n) for n in range(values[0], values[1] + 1, values[2] if len(values) == 3 else 1))
    for height, width in sizes:
        if not (1 <= height <= 512 and 1 <= width <= 512):
            parser.error("font sizes must be within 1..512 dots for the bounded atlas")
        for anchor in args.anchors.upper().split(","):
            for orientation in args.orientations.upper().split(","):
                if anchor not in ("FO", "FT") or orientation not in ("N", "R", "I", "B"):
                    parser.error("anchors must be FO/FT and orientations N/R/I/B")
                profiles.append((height, width, anchor, orientation))
    plans = [] if args.metrics_only or args.extras_only or args.semantics_only or args.clamp_only or args.clean_training else [(manifest_path, manifest, c, s) for c, s in validation_cases(list(dict.fromkeys(profiles or HOLDOUTS)))]
    if not args.metrics_only and not args.semantics_only and not args.clamp_only and not args.clean_training and not profiles:
        legacy = {int(c["codepoint"][2:], 16) for case in manifest["cases"]
                  if case["name"].startswith("font0-h") or case["name"].startswith("font0-special-") for c in case["cells"]}
        extra = sorted(set(REPERTOIRE) - legacy)
        plans = [(manifest_path, manifest, c, s) for c, s in atlas_cases(
            [(30, 30, "FT", "N"), (70, 100, "FT", "N")], extra, "train", "font0-extra")] + plans
    if args.metrics or args.metrics_only:
        metrics = metrics or dict(source="Labelary API", quality="Bitonal", dpmm=8, width=3045, height=3045, cases=[])
        plans += [(metric_path, metrics, c, s) for c, s in metric_cases()]
    if args.semantics_only:
        semantics = semantics or dict(source="Labelary API", quality="Bitonal", dpmm=8, width=2436, height=2436, cases=[])
        plans += [(semantic_path, semantics, c, s) for c, s in semantic_cases()]
    if args.clamp_only:
        clamps = clamps or dict(source="Labelary API", quality="Bitonal", dpmm=8, width=2436, height=2436, cases=[])
        plans += [(clamp_path, clamps, c, s) for c, s in clamp_cases()]
    if args.clean_training:
        if profiles or args.metrics or args.metrics_only or args.extras_only or args.semantics_only or args.clamp_only:
            parser.error("--clean-training is a separate bounded maintenance operation")
        training_path = OUT / "training-manifest.json"
        training = load_json(training_path) or dict(source="Labelary API", quality="Bitonal", dpmm=8,
                                                  width=3045, height=3045, cases=[])
        generated = atlas_cases(TRAINING, role="train", family="font0-training", isolated=True)
        generated += validation_cases([(47, 47, "FT", "N")])
        plans = [(training_path, training, c, s) for c, s in generated]
    pending = []
    for path, collection, case, zpl in plans:
        prior = next((c for c in collection.get("cases", []) if c["name"] == case["name"]), None)
        source, raster = OUT / f'{case["name"]}.zpl', OUT / f'{case["name"]}-labelary-bitonal.png'
        cached = prior is not None and source.exists() and raster.exists() and source.read_text(encoding="utf-8").strip() == zpl
        print(f'{"Cached" if cached else "Fetch "} {case["name"]}: {len(case["cells"])} cells, {case["width"]}x{case["height"]}')
        if cached:
            check_case(prior, collection)
        else:
            pending.append((path, collection, case, zpl))
    print(f"{len(REPERTOIRE)} explicit code points; {len(pending)} HTTP requests needed before retries")
    if not args.fetch:
        return 0
    if len(pending) > args.max_requests:
        parser.error("plan exceeds --max-requests; select fewer sizes or explicitly raise the budget")
    fetcher = Fetcher(args.max_requests, args.interval)
    for path, collection, case, zpl in pending:
        data, warnings, response_date = fetcher.fetch(case, zpl)
        png_rows(data, (case["width"], case["height"]))
        case["unsupportedCharacters"] = missing_from_warnings(warnings, case["cells"])
        case["oracleReportedUnsupportedCharacters"] = list(case["unsupportedCharacters"])
        case.update(oracleWarnings=warnings, oracleResponseDate=response_date,
                    fetchedAtUtc=datetime.now(timezone.utc).isoformat(), pngSha256=hashlib.sha256(data).hexdigest(),
                    zplSha256=hashlib.sha256((zpl + "\n").encode()).hexdigest())
        # Preserve the actual response, never threshold or rerasterize it.
        (OUT / f'{case["name"]}.zpl').write_bytes((zpl + "\n").encode())
        (OUT / f'{case["name"]}-labelary-bitonal.png').write_bytes(data)
        collection["cases"] = [c for c in collection.get("cases", []) if c["name"] != case["name"]] + [case]
        if path == manifest_path:
            update_manifest(collection)
        else:
            known_missing = set(manifest.get("unsupportedCharacters", []))
            case["unsupportedCharacters"] = sorted({c["codepoint"] for c in case["cells"]} & known_missing
                                                     | set(case["unsupportedCharacters"]))
        save_json(path, collection)  # checkpoint every successful request
        check_case(case, collection)
        print(f'Saved {case["name"]}: {len(case["unsupportedCharacters"])} unsupported code points', flush=True)
    if args.clean_training:
        expected = {case["name"] for _, _, case, _ in plans}
        staged = [case for case in training["cases"] if case["name"] in expected]
        if {case["name"] for case in staged} != expected:
            raise ValueError("Incomplete isolated training stage; main manifest not changed")
        for case in staged:
            check_case(case, training, require_census=True)
        for case in manifest["cases"]:
            if (case.get("role") == "train" or case["name"] == "font0-validation-h47-w47-ft-n") and case["name"] not in expected:
                case["role"] = "legacy-validation"
                case["legacyReason"] = "Superseded by isolated full-census atlas; historical crops may clip bearings or include neighbours"
        manifest["cases"] = [case for case in manifest["cases"] if case["name"] not in expected] + staged
    update_manifest(manifest)
    save_json(manifest_path, manifest)
    if metrics:
        missing = set(manifest["unsupportedCharacters"])
        for case in metrics["cases"]:
            case["unsupportedCharacters"] = sorted({c["codepoint"] for c in case["cells"]} & missing)
        save_json(metric_path, metrics)
        save_json(OUT / "metrics.json", measure_advances(metrics))
    if semantics:
        semantics["observations"] = semantic_observations(semantics)
        save_json(semantic_path, semantics)
    if clamps:
        clamps["observations"] = clamp_observations(clamps)
        save_json(clamp_path, clamps)
    print(f"Complete: {fetcher.used} requests; existing oracle images retained unchanged")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
