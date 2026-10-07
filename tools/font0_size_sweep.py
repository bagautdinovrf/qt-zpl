"""Acquire the offline Font 0 validation census at every square size 4..512.

Network access is explicit (--fetch). Original Labelary Bitonal PNGs and their
ZPL are checkpointed per page; a separate small manifest describes each size.
Nothing from this validation sweep is used to fit glyph contours.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime, timezone
from email.utils import parsedate_to_datetime
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import time
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

from fetch_font_goldens import REPERTOIRE, cp_name, fields_to_zpl, load_json, missing_from_warnings, png_rows, save_json

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUT = ROOT / "tests/golden/font0-size-sweep"
SOURCE = ROOT / "tests/golden/font-glyphs/manifest.json"
MAX_EDGE = 8000
MAX_PIXELS = 64 * 1024 * 1024
DPI = {8: 203, 12: 304, 24: 609}


def ink_bounds(rows, cell):
    """Find black bounds using row-sized bit operations, without pixel lists."""
    x, y, width, height = (cell[k] for k in ("x", "y", "width", "height"))
    first, last = x // 8, (x + width + 7) // 8
    tail, mask = last * 8 - x - width, (1 << width) - 1
    left, top, right, bottom = x + width, y + height, -1, -1
    for row_y in range(y, y + height):
        black = (~int.from_bytes(rows[row_y][first:last], "big") >> tail) & mask
        if black:
            left = min(left, x + width - black.bit_length())
            right = max(right, x + width - 1 - ((black & -black).bit_length() - 1))
            top, bottom = min(top, row_y), row_y
    return None if right < 0 else (left, top, right + 1, bottom + 1)


def layout_bounds(source):
    """Training measurements affect packing only; no raster enters a font."""
    result = {}
    for case in source["cases"]:
        if case.get("role", "train") != "train" or case["fontHeight"] != 70 or case["fontWidth"] != 100:
            continue
        path = SOURCE.parent / (case["name"] + "-labelary-bitonal.png")
        size = (case.get("width", source["width"]), case.get("height", source["height"]))
        rows = png_rows(path.read_bytes(), size)
        for cell in case["cells"]:
            bounds = ink_bounds(rows, cell)
            if bounds:
                origin = cell.get("anchorX", cell["x"])
                result[cell["codepoint"]] = ((bounds[0] - origin) / 100, (bounds[2] - origin) / 100)
    return result


def dimensions(size, bounds):
    margin = math.ceil(size * .1) + 12
    height = math.ceil(size * 1.35) + 40
    cells = []
    for cp in REPERTOIRE:
        left, right = bounds.get(cp_name(cp), (0, .8))
        # Legacy training crops did not retain all negative bearings (notably
        # U+2215). Reserve an independent quarter-em on the left as well.
        bearing = math.ceil(max(.25, -left) * size)
        width = max(32, bearing + math.ceil(max(0, right) * size) + margin * 2)
        cells.append((cp, width, height, bearing + margin, size + 20))
    return cells


def pack(cells, edge):
    pages, fields = [], []
    x, y, row_height, used_width = 20, 20, 0, 0
    for cp, width, height, ax, ay in cells:
        if x + width + 20 > edge:
            x, y, row_height = 20, y + row_height, 0
        if y + height + 20 > edge:
            pages.append((fields, used_width + 20, y))
            fields, x, y, row_height, used_width = [], 20, 20, 0, 0
        fields.append(dict(codepoint=cp_name(cp), x=x, y=y, width=width, height=height,
                           anchorX=x + ax, anchorY=y + ay, text=chr(cp)))
        x += width
        row_height = max(row_height, height)
        used_width = max(used_width, x)
    if fields:
        pages.append((fields, used_width + 20, y + row_height + 20))
    return pages


def size_cases(size, bounds):
    cells = dimensions(size, bounds)
    # Prefer few requests, then small image buffers. Candidate widths include
    # smaller near-square pages for the many small sizes that fit one request.
    alternatives = []
    for edge in (1600, 2436, 3045, 4000, 5000, 6000, 7000, MAX_EDGE):
        if any(max(c[1], c[2]) + 40 > edge for c in cells):
            continue
        pages = pack(cells, edge)
        alternatives.append(((len(pages), sum(w * h for _, w, h in pages), edge), pages))
    pages = min(alternatives, key=lambda item: item[0])[1]
    result = []
    for page, (fields, width, height) in enumerate(pages, 1):
        # Byte-aligned widths simplify independent bitonal checks.
        width = math.ceil(width / 8) * 8
        if width > MAX_EDGE or height > MAX_EDGE or width * height > MAX_PIXELS:
            raise ValueError("Atlas packing exceeded the bounded image dimensions")
        dpmm = next(d for d in (8, 12, 24) if max(width, height) / DPI[d] < 15)
        for field in fields:
            field.update(fontHeight=size, fontWidth=size, anchor="FT", orientation="N")
        zpl, saved_cells = fields_to_zpl(fields, width)
        zpl = zpl.replace(f"^LL{width}", f"^LL{height}", 1)
        for cell in saved_cells:
            cell["sourceDataOffset"] += len(str(height)) - len(str(width))
            for key in ("fontHeight", "fontWidth", "anchor", "orientation"):
                cell.pop(key)
        name = f"font0-h{size:03d}-w{size:03d}-ft-n-page{page}"
        result.append((dict(name=name, role="validation", font="0", layoutVersion=2, fontHeight=size, fontWidth=size,
                            anchor="FT", orientation="N", width=width, height=height, dpmm=dpmm,
                            cells=saved_cells), zpl + "\n"))
    return result


def planned_cases(size, bounds, previous, directory):
    # Completed original evidence remains immutable when later atlas packing
    # gains a wider safety margin. Its manifest is the geometry contract.
    previous_cells = [cell["codepoint"] for case in previous.get("cases", []) for cell in case["cells"]]
    if len(previous_cells) == len(REPERTOIRE) and set(previous_cells) == {cp_name(cp) for cp in REPERTOIRE}:
        return [(dict(case), (directory / (case["name"] + ".zpl")).read_text(encoding="utf-8"))
                for case in previous["cases"]]
    return size_cases(size, bounds)


def validate_page(case, data, unsupported):
    rows = png_rows(data, (case["width"], case["height"]))
    stride_bits = len(rows[0]) * 8
    pixel_mask = ((1 << case["width"]) - 1) << (stride_bits - case["width"])
    bands = {}
    for cell in case["cells"]:
        key = (cell["y"], cell["y"] + cell["height"])
        mask = ((1 << cell["width"]) - 1) << (stride_bits - cell["x"] - cell["width"])
        bands[key] = bands.get(key, 0) | mask
    next_row = 0
    for (top, bottom), allowed in sorted(bands.items()):
        if top < next_row:
            raise ValueError("Overlapping atlas row bands")
        for y in range(next_row, bottom):
            black = ~int.from_bytes(rows[y], "big") & pixel_mask
            if black & ~(allowed if y >= top else 0):
                raise ValueError(f"Unexpected ink outside every declared glyph cell at y={y}")
        next_row = bottom
    for y in range(next_row, case["height"]):
        if ~int.from_bytes(rows[y], "big") & pixel_mask:
            raise ValueError(f"Unexpected ink below every declared glyph cell at y={y}")
    blank = []
    for cell in case["cells"]:
        ink = ink_bounds(rows, cell)
        # Explicit anchors separate the field position from its already padded
        # crop. Only legacy cells without anchors need the native ten-dot halo.
        if "anchorX" not in cell or "anchorY" not in cell:
            halo = dict(cell, x=max(0, cell["x"] - 10), y=max(0, cell["y"] - 10),
                        width=cell["width"] + min(10, cell["x"]),
                        height=cell["height"] + min(10, cell["y"]))
            if ink_bounds(rows, halo) != ink:
                raise ValueError(f"Native probe crop includes neighboring ink: {cell['codepoint']}")
        if ink is None:
            blank.append(cell["codepoint"])
        if ink and cell["codepoint"] in unsupported:
            raise ValueError(f"Previously unsupported glyph now has ink: {cell['codepoint']}")
        if ink and (ink[0] <= cell["x"] or ink[1] <= cell["y"]
                    or ink[2] >= cell["x"] + cell["width"] or ink[3] >= cell["y"] + cell["height"]):
            raise ValueError(f"Glyph touches the cell boundary: {cell['codepoint']}")
    return blank


def cached(case, zpl, directory, previous, deep=False):
    prior = next((item for item in previous.get("cases", []) if item["name"] == case["name"]), None)
    source, raster = directory / (case["name"] + ".zpl"), directory / (case["name"] + "-labelary-bitonal.png")
    if not prior or not source.exists() or not raster.exists():
        return False
    if prior.get("validationError"):
        raise ValueError(f"Cached oracle evidence needs investigation: {case['name']}: {prior['validationError']}")
    if any(prior.get(key) != value for key, value in case.items()):
        raise ValueError(f"Cached atlas geometry changed: {case['name']}")
    source_bytes, data = source.read_bytes(), raster.read_bytes()
    if source_bytes != zpl.encode() or hashlib.sha256(source_bytes).hexdigest() != prior.get("zplSha256"):
        raise ValueError(f"Cached ZPL changed: {source}")
    if hashlib.sha256(data).hexdigest() != prior.get("pngSha256"):
        raise ValueError(f"Cached PNG changed: {raster}")
    if deep:
        validate_page(prior, data, set(previous["unsupportedCharacters"]))
    return True


class SweepFetcher:
    def __init__(self, budget, interval, index, index_path):
        self.budget, self.interval, self.index, self.index_path = budget, interval, index, index_path
        self.last_request = 0.0

    def fetch(self, case, zpl):
        dpi = DPI[case["dpmm"]]
        # Quarter-dot slack avoids decimal rounding to the preceding integer.
        inches = f"{(case['width'] + .25) / dpi:.9f}x{(case['height'] + .25) / dpi:.9f}"
        url = f"https://api.labelary.com/v1/printers/{case['dpmm']}dpmm/labels/{inches}/0/"
        request = Request(url, data=zpl.encode(), headers={"Accept": "image/png", "X-Quality": "Bitonal",
                          "X-Linter": "On", "Content-Type": "application/x-www-form-urlencoded"})
        for attempt in range(4):
            if self.index.get("requestAttempts", 0) >= self.budget:
                raise RuntimeError("Cumulative request budget exhausted; the cache is resumable")
            time.sleep(max(0, self.interval - (time.monotonic() - self.last_request)))
            self.index["requestAttempts"] = self.index.get("requestAttempts", 0) + 1
            self.index["lastRequestUtc"] = datetime.now(timezone.utc).isoformat()
            save_json(self.index_path, self.index)
            self.last_request = time.monotonic()
            try:
                with urlopen(request, timeout=55) as response:
                    chunks, digest, total = [], hashlib.sha256(), 0
                    while chunk := response.read(65536):
                        total += len(chunk)
                        if total > 12 * 1024 * 1024:
                            raise ValueError("Unexpected oversized response")
                        digest.update(chunk)
                        chunks.append(chunk)
                    return b"".join(chunks), dict(oracleWarnings=response.headers.get("X-Warnings", ""),
                        oracleResponseDate=response.headers.get("Date", ""), requestUrl=url, pngSha256=digest.hexdigest())
            except HTTPError as error:
                message = error.read(4096).decode("utf-8", errors="replace")
                if error.code != 429:
                    raise RuntimeError(f"Labelary HTTP {error.code}: {message}") from error
                retry = error.headers.get("Retry-After", "")
                if retry.isdecimal():
                    delay = int(retry)
                elif retry:
                    delay = max(0, (parsedate_to_datetime(retry) - datetime.now(timezone.utc)).total_seconds())
                else:
                    delay = 3 * (attempt + 1)
                if "daily" in message.lower() or "quota" in message.lower() or delay > 60 or attempt == 3:
                    raise RuntimeError(f"Labelary limit reached; stop and preserve cache. Retry-After={retry}: {message}") from error
                print(f"HTTP 429: honoring Retry-After, waiting {max(1, delay):g}s", flush=True)
                time.sleep(max(1, delay))
            except (TimeoutError, URLError) as error:
                if attempt == 3:
                    raise RuntimeError(f"Network failed after bounded retries: {error}") from error
                print(f"Transient network error; bounded retry {attempt + 1}/3: {error}", flush=True)
                time.sleep(3 * (attempt + 1))
        raise AssertionError("unreachable")


def probe_environment(probe, qt_bin):
    environment = os.environ.copy()
    paths = [str(probe.parent)]
    if qt_bin:
        paths.append(str(qt_bin.resolve()))
    elif os.name == "nt":
        for parent in probe.parents:
            cache = parent / "CMakeCache.txt"
            if not cache.exists():
                continue
            for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
                if line.startswith("Qt6_DIR:") and "=" in line:
                    directory = Path(line.split("=", 1)[1]).parents[2] / "bin"
                    if directory.is_dir():
                        paths.append(str(directory))
            break
    environment["PATH"] = os.pathsep.join(paths + [environment.get("PATH", "")])
    return environment


def score_size(size, corpus, destination, probe, font, fingerprints, environment):
    manifest_path = corpus / f"h{size:03d}/manifest.json"
    manifest_bytes = manifest_path.read_bytes()
    manifest = json.loads(manifest_bytes)
    golden_hashes = []
    for case in manifest["cases"]:
        if case.get("validationError"):
            raise ValueError(f"Unvalidated oracle page: {case['name']}")
        for suffix, key in ((".zpl", "zplSha256"), ("-labelary-bitonal.png", "pngSha256")):
            path = manifest_path.parent / (case["name"] + suffix)
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
            if digest != case[key]:
                raise ValueError(f"Oracle hash mismatch: {path}")
            golden_hashes.append(digest)
    key = dict(**fingerprints, manifestSha256=hashlib.sha256(manifest_bytes).hexdigest(),
               goldenHashes=golden_hashes, arguments=["--no-hinting", "--role", "validation", "--no-atlas-images"])
    key_digest = hashlib.sha256(json.dumps(key, sort_keys=True).encode()).hexdigest()
    output = destination / f"h{size:03d}"
    report_path, cache_path = output / "report.json", output / "score-cache.json"
    prior = load_json(cache_path)
    reused = (prior.get("keySha256") == key_digest and report_path.exists()
              and hashlib.sha256(report_path.read_bytes()).hexdigest() == prior.get("reportSha256"))
    if not reused:
        output.mkdir(parents=True, exist_ok=True)
        command = [str(probe), "--font", str(font), "--manifest", str(manifest_path), "--output-dir", str(output),
                   "--no-hinting", "--role", "validation", "--no-atlas-images"]
        result = subprocess.run(command, cwd=ROOT, env=environment, capture_output=True, text=True,
                                encoding="utf-8", errors="replace", timeout=600)
        (output / "probe.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        if result.returncode or not report_path.exists():
            raise RuntimeError(f"Native probe failed for h{size}: exit {result.returncode}; {result.stderr[-1000:]}")
        save_json(cache_path, dict(keySha256=key_digest, inputs=key,
                                  reportSha256=hashlib.sha256(report_path.read_bytes()).hexdigest()))
    report = load_json(report_path)
    if (report.get("fontSha256") != fingerprints["fontSha256"]
        or hashlib.sha256(font.read_bytes()).hexdigest() != fingerprints["fontSha256"]
        or hashlib.sha256(probe.read_bytes()).hexdigest() != fingerprints["probeSha256"]):
        raise ValueError("Font or native probe changed during the scoring run; use immutable inputs")
    cells = {cell["codepoint"]: cell for case in manifest["cases"] for cell in case["cells"]}
    for case in report["cases"]:
        for glyph in case["glyphs"]:
            bitmap, origin = glyph.get("bitmap", {}), glyph.get("origin", [])
            if not bitmap.get("width") or not bitmap.get("height"):
                continue
            cell = cells[glyph["codepoint"]]
            left, top = origin[0] + bitmap["left"], origin[1] - bitmap["top"]
            if (left < cell["x"] or top < cell["y"]
                or left + bitmap["width"] > cell["x"] + cell["width"]
                or top + bitmap["height"] > cell["y"] + cell["height"]):
                raise ValueError(f"Candidate glyph exceeds its isolated crop: h{size} {glyph['codepoint']}")
        if sum(glyph["differentPixels"] for glyph in case["glyphs"]) != case["differentPixels"]:
            raise ValueError(f"Cell scores do not account for the complete atlas: {case['case']}")
    glyphs = [glyph for case in report["cases"] for glyph in case["glyphs"]]
    supported = [glyph for glyph in glyphs if not glyph["oracleUnsupported"]]
    if len(glyphs) != len(REPERTOIRE) or {glyph["codepoint"] for glyph in glyphs} != {cp_name(cp) for cp in REPERTOIRE}:
        raise ValueError(f"Native report has an incomplete repertoire at size {size}")
    intersection = sum(glyph["intersectionPixels"] for glyph in supported)
    union = sum(glyph["unionPixels"] for glyph in supported)
    return dict(size=size, supported=len(supported), unsupported=len(glyphs) - len(supported),
                missing=sum(glyph["missingGlyph"] for glyph in supported),
                exact=sum(glyph["differentPixels"] == 0 and not glyph["missingGlyph"] for glyph in supported),
                errorGlyphs=sum(glyph["differentPixels"] != 0 or glyph["missingGlyph"] for glyph in supported),
                differentPixels=sum(glyph["differentPixels"] for glyph in supported), intersectionPixels=intersection,
                unionPixels=union, iou=intersection / union if union else 1, reused=reused,
                manifestSha256=key["manifestSha256"], report=f"h{size:03d}/report.json")


def score_sweep(args, parser):
    if not args.font or not args.probe:
        parser.error("--score requires --font and --probe")
    font, probe = args.font.resolve(), args.probe.resolve()
    fingerprints = dict(fontSha256=hashlib.sha256(font.read_bytes()).hexdigest(),
                        probeSha256=hashlib.sha256(probe.read_bytes()).hexdigest())
    index = load_json(args.output / "index.json")
    sizes = list(range(args.first, args.last + 1))
    for size in sizes:
        record = index.get("sizes", {}).get(str(size), {})
        path = args.output / f"h{size:03d}/manifest.json"
        if not record.get("complete") or not path.exists():
            parser.error(f"Oracle size {size} is not complete yet; scores can be resumed after fetching it")
        if hashlib.sha256(path.read_bytes()).hexdigest() != record["manifestSha256"]:
            raise ValueError(f"Index/manifest checksum mismatch at size {size}")
    destination = args.score_output.resolve()
    destination.mkdir(parents=True, exist_ok=True)
    environment = probe_environment(probe, args.qt_bin)
    results = []

    def checkpoint():
        ordered = sorted(results, key=lambda item: item["size"])
        totals = {key: sum(row[key] for row in ordered) for key in
                  ("supported", "unsupported", "missing", "exact", "errorGlyphs", "differentPixels", "intersectionPixels", "unionPixels")}
        totals["iou"] = totals["intersectionPixels"] / totals["unionPixels"] if totals["unionPixels"] else 1
        totals["meanSizeIou"] = sum(row["iou"] for row in ordered) / len(ordered) if ordered else None
        report = dict(schemaVersion=1, **fingerprints, font=str(font), probe=str(probe), role="validation",
                      requestedRange=[args.first, args.last], complete=len(ordered) == len(sizes),
                      completedSizes=len(ordered), totals=totals, sizes=ordered,
                      rasterizer="FreeType mono unhinted", networkRequests=0)
        save_json(destination / "summary.json", report)
        return report

    with ThreadPoolExecutor(max_workers=args.workers) as executor:
        futures = [executor.submit(score_size, size, args.output.resolve(), destination, probe, font, fingerprints, environment)
                   for size in sizes]
        try:
            for future in as_completed(futures):
                row = future.result()
                results.append(row)
                checkpoint()
                print(f"Scored {len(results)}/{len(sizes)}: h{row['size']:03d}, IoU={row['iou']:.6f}, "
                      f"exact={row['exact']}/{row['supported']}, missing={row['missing']}, cached={row['reused']}", flush=True)
        except BaseException:
            for future in futures:
                future.cancel()
            checkpoint()
            raise
    report = checkpoint()
    print(json.dumps(report["totals"], indent=2), flush=True)
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--first", type=int, default=4)
    parser.add_argument("--last", type=int, default=512)
    parser.add_argument("--dry-run", action="store_true", help="Plan only; the default unless --fetch/--check is supplied")
    parser.add_argument("--fetch", action="store_true")
    parser.add_argument("--check", action="store_true", help="Verify all selected cached hashes, bitonal rasters and cell bounds offline")
    parser.add_argument("--score", action="store_true", help="Compare all selected sizes offline using the native probe; resume by input hashes")
    parser.add_argument("--probe", type=Path, help="Built qtzpl_font0_probe executable; never built by this tool")
    parser.add_argument("--font", type=Path, help="Candidate font for offline scoring")
    parser.add_argument("--score-output", type=Path, default=ROOT / "build_font0_research/size-sweep-scores")
    parser.add_argument("--workers", type=int, default=3, choices=range(1, 4))
    parser.add_argument("--qt-bin", type=Path, help="Optional Qt runtime bin directory (normally read from the probe's CMake cache)")
    parser.add_argument("--max-requests", type=int, default=2500, help="Cumulative budget including retries and resumed runs")
    parser.add_argument("--interval", type=float, default=.6, help="Minimum request interval, at least .5 seconds")
    args = parser.parse_args()
    if not 4 <= args.first <= args.last <= 512 or args.interval < .5 or args.max_requests < 0:
        parser.error("Require 4 <= first <= last <= 512, interval >= .5 and a nonnegative budget")
    if sum((args.fetch, args.check, args.dry_run, args.score)) > 1:
        parser.error("Select only one of --fetch, --check, --dry-run, --score")
    if args.score:
        return score_sweep(args, parser)
    index_path = args.output / "index.json"
    index = load_json(index_path)
    if args.check:
        # An offline check depends only on this frozen corpus, never on a
        # subsequently revised set of training fixtures or packing bounds.
        bounds, unsupported = {}, set(index.get("unsupportedCharacters", []))
    else:
        source = load_json(SOURCE)
        bounds, unsupported = layout_bounds(source), set(source["unsupportedCharacters"])
    plans, page_count, pending, uncompressed, zpl_bytes = [], 0, 0, 0, 0
    for size in range(args.first, args.last + 1):
        directory = args.output / f"h{size:03d}"
        previous = load_json(directory / "manifest.json")
        if args.check:
            recorded = index.get("sizes", {}).get(str(size), {})
            if recorded.get("manifestSha256") != hashlib.sha256((directory / "manifest.json").read_bytes()).hexdigest():
                raise ValueError(f"Index/manifest checksum mismatch for size {size}")
        cases = planned_cases(size, bounds, previous, directory)
        for case, zpl in cases:
            exists = cached(case, zpl, directory, previous, args.check)
            if args.check and not exists:
                raise ValueError(f"Missing requested cached case: {case['name']}")
            page_count += 1
            pending += not exists
            uncompressed += (case["width"] + 7) // 8 * case["height"]
            zpl_bytes += len(zpl.encode())
        plans.append((size, directory, previous, cases))
        if args.check and (size - args.first + 1) % 25 == 0:
            print(f"Verified {size - args.first + 1}/{args.last - args.first + 1} sizes through h{size:03d}", flush=True)
    summary = dict(sizes=args.last - args.first + 1, range=[args.first, args.last], codepoints=len(REPERTOIRE),
                   glyphComparisons=(args.last - args.first + 1) * len(REPERTOIRE), pages=page_count,
                   pendingRequests=pending, cumulativeRequests=index.get("requestAttempts", 0),
                   uncompressedBitonalMiB=round(uncompressed / 1024**2, 2), zplMiB=round(zpl_bytes / 1024**2, 2),
                   estimatedCompressedPngMiB=[round(uncompressed / 1024**2 * factor, 1) for factor in (.01, .05)],
                   minimumRateLimitSeconds=round(pending * args.interval, 1),
                   maximumPixels=MAX_PIXELS, maximumPageEdge=MAX_EDGE)
    print(json.dumps(summary, indent=2), flush=True)
    if args.check:
        print(f"Offline validation passed for {page_count} original PNG/ZPL pairs", flush=True)
        return 0
    if not args.fetch:
        return 0
    if pending + index.get("requestAttempts", 0) > args.max_requests:
        parser.error("The pending plan exceeds the cumulative request budget")
    args.output.mkdir(parents=True, exist_ok=True)
    lock = args.output / ".fetch.lock"
    with lock.open("x", encoding="utf-8") as handle:
        handle.write(f"pid={os.getpid()}\n")
    try:
        index.update(schemaVersion=1, source="Labelary API", quality="Bitonal", role="validation", font="0",
                     squareSizeRange=[4, 512], characters=[cp_name(cp) for cp in REPERTOIRE],
                     unsupportedCharacters=sorted(unsupported), generationTool="tools/font0_size_sweep.py",
                     packingSourceSha256=hashlib.sha256(SOURCE.read_bytes()).hexdigest(),
                     notes="Independent all-size validation; never consumed by glyph fitting. Original PNG response bytes retained.")
        index.setdefault("sizes", {})
        fetcher = SweepFetcher(args.max_requests, args.interval, index, index_path)
        completed_pages = page_count - pending
        for size, directory, previous, cases in plans:
            directory.mkdir(parents=True, exist_ok=True)
            manifest = previous or dict(schemaVersion=2, source="Labelary API", quality="Bitonal", role="validation",
                width=MAX_EDGE, height=MAX_EDGE, fontHeight=size, fontWidth=size,
                characters=[cp_name(cp) for cp in REPERTOIRE], unsupportedCharacters=sorted(unsupported), cases=[])
            for case, zpl in cases:
                if cached(case, zpl, directory, manifest):
                    continue
                data, evidence = fetcher.fetch(case, zpl)
                warnings = missing_from_warnings(evidence["oracleWarnings"], case["cells"])
                case.update(evidence, oracleReportedUnsupportedCharacters=warnings,
                            unsupportedCharacters=sorted({c["codepoint"] for c in case["cells"]} & unsupported),
                            fetchedAtUtc=datetime.now(timezone.utc).isoformat(),
                            zplSha256=hashlib.sha256(zpl.encode()).hexdigest())
                validation_error = None
                try:
                    case["blankCharacters"] = validate_page(case, data, unsupported | set(warnings))
                except ValueError as error:
                    # Preserve unexpected original evidence for diagnosis, but
                    # never call it a complete or reusable validation page.
                    validation_error = error
                    case["validationError"] = str(error)
                for suffix, content in ((".zpl", zpl.encode()), ("-labelary-bitonal.png", data)):
                    target = directory / (case["name"] + suffix)
                    temporary = target.with_suffix(target.suffix + ".tmp")
                    temporary.write_bytes(content)
                    temporary.replace(target)
                manifest["cases"].append(case)
                save_json(directory / "manifest.json", manifest)
                if validation_error:
                    raise RuntimeError(f"Original failing oracle evidence saved for {case['name']}: {validation_error}") from validation_error
                completed_pages += 1
                print(f"Saved {completed_pages}/{page_count}: {case['name']}, {len(case['cells'])} glyphs, {len(data)} bytes", flush=True)
            manifest_path = directory / "manifest.json"
            index["sizes"][str(size)] = dict(manifest=f"h{size:03d}/manifest.json", pages=len(cases),
                glyphs=sum(len(c["cells"]) for c, _ in cases), complete=True,
                manifestSha256=hashlib.sha256(manifest_path.read_bytes()).hexdigest())
            index["completeSizes"] = len(index["sizes"])
            index["complete"] = set(index["sizes"]) == {str(n) for n in range(4, 513)}
            save_json(index_path, index)
        print(f"Complete selected range: {page_count} original PNG/ZPL pairs; cumulative requests {index.get('requestAttempts', 0)}", flush=True)
    finally:
        lock.unlink()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
