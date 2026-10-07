"""Offline donor search, per-glyph selection, assembly and independent scoring.

Call generate_font0_native.py prepare first. This script makes no network calls.
Report PNGs are measurements; no reference bitmap is copied into the font.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor, as_completed
import json
import os
from pathlib import Path
import subprocess
import sys

from generate_font0_native import ROOT, sha256, read_json, write_json, build, refine_recipe, UPM


def run_probe(args, font, output, extra=()):
    env = os.environ.copy()
    if args.qt_bin:
        env["PATH"] = str(args.qt_bin) + os.pathsep + env.get("PATH", "")
    command = [str(args.probe), "--font", str(font), "--manifest", str(args.manifest),
               "--output-dir", str(output), "--no-hinting", *extra]
    result = subprocess.run(command, env=env, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if result.returncode:
        raise RuntimeError(f"Probe failed ({result.returncode}): {font}\n{result.stdout}\n{result.stderr}")
    return read_json(output / "report.json")


def grouped_scores(report):
    scores = defaultdict(lambda: {"differentPixels": 0, "unionPixels": 0, "comparisons": 0, "exact": 0, "missing": 0})
    for case in report["cases"]:
        role = case.get("role", "train")
        for glyph in case["glyphs"]:
            if glyph["oracleUnsupported"]:
                continue
            score = scores[role]
            score["differentPixels"] += glyph["differentPixels"]
            score["unionPixels"] += glyph["unionPixels"]
            score["comparisons"] += 1
            score["exact"] += glyph["differentPixels"] == 0 and not glyph["missingGlyph"]
            score["missing"] += bool(glyph["missingGlyph"])
    for score in scores.values():
        score["iou"] = 1 - score["differentPixels"] / max(1, score["unionPixels"])
    return dict(scores)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, default=ROOT / "build_agent_debug/examples/qtzpl_font0_probe.exe")
    parser.add_argument("--qt-bin", type=Path)
    parser.add_argument("--manifest", type=Path, default=ROOT / "tests/golden/font-glyphs/manifest.json")
    parser.add_argument("--sources", type=Path, default=ROOT / "third_party/font0-native/sources.json")
    parser.add_argument("--candidates", type=Path, default=ROOT / "build_font0_research/donors/candidates.json")
    parser.add_argument("--metrics", type=Path, default=ROOT / "tests/golden/font-glyphs/metrics.json")
    parser.add_argument("--output", type=Path, default=ROOT / "build_font0_research")
    parser.add_argument("--jobs", type=int, default=3)
    parser.add_argument("--force", action="store_true", help="Refit even if cached fitting inputs match")
    parser.add_argument("--select", nargs="+", help="Only fit the listed candidate IDs")
    parser.add_argument("--refine-outlines", action="store_true", help="Fit bounded, size-independent control-point edits after donor selection")
    args = parser.parse_args()
    if not 1 <= args.jobs <= 16:
        parser.error("--jobs must be between 1 and 16")
    args.probe = args.probe.resolve()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    if not args.qt_bin and os.name == "nt":
        cache = args.probe.parent.parent / "CMakeCache.txt"
        if cache.exists():
            for line in cache.read_text(encoding="utf-8").splitlines():
                if line.startswith("Qt6_DIR:PATH="):
                    args.qt_bin = Path(line.split("=", 1)[1]).parents[2] / "bin"
    candidates = read_json(args.candidates)["candidates"]
    if args.select:
        candidates = [item for item in candidates if item["id"] in args.select]
    if not candidates:
        parser.error("No candidates selected")
    manifest = read_json(args.manifest)
    # Freeze selection inputs. Validation pages can grow independently without
    # invalidating cached fits, but any training image change invalidates them.
    training = [case for case in manifest["cases"] if case.get("role", "train") == "train"]
    training_identity = {"cases": training, "unsupported": manifest["unsupportedCharacters"],
                         "images": {case["name"]: sha256(args.manifest.parent / (case["name"] + "-labelary-bitonal.png")) for case in training}}
    import hashlib
    train_hash = hashlib.sha256(json.dumps(training_identity, sort_keys=True).encode()).hexdigest()

    def fit(candidate):
        font = args.candidates.parent / candidate["file"]
        if sha256(font) != candidate["sha256"]:
            raise ValueError(f"Donor checksum mismatch: {candidate['id']}")
        output = args.output / "fits" / candidate["id"]
        identity = {"font": candidate["sha256"], "training": train_hash, "probe": sha256(args.probe)}
        cache = output / "cache.json"
        if args.force or not cache.exists() or read_json(cache) != identity or not (output / "fitted-adjustments.json").exists() or not (output / "report.json").exists():
            report = run_probe(args, font, output, ["--fit", "--role", "train", "--no-atlas-images"])
            write_json(cache, identity)
        else:
            report = read_json(output / "report.json")
        fitted = read_json(output / "fitted-adjustments.json")
        if fitted["fontSha256"] != candidate["sha256"] or report["fontSha256"] != candidate["sha256"]:
            raise ValueError(f"Fitting report does not match donor: {candidate['id']}")
        print(f"Fitted {candidate['id']}: {report['summary']}", flush=True)
        return candidate, fitted

    fits = []
    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        for future in as_completed([executor.submit(fit, candidate) for candidate in candidates]):
            fits.append(future.result())
    fits.sort(key=lambda entry: entry[0]["id"])
    recipes = {}
    rankings = {}
    for candidate, fitted in fits:
        for cp, result in fitted["glyphs"].items():
            if result.get("missingGlyph"):
                continue
            rankings.setdefault(cp, []).append({"donor": candidate["id"], "loss": result["loss"],
                                                "sourceOrigin": candidate.get("sourceOrigin", "upstream"),
                                                "differentPixels": result["differentPixels"],
                                                "unionPixels": result["unionPixels"], "samples": result["samples"],
                                                "adjustment": {key: result[key] for key in ("scaleX", "scaleY", "offsetX", "offsetY")}})
    from fontTools.ttLib import TTFont
    metrics = read_json(args.metrics) if args.metrics.exists() else {}
    measured = metrics.get("glyphs", {})
    for cp, options in sorted(rankings.items()):
        # On an exactly equal measured result prefer our explicit primitives:
        # they are easier to inspect and edit than an imported glyph outline.
        options.sort(key=lambda option: (option["loss"], option["sourceOrigin"] != "authored", option["donor"]))
        best = options[0]
        candidate = next(candidate for candidate in candidates if candidate["id"] == best["donor"])
        with TTFont(args.candidates.parent / candidate["file"]) as font:
            source_advance = font["hmtx"][font.getBestCmap()[int(cp[2:], 16)]][0] / UPM
        metric = measured.get(cp, {})
        recipes[cp] = {**best, "sourceId": candidate["sourceId"], "sourceSha256": candidate["sourceSha256"],
                       "sourceCodepoint": candidate.get("glyphAliases", {}).get(cp, cp),
                       "advanceEm": metric.get("advanceEm", source_advance * best["adjustment"]["scaleX"]),
                       "advanceSource": "Labelary raster measurement" if metric else "donor estimate; not validated",
                       "pointEdits": []}
    unsupported = set(manifest["unsupportedCharacters"])
    uncovered = sorted(set(manifest["characters"]) - unsupported - set(recipes))
    recipe = {"schemaVersion": 1, "status": "research; not accepted as pixel-exact Font 0",
              "license": "OFL-1.1", "family": "QtZpl Font Zero Research", "unitsPerEm": UPM,
              "trainingIdentitySha256": train_hash, "sourceManifestSha256": sha256(args.sources),
              "metricsFile": os.path.relpath(args.metrics, ROOT).replace(os.sep, "/") if args.metrics.exists() else None,
              "metricsSha256": sha256(args.metrics) if args.metrics.exists() else None,
              "candidatesManifestSha256": sha256(args.candidates), "glyphs": recipes,
              "oracleUnsupported": sorted(unsupported), "uncovered": uncovered,
              "sourceHistogram": dict(Counter(g["sourceId"] for g in recipes.values()))}
    recipe_path = args.output / "glyph-recipe.json"
    write_json(recipe_path, recipe)
    write_json(args.output / "donor-rankings.json", rankings)
    args.recipe = recipe_path
    font_path = args.output / "QtZplFontZeroResearch.ttf"
    original_output = args.output
    args.output = font_path
    build(args)
    args.output = original_output
    report = run_probe(args, font_path, args.output / "composite", ["--glyph-images"])
    summary = {"status": recipe["status"], "fontSha256": sha256(font_path), "glyphs": len(recipes),
               "donorFamilies": len(recipe["sourceHistogram"]), "sourceHistogram": recipe["sourceHistogram"],
               "uncovered": uncovered, "scores": grouped_scores(report)}
    if args.refine_outlines:
        run_probe(args, font_path, args.output / "outline-fit", ["--fit-outline", "--role", "train", "--no-atlas-images"])
        refined = refine_recipe(recipe, read_json(args.output / "outline-fit/fitted-adjustments.json"), font_path)
        refined_recipe = args.output / "glyph-recipe-refined.json"
        write_json(refined_recipe, refined)
        args.recipe = refined_recipe
        refined_font = args.output / "QtZplFontZeroResearch-Refined.ttf"
        args.output = refined_font
        build(args)
        args.output = original_output
        refined_report = run_probe(args, refined_font, args.output / "refined", ["--glyph-images"])
        summary["refined"] = {"fontSha256": sha256(refined_font), "changedGlyphs": refined["outlineRefinement"]["changedGlyphs"],
                              "scores": grouped_scores(refined_report)}
    write_json(args.output / "summary.json", summary)
    print(json.dumps(summary, ensure_ascii=False, indent=2), flush=True)


if __name__ == "__main__":
    main()
